/**
 * @file    gpu/hooks/draw.cpp
 * @brief   Guest draw calls, and the EDRAM resolve / tiling calls that bracket
 *          them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#if defined(SVR_NATIVE_RENDERER)
#include "svr_frame_diag.h"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#if defined(SVR_NATIVE_RENDERER)
#include "svr_draw_log.h"
#include "svr_geometry.h"

#if defined(SVR_NATIVE_RENDERER)
REXCVAR_DEFINE_BOOL(svr_edge_blur, false, "SvR",
                    "SvR 2008's own edge-blur filter (the console's anti-aliasing): off keeps the image "
                    "sharp; the higher internal resolution already smooths edges");
// Applied in gpu/constant_buffers.cpp (the depth-of-field factor of the end-of-frame composites).
REXCVAR_DEFINE_BOOL(svr_depth_of_field, false, "SvR",
                    "SvR 2008's depth of field, which blurs the crowd and arena behind the wrestlers: "
                    "off keeps the whole picture sharp");
#endif
#include "svr_hook.h"
#include "svr_resources.h"
#endif
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/shaders/shader_cache.h"

namespace {

namespace xe = rex::graphics::xenos;

#if defined(SVR_NATIVE_RENDERER)
// How a BeginVertices draw was fitted to a shaped (wider or taller) target, SvrFitUpVertices.
enum class SvrUpFit : u8 {
  kNone,     // not fitted: the viewport fills the target (3D, or not a shaped target)
  kCentred,  // in its vertices: kept in shape in the 16:9 middle, and cut off at its edges
  kSpread,   // in its vertices: reaching the target's edges
  kInset,    // drawn in a part of the screen: that part's viewport kept in shape in the middle
};
#endif

struct DrawArgs {
  bool indexed = false;
  bool is_up = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
#if defined(SVR_NATIVE_RENDERER)
  SvrUpFit svrFit = SvrUpFit::kNone;
#endif
};

#if defined(SVR_NATIVE_RENDERER)
// Wide and tall screens (src/widescreen.cpp): how 2D (BeginVertices without depth, positions in the
// game's 1280x720 pixels) is fitted to a shaped target. It is done to the uploaded copy of its
// vertices, with the viewport over the whole target: the rasterizer clips to the viewport's box, so
// a viewport kept to the 16:9 middle could never show what the game drew past the screen's edge.
// - Panels, text and icons keep their shape and stay in the middle.
// - A quad reaching an edge of the screen (a full-width bar) or running past it (a side panel) is
//   anchored to that edge: its vertices out there move out to the target's edge and the ones inside
//   stay put, so it still meets what it met on screen and runs on to the real edge.
//   A detailed picture running past an edge (the title screen's photo) keeps its shape instead and
//   shows what the game drew past the 16:9 edge.
// - A quad covering the screen that doesn't read the scene is a backdrop: SvR's backgrounds are
//   pictures, so they get half their widening as a stretch and half as an even zoom, which trims a
//   little of their top and bottom. A full-screen pass over the scene stays exactly on it.
// 3D fills the target: its camera was given the screen's shape, so it shows more of the world.
// Whether a quad draws a detailed picture: more than 256 texels of its texture (slot 0) along an
// axis, at under 2 screen pixels each (the title screen's photo). A panel or a bar is a small piece
// of a UI sheet blown up.
bool SvrDetailedPicture(const u8 *verts, u32 count, u32 stride, float width, float height) {
  const auto &s = bd::gpu::state();
  const auto *decl = s.vertex_declaration;
  const auto *tex = s.textures[0];
  if (!decl || !tex || !tex->width || !tex->height)
    return false;
  for (u32 e = 0; e < decl->inputElementCount; ++e) {
    const auto &el = decl->inputElements[e];
    if (el.slotIndex != 0 || el.semanticIndex != 0 || !el.semanticName ||
        std::strcmp(el.semanticName, "TEXCOORD") != 0 ||
        el.format != plume::RenderFormat::R32G32_FLOAT || el.alignedByteOffset + 8 > stride)
      continue;
    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    for (u32 i = 0; i < count; ++i) {
      const auto *uv = reinterpret_cast<const float *>(verts + i * stride + el.alignedByteOffset);
      if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
        return false;
      u0 = std::min(u0, uv[0]), u1 = std::max(u1, uv[0]);
      v0 = std::min(v0, uv[1]), v1 = std::max(v1, uv[1]);
    }
    constexpr float kTexels = 256.0f, kBlownUp = 2.0f;
    const float texels_x = (u1 - u0) * float(tex->width), texels_y = (v1 - v0) * float(tex->height);
    return (texels_x > kTexels && width < kBlownUp * texels_x) ||
           (texels_y > kTexels && height < kBlownUp * texels_y);
  }
  return false;
}

// A screen whose 2D is a few small pieces in one corner (the "Loading" indicator) has them moved to
// the real corner of a wide (or tall) screen. Decided from the previous frame's centred 2D, which
// such a screen repeats: at most kFewPieces draws, all in one quarter of the screen. side_x and
// side_y: -1 left/top, 1 right/bottom, 0 stay in the middle.
void SvrCornerSide(float min_x, float max_x, float min_y, float max_y, int &side_x, int &side_y) {
  constexpr u32 kFewPieces = 12;
  static std::mutex s_mutex;
  static u64 s_frame = ~0ull;
  static u32 s_count = 0;
  static float s_min_x = 0.0f, s_max_x = 0.0f, s_min_y = 0.0f, s_max_y = 0.0f;
  static int s_side_x = 0, s_side_y = 0;
  std::lock_guard lock(s_mutex);
  const u64 frame = bd::gpu::SvrFrameIndex();
  if (frame != s_frame) {
    const bool few = s_frame + 1 == frame && s_count && s_count <= kFewPieces;
    const int x = s_max_x <= 640.0f ? -1 : s_min_x >= 640.0f ? 1 : 0;
    const int y = s_max_y <= 360.0f ? -1 : s_min_y >= 360.0f ? 1 : 0;
    s_side_x = few && x && y ? x : 0;
    s_side_y = few && x && y ? y : 0;
    s_frame = frame;
    s_count = 0;
    s_min_x = s_min_y = 1e9f;
    s_max_x = s_max_y = -1e9f;
  }
  ++s_count;
  s_min_x = std::min(s_min_x, min_x), s_max_x = std::max(s_max_x, max_x);
  s_min_y = std::min(s_min_y, min_y), s_max_y = std::max(s_max_y, max_y);
  side_x = s_side_x;
  side_y = s_side_y;
}

SvrUpFit SvrFitUpVertices(u32 device_guest, u32 primitive_type, u8 *verts, u32 count,
                          u32 stride) {
  if (!verts || !count || stride < 8)
    return SvrUpFit::kNone;
  const auto &s = bd::gpu::state();
  float wide = 1.0f, tall = 1.0f;
  bd::gpu::SvrTargetShape(s.render_target ? s.render_target : s.depth_stencil, wide, tall);
  if (wide == 1.0f && tall == 1.0f)
    return SvrUpFit::kNone;
  // RB_DEPTHCONTROL (register 0x2200, device +0x2934): z_enable is bit 1.
  if (bd::mem::load<u32>(device_guest + 0x2934) & 2u)
    return SvrUpFit::kNone;
  const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest);
  if (!dev)
    return SvrUpFit::kNone;
  const auto &vp = dev->viewport;
  if (float(vp.X) != 0.0f || float(vp.Y) != 0.0f || float(vp.Width) < 1280.0f ||
      float(vp.Height) < 720.0f)
    return SvrUpFit::kInset;

  // Uploaded already byte-swapped: the position is two host floats at the front of each vertex.
  const auto pos = [verts, stride](u32 i) { return reinterpret_cast<float *>(verts + i * stride); };
  float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
  for (u32 i = 0; i < count; ++i) {
    const float *p = pos(i);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]))
      return SvrUpFit::kNone;
    min_x = std::min(min_x, p[0]), max_x = std::max(max_x, p[0]);
    min_y = std::min(min_y, p[1]), max_y = std::max(max_y, p[1]);
  }
  // One primitive, not a batch (a line of text spans the screen without being a background).
  const auto prim = static_cast<xe::PrimitiveType>(primitive_type);
  const bool one = prim == xe::PrimitiveType::kTriangleStrip ||
                   prim == xe::PrimitiveType::kTriangleFan || count <= 4;
  constexpr float kEdge = 8.0f;  // authored extents this close to the screen edge reach it
  bool fill_x = one && min_x <= kEdge && max_x >= 1280.0f - kEdge;
  bool fill_y = one && min_y <= kEdge && max_y >= 720.0f - kEdge;
  // A piece of a background picture drawn in parts (the title screen's picture is two halves over
  // a full-screen one): taller than the screen, running past its side and reaching its middle. It
  // is fitted like the picture it belongs to, or the parts would come apart.
  if (one && min_y < -kEdge && max_y > 720.0f + kEdge &&
      (min_x < -kEdge || max_x > 1280.0f + kEdge) && min_x <= 640.0f + kEdge &&
      max_x >= 640.0f - kEdge)
    fill_x = fill_y = true;
  if (fill_x && fill_y) {
    // One reading a shaped target (the scene) is a pass over the scene. Slot 0's texture counts
    // however the game bound it (the final passes over a match bind its copy past SetTexture's
    // table); the other slots by that table, since a slot keeps what it last held. SetTexture
    // (sub_82237868) keeps the bound D3D texture per slot at device + (slot + 3134) * 4.
    float shaped_x = 1.0f, shaped_y = 1.0f;
    bd::gpu::SvrTargetShape(s.textures[0], shaped_x, shaped_y);
    bool backdrop = shaped_x == 1.0f && shaped_y == 1.0f;
    for (u32 slot = 0; slot < bd::gpu::kTextureSlots && backdrop; ++slot)
      backdrop = !bd::gpu::SvrShapedTexture(bd::mem::load<u32>(device_guest + (3134u * 4u) + slot * 4u));
    if (backdrop) {
      // Fills the long side; the zoom half of the widening also scales the other, about the middle.
      const float zoom = std::sqrt(std::max(wide, tall));
      for (u32 i = 0; i < count; ++i) {
        float *p = pos(i);
        if (wide != 1.0f)
          p[1] = 360.0f + (p[1] - 360.0f) * zoom;
        else
          p[0] = 640.0f + (p[0] - 640.0f) * zoom;
      }
    }
    return SvrUpFit::kSpread;
  }

  // A quad running past an edge: a panel or a bar is anchored to it, a detailed picture keeps its
  // shape and shows what the game drew past the 16:9 edge (SvrDetailedPicture). A small one (a
  // letter of scrolling text leaving the screen) is only cut off there, as on a 16:9 screen.
  constexpr float kAnchorSize = 64.0f;
  const bool big_x = one && max_x - min_x >= kAnchorSize;
  const bool big_y = one && max_y - min_y >= kAnchorSize;
  const bool past_x = big_x && (min_x < -kEdge || max_x > 1280.0f + kEdge);
  const bool past_y = big_y && (min_y < -kEdge || max_y > 720.0f + kEdge);
  const bool picture = (past_x || past_y) &&
                       SvrDetailedPicture(verts, count, stride, max_x - min_x, max_y - min_y);
  const bool left = big_x && (fill_x || (!picture && min_x < -kEdge));
  const bool right = big_x && (fill_x || (!picture && max_x > 1280.0f + kEdge));
  const bool top = big_y && (fill_y || (!picture && min_y < -kEdge));
  const bool bottom = big_y && (fill_y || (!picture && max_y > 720.0f + kEdge));
  // The band on each side of the 16:9 screen, in the game's pixels.
  const float pad_x = 640.0f * (wide - 1.0f), pad_y = 360.0f * (tall - 1.0f);
  const bool overscan = (wide != 1.0f && past_x) || (tall != 1.0f && past_y);
  int side_x = 0, side_y = 0;
  if (!overscan && !(wide != 1.0f && (left || right)) && !(tall != 1.0f && (top || bottom)))
    SvrCornerSide(min_x, max_x, min_y, max_y, side_x, side_y);
  bool anchored = side_x != 0 || side_y != 0;
  for (u32 i = 0; i < count; ++i) {
    float *p = pos(i);
    float x = p[0] + float(side_x) * pad_x, y = p[1] + float(side_y) * pad_y;
    if (wide != 1.0f && left && x <= kEdge)
      x -= pad_x, anchored = true;
    else if (wide != 1.0f && right && x >= 1280.0f - kEdge)
      x += pad_x, anchored = true;
    if (tall != 1.0f && top && y <= kEdge)
      y -= pad_y, anchored = true;
    else if (tall != 1.0f && bottom && y >= 720.0f - kEdge)
      y += pad_y, anchored = true;
    // Into the viewport over the whole target, with the 16:9 screen in its middle.
    p[0] = (x + pad_x) / wide;
    p[1] = (y + pad_y) / tall;
  }
  return anchored || overscan ? SvrUpFit::kSpread : SvrUpFit::kCentred;
}
#endif

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    return plume::RenderPrimitiveTopology::TRIANGLE_FAN;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    // Unmapped X360 primitive type (e.g. kRectangleList). Drawing as a
    // triangle list is almost certainly wrong, so warn rather than fail
    // silently.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN("MapPrimitiveType: unmapped X360 primitive type {} drawn as "
              "TRIANGLE_LIST - rect/unknown topology not implemented",
              prim);
    }
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const char *name,
                  const DrawArgs &args = {}) {
#if defined(REXGLUE_ENABLE_PROFILING)
  // Per-draw pass/shader attribution, only formatted while a profiler is
  // connected (TRACY_ON_DEMAND).
  char zone_name[64] = "Draw";
  if (BD_PROFILER_CONNECTED()) {
    const auto *ps = bd::gpu::state().pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    std::snprintf(zone_name, sizeof(zone_name), "Draw pass=%u ps=%016llX",
                  bd::gpu::CurrentRenderPassId(),
                  static_cast<unsigned long long>(ps_hash));
  }
  // CPU zone only: per-draw GPU timestamps serialize the GPU and poison every
  // GPU number in the capture. Coarse GPU cost comes from the per-frame zones.
  BD_CPU_ZONE_DYN(zone_name);
#endif
  // One lock across the whole recording sequence: loader threads record texture
  // uploads and Present records under the same mutex, and the per-frame command
  // list they all write is single-producer.
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDiagTimer diag_draw(bd::gpu::SvrDiag::kDraw);
#endif
  auto &s = bd::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  bd::gpu::Video::OpenCommandListLocked();

#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogDraw(device_guest, name, primitive_type, args.indexed,
                          args.vertexOrIndexCount,
                          args.indexed ? args.startIndex : args.startVertex,
                          args.baseVertexIndex);
  // Dev bisection switch: SVR_SKIP_DRAWS bitmask, 1 indexed, 2 DrawVertices, 4 BeginVertices (UP),
  // 8 any draw into a render target other than R8G8B8A8, 16 indexed with DHEN3N normals (lit
  // meshes), 32 indexed without, 64 unlit indexed skinned (real BLENDINDICES), 128 unlit indexed
  // static.
  {
    static const u32 skip_mask = [] {
      const char *env = std::getenv("SVR_SKIP_DRAWS");
      return env ? u32(std::strtoul(env, nullptr, 0)) : 0u;
    }();
    if (skip_mask) {
      const bool up = args.is_up;
      const auto *decl = s.pipelineState.vertexDeclaration;
      const bool lit = decl && decl->hasDhen3nNormal;
      bool skinned = false;
      if (decl) {
        for (u32 i = 0; i < decl->inputElementCount; ++i) {
          const auto &e = decl->inputElements[i];
          if (e.slotIndex != 15 && e.semanticName &&
              std::strcmp(e.semanticName, "BLENDINDICES") == 0)
            skinned = true;
        }
      }
      if (((skip_mask & 1) && args.indexed) || ((skip_mask & 2) && !args.indexed && !up) ||
          ((skip_mask & 4) && up) ||
          ((skip_mask & 8) && s.pipelineState.renderTargetFormat !=
                                  plume::RenderFormat::R8G8B8A8_UNORM) ||
          ((skip_mask & 16) && args.indexed && lit) ||
          ((skip_mask & 32) && args.indexed && !lit) ||
          ((skip_mask & 64) && args.indexed && !lit && skinned) ||
          ((skip_mask & 128) && args.indexed && !lit && !skinned))
        return;
    }
  }
  // SvR 2008's edge-blur post pass (pixel shader 0xF6E6390C10B93944; the same filter as SvR 2010's): a 3x3
  // Sobel edge test that replaces every edge pixel with the average of its 9 neighbours. On the
  // console that softened jaggies at 720p; here its 1-pixel 720p offsets are 2+ pixels at the
  // internal resolution, so it blurs hair, tattoos, ropes and the crowd while the supersampling
  // already smooths edges. Skipping the draw leaves the scene as rendered.
  if (!REXCVAR_GET(svr_edge_blur) && s.pixel_shader && s.pixel_shader->shaderCacheEntry &&
      s.pixel_shader->shaderCacheEntry->hash == 0xF6E6390C10B93944ull)
    return;
  // Vulkan has no "no index buffer" state: an indexed draw with none bound, or reaching past the
  // end of the bound one, can hang the GPU. Such a draw is skipped (and counted in the log).
  if (args.indexed && primitive_type != 13) {
    const u32 index_size = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
    const u64 end = (u64(args.startIndex) + args.vertexOrIndexCount) * index_size;
    if (s.index_view.buffer.ref == nullptr || end > s.index_view.size) {
      static std::atomic<u32> s_skipped{0};
      const u32 k = s_skipped.fetch_add(1, std::memory_order_relaxed);
      if (k < 16 || (k & (k - 1)) == 0)
        BD_WARN("[draw] SvR: skipped indexed draw #{} ({}): {} count={} startI={} ib size={}", k,
                s.index_view.buffer.ref ? "past end of index buffer" : "no index buffer", name,
                args.vertexOrIndexCount, args.startIndex, s.index_view.size);
      return;
    }
  }
#endif

#if defined(SVR_NATIVE_RENDERER)
  // SvR's SetViewport isn't hooked: the viewport the game's draw uses is the one its D3D keeps in
  // the device (floats at +0x3160; SetRenderTarget/SetDepthStencilSurface reset it to the surface).
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest)) {
    const auto &vp = dev->viewport;
    // Host targets are SvrRenderScale() times the guest's size (a fraction at 480p and 1080p),
    // and a shaped target (wide and tall screens) that again wider or taller: 3D fills it, 2D is
    // fitted in its vertices (SvrFitUpVertices).
    const float k = bd::gpu::SvrRenderScale();
    const auto *target = s.render_target ? s.render_target : s.depth_stencil;
    float wide = 1.0f, tall = 1.0f;
    bd::gpu::SvrTargetShape(target, wide, tall);
    bd::gpu::SvrPixelMap map{k * wide, k * tall, 0.0f, 0.0f};
    // RB_DEPTHCONTROL (register 0x2200, device +0x2934): z_enable is bit 1. 3D drawn into a shaped
    // target is squeezed if its camera still has the 16:9 shape (gpu/constant_buffers.cpp).
    const bool depth_tested = (bd::mem::load<u32>(device_guest + 0x2934) & 2u) != 0;
    bd::gpu::SvrSetViewSqueeze(depth_tested ? 1.0f / wide : 1.0f, depth_tested ? 1.0f / tall : 1.0f);
    bd::gpu::SvrPixelMap scissor = map;
    if (args.svrFit != SvrUpFit::kNone && target) {
      // 2D fitted in its vertices (SvrFitUpVertices): the game's scissor is placed as its 16:9
      // screen is, in the middle, and one of the whole screen opens the whole target only to a quad
      // reaching the target's edges.
      scissor = {k, k, (float(target->width) - float(bd::gpu::SvrScaled(1280))) * 0.5f,
                 (float(target->height) - float(bd::gpu::SvrScaled(720))) * 0.5f,
                 args.svrFit == SvrUpFit::kSpread};
      if (args.svrFit == SvrUpFit::kInset)
        map = scissor;
    }
    bd::gpu::SvrSetPixelMap(scissor);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x,
                                         float(vp.X) * map.scale_x + map.offset_x);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y,
                                         float(vp.Y) * map.scale_y + map.offset_y);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width,
                                         float(vp.Width) * map.scale_x);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.height,
                                         float(vp.Height) * map.scale_y);
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.minDepth,
                                         float(vp.MinZ));
    bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.maxDepth,
                                         float(vp.MaxZ));
    s.dirtyStates.scissorRect = s.dirtyStates.scissorRect || s.dirtyStates.viewport;
  }
#endif

  // PSO key includes topology, so set it before any flush.
  bd::gpu::Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));

  if (!bd::gpu::Video::BindDrawFramebufferLocked()) {
    return;
  }
#if defined(SVR_NATIVE_RENDERER)
  // Wide and tall screens: a shaped target this frame hasn't cleared gets the bands outside its
  // 16:9 middle cleared (svr_resources.h SvrTakeBandClear), so a screen that only draws the middle
  // shows black there, not what an earlier screen left.
  if (auto *rt = s.render_target; rt && s.command_list) {
    float wide = 1.0f, tall = 1.0f;
    bd::gpu::SvrTargetShape(rt, wide, tall);
    if ((wide != 1.0f || tall != 1.0f) && bd::gpu::SvrTakeBandClear(rt)) {
      const i32 w = i32(rt->width), h = i32(rt->height);
      const i32 pad_x = (w - i32(bd::gpu::SvrScaled(1280))) / 2;
      const i32 pad_y = (h - i32(bd::gpu::SvrScaled(720))) / 2;
      const plume::RenderRect bands[2] = {
          wide != 1.0f ? plume::RenderRect(0, 0, pad_x, h) : plume::RenderRect(0, 0, w, pad_y),
          wide != 1.0f ? plume::RenderRect(w - pad_x, 0, w, h)
                       : plume::RenderRect(0, h - pad_y, w, h)};
      s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f), bands, 2);
    }
  }
#endif
  if (!bd::gpu::Video::FlushRenderStateLocked(device_guest)) {
    return; // FlushRenderState logs its own reason
  }

  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;

  if (primitive_type == 13) {
    u32 quads = args.vertexOrIndexCount / 4;
    const u32 max_quads = bd::gpu::Video::QuadlistMaxQuads();
    if (quads > max_quads)
      quads = max_quads;
    const auto *ib = bd::gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      const i32 base = args.indexed ? args.baseVertexIndex
                                    : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, base, 0);
      // The expansion IB replaced the tracked guest IB on the command list, and
      // without re-dirtying the next indexed draw reads quad pattern indices.
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    // No IB bound on an indexed draw: FlushRenderState skipped setIndexBuffer
    // and D3D12 fires EXECUTION WARNING #211, every index reads as 0,
    // geometry collapses, scene goes black.
    if (s.index_view.buffer.ref == nullptr) {
      static std::atomic<u32> s_no_ib{0};
      const u32 k = s_no_ib.fetch_add(1, std::memory_order_relaxed);
      if (k < 16) {
        BD_WARN("[draw] indexed draw with no IB bound: #{} {} count={} "
                "startI={} baseV={}",
                k, name, args.vertexOrIndexCount, args.startIndex,
                args.baseVertexIndex);
      }
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
}

// bdBuildQuadVertices assembles every glyph into this one buffer, and unlike
// bdPrim it divides each position by the 2D basis on the way in, so text
// arrives in the unit square while sprites arrive in canvas pixels.
constexpr u32 kTextQuadBatchEA = 0x82DBECF0;

// Which edges of a quad's UV rect the inset is allowed to move.
enum class UVEdges {
  All,      // every edge, for a batch whose cells are known to abut
  CellSeam, // only edges sitting on an interior texel boundary
};

void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized);
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges);
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride);

#if defined(SVR_NATIVE_RENDERER)
bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData, u32 vertexCount,
                             u32 vertexStride, u32 device_guest = 0,
                             SvrUpFit *fit = nullptr) {
#else
bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData,
                             u32 vertexCount, u32 vertexStride) {
#endif
  if (!pVertexData || !vertexCount || !vertexStride)
    return false;
  const u32 totalSize = vertexCount * vertexStride;
  auto alloc = bd::gpu::UploadGuestBytesByteSwap32(pVertexData, totalSize,
                                                   /*alignment=*/4);
  if (!alloc.memory)
    return false;
#if !defined(SVR_NATIVE_RENDERER)
  // Blue Dragon's 2D canvas fit-ups (its text batch address, sprite UV insets); SvR's UI vertices
  // are drawn as the game wrote them.
  const bool text_batch = pVertexData == kTextQuadBatchEA;
  FitDesignCanvasVertices(alloc.memory, primitiveType, vertexCount,
                          vertexStride, text_batch);
  if (text_batch)
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::All);
  else if (IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::CellSeam);
#else
  if (fit)
    *fit = SvrFitUpVertices(device_guest, primitiveType, alloc.memory, vertexCount, vertexStride);
#endif
  bd::gpu::Video::SetVertexStream(0, alloc.ref, alloc.size, vertexStride);
  return true;
}

// Zooming an event sprite about an off-canvas pivot retreats the authored art
// inside the frame and lets the power-of-two padding take the edge, which X360
// TV overscan hid. Pull the sampled UV back until the art edge meets the
// frame edge. Sibling of bdMotionBlurQuadInsetHook in gpu/hooks/tweaks.cpp.
constexpr u32 kQuadVertices = 4;

// An overshoot an artist could hide had to fit inside title-safe overscan. Past
// that, the art fills its canvas and the UV means what it says.
constexpr float kOverscanFraction = 0.05f;

// bdPrimPushVertex2D's layout. bdDrawRectPrimitive shares the stride but puts
// the color where u sits here, and submits nothing but QUADLIST or LINESTRIP,
// so the primitive type is what tells the two apart.
struct ScreenSpriteVertex {
  be_f32 x;
  be_f32 y;
  be_f32 u;
  be_f32 v;
  be_u32 color;
};
static_assert(sizeof(ScreenSpriteVertex) == 0x14);

// Stride tells the prim system's three vertex layouts apart. The two 2D ones
// lead with a canvas position, the 3D one with a world position.
constexpr u32 kPrimVertex2DDualTexStride = 0x1C;

bool Is2DPrimStride(u32 stride) {
  return stride == sizeof(ScreenSpriteVertex) ||
         stride == kPrimVertex2DDualTexStride;
}

// bdPrimPushVertex2D writes x,y,u,v,color and always submits a strip, while
// bdDrawRectPrimitive shares the stride but puts the color where u sits and
// submits QUADLIST or LINESTRIP. Matching all three tells the two apart.
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride) {
  return static_cast<xe::PrimitiveType>(primitiveType) ==
             xe::PrimitiveType::kTriangleStrip &&
         vertexCount == kQuadVertices &&
         vertexStride == sizeof(ScreenSpriteVertex);
}

// uv runs linearly from uv_lo at pos_lo to uv_hi at pos_hi. Yields the uv_hi
// that puts art_uv on the frame edge, or nothing when the quad already keeps
// the padding off screen.
std::optional<float> InsetSampledSpan(float pos_lo, float pos_hi, float uv_lo,
                                      float uv_hi, float frame_extent,
                                      float art_uv) {
  const float span = pos_hi - pos_lo;
  const float reach = frame_extent - pos_lo;
  if (span <= 0.0f || uv_hi <= uv_lo || uv_lo >= art_uv)
    return std::nullopt;
  // Only a quad covering the whole frame can put padding on screen.
  if (pos_lo > 0.0f || pos_hi < frame_extent)
    return std::nullopt;
  const float at_edge = uv_lo + reach * (uv_hi - uv_lo) / span;
  if (at_edge <= art_uv || at_edge > art_uv * (1.0f + kOverscanFraction))
    return std::nullopt;
  return uv_lo + (art_uv - uv_lo) * span / reach;
}

void InsetOverscanScreenSprite(u32 pVertexData, u32 primitiveType,
                               u32 vertexCount, u32 vertexStride) {
  if (!IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    return;

  const auto *tex = bd::gpu::state().textures[0];
  if (!tex)
    return;
  const auto tex_w = static_cast<float>(tex->width);
  const auto tex_h = static_cast<float>(tex->height);
  if (tex_w <= bd::gpu::kDesignCanvasWidth ||
      tex_h <= bd::gpu::kDesignCanvasHeight)
    return;

  auto *v = bd::mem::at<ScreenSpriteVertex>(pVertexData);
  if (!v)
    return;

  // Strip order is top-left, bottom-left, top-right, bottom-right, so u is
  // shared down each column and v across each row.
  if (auto inset_u = InsetSampledSpan(v[0].x, v[2].x, v[0].u, v[2].u,
                                      bd::gpu::kDesignCanvasWidth,
                                      bd::gpu::kDesignCanvasWidth / tex_w)) {
    v[2].u = *inset_u;
    v[3].u = *inset_u;
  }
  if (auto inset_v = InsetSampledSpan(v[0].y, v[1].y, v[0].v, v[1].v,
                                      bd::gpu::kDesignCanvasHeight,
                                      bd::gpu::kDesignCanvasHeight / tex_h)) {
    v[1].v = *inset_v;
    v[3].v = *inset_v;
  }
}

// Guest UV rects land on exact texel boundaries, so a fraction of a texel of
// slack is float noise rather than an authored position.
constexpr float kTexelBoundaryTolerance = 0.01f;

// An atlas packs its cells flush against each other, so a UV edge on a texel
// boundary with texture on both sides is a cell seam. An edge at 0 or 1 has no
// neighbor to reach across, and CLAMP already returns the border texel there.
bool OnCellSeam(float uv, float extent) {
  if (uv <= 0.0f || uv >= 1.0f)
    return false;
  const float texel = uv * extent;
  return std::abs(texel - std::round(texel)) < kTexelBoundaryTolerance;
}

// At any scale but 1:1 a bilinear tap at a quad edge reaches past it, so an
// edge sitting on a cell seam returns the neighbor cell: the next glyph's ink,
// the orange copy of a gauge behind the gray one, the far half of a nine slice.
// Pull the edge half a source texel inward, which keeps every tap inside its
// own cell at any scale and still puts the 1:1 case on texel centers.
//
// UVEdges::All moves both edges whether or not they measure onto a boundary,
// for the glyph batch, whose cells are known to abut. CellSeam moves only the
// edges that do. That is what leaves a quad spanning a whole texture, the frame
// composite above all, sampling its own texel grid untouched.
//
// Applied to the uploaded copy, after the byte swap, so guest memory is
// untouched.
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges) {
  if (vertexStride != sizeof(ScreenSpriteVertex))
    return;
  const auto *tex = bd::gpu::state().textures[0];
  if (!tex || !tex->width || !tex->height)
    return;
  const float extent[2] = {static_cast<float>(tex->width),
                           static_cast<float>(tex->height)};

  for (u32 base = 0; base + kQuadVertices <= vertexCount;
       base += kQuadVertices) {
    auto uv = [verts, vertexStride, base](u32 i) {
      return reinterpret_cast<float *>(verts +
                                       (base + i) * size_t{vertexStride}) +
             2;
    };
    for (u32 axis = 0; axis < 2; ++axis) {
      const float inset = 0.5f / extent[axis];
      float lo = uv(0)[axis], hi = lo;
      for (u32 i = 1; i < kQuadVertices; ++i) {
        lo = std::min(lo, uv(i)[axis]);
        hi = std::max(hi, uv(i)[axis]);
      }
      if (hi - lo <= 2.0f * inset)
        continue;
      const bool move_lo =
          edges == UVEdges::All || OnCellSeam(lo, extent[axis]);
      const bool move_hi =
          edges == UVEdges::All || OnCellSeam(hi, extent[axis]);
      // bdBuildQuadVertices copies the rect's two u and two v values verbatim
      // into the corners, so comparing against the extremes recovers which
      // corner each vertex is regardless of winding or mirroring.
      for (u32 i = 0; i < kQuadVertices; ++i) {
        float &c = uv(i)[axis];
        if (move_lo && c == lo)
          c = lo + inset;
        else if (move_hi && c == hi)
          c = hi - inset;
      }
    }
  }
}

// Authored extents this far apart still count as the same canvas edge.
constexpr float kCanvasEdgeTolerance = 8.0f;

bool IsOnePrimitive(u32 primitiveType, u32 vertexCount) {
  switch (static_cast<xe::PrimitiveType>(primitiveType)) {
  case xe::PrimitiveType::kTriangleStrip:
  case xe::PrimitiveType::kTriangleFan:
  case xe::PrimitiveType::kLineStrip:
    return true;
  default:
    return vertexCount <= 4;
  }
}

// Scales one 2D draw about the canvas center. It has to be the geometry rather
// than a shrunk viewport: the rasterizer clips to the NDC box before the
// viewport transform, so a backdrop reaching for the surface edge would be cut
// off at exactly the rect it was trying to escape.
//
// Applied to the uploaded copy, so guest memory is untouched. 'normalized' says
// the draw arrived already divided by the 2D basis, as the text batch is.
void FitDesignCanvasVertices(u8 *verts, u32 primitiveType, u32 vertexCount,
                             u32 vertexStride, bool normalized) {
  if (!verts || !bd::gpu::Video::DesignCanvasDrain() ||
      !Is2DPrimStride(vertexStride) || !vertexCount)
    return;
  const float kx = bd::gpu::Output::DesignScaleX();
  const float ky = bd::gpu::Output::DesignScaleY();
  if (kx == 1.0f && ky == 1.0f)
    return;

  const float canvas_x = normalized ? 1.0f : bd::gpu::kDesignCanvasWidth;
  const float canvas_y = normalized ? 1.0f : bd::gpu::kDesignCanvasHeight;
  const float tol_x = kCanvasEdgeTolerance * canvas_x / bd::gpu::kDesignCanvasWidth;
  const float tol_y =
      kCanvasEdgeTolerance * canvas_y / bd::gpu::kDesignCanvasHeight;

  // Uploaded already byte-swapped, so position is two host floats at the front
  // of each vertex.
  auto pos = [verts, vertexStride](u32 i) {
    return reinterpret_cast<float *>(verts + i * vertexStride);
  };

  float min_x = 999999.0f, min_y = 999999.0f;
  float max_x = -999999.0f, max_y = -999999.0f;
  for (u32 i = 0; i < vertexCount; ++i) {
    const float *p = pos(i);
    min_x = std::min(min_x, p[0]);
    min_y = std::min(min_y, p[1]);
    max_x = std::max(max_x, p[0]);
    max_y = std::max(max_y, p[1]);
  }

  // Covering the canvas, not matching it: a menu's ground is often authored
  // well past the edges, and scaling one of those inward opens a gap at the
  // very edge it was oversized to reach.
  const bool one = IsOnePrimitive(primitiveType, vertexCount);
  const bool covers_x = one && min_x <= tol_x && max_x >= canvas_x - tol_x;
  const bool covers_y = one && min_y <= tol_y && max_y >= canvas_y - tol_y;
  const bool flush_x = one && (min_x <= tol_x || max_x >= canvas_x - tol_x);
  const bool flush_y = one && (min_y <= tol_y || max_y >= canvas_y - tol_y);
  const bool spans_x = covers_x || (covers_y && flush_x);
  const bool spans_y = covers_y || (covers_x && flush_y);
  const float scale_x = spans_x ? 1.0f : kx;
  const float scale_y = spans_y ? 1.0f : ky;
  if (scale_x == 1.0f && scale_y == 1.0f)
    return;

  const float mid_x = canvas_x * 0.5f;
  const float mid_y = canvas_y * 0.5f;
  for (u32 i = 0; i < vertexCount; ++i) {
    float *p = pos(i);
    p[0] = mid_x + (p[0] - mid_x) * scale_x;
    p[1] = mid_y + (p[1] - mid_y) * scale_y;
  }
}

u32 D3DDevice_DrawVerticesUP_hook(u32 device_guest, u32 primitiveType,
                                  u32 vertexCount, u32 pVertexData,
                                  u32 vertexStride) {
  InsetOverscanScreenSprite(pVertexData, primitiveType, vertexCount,
                            vertexStride);
  const bool ok =
      UploadAndBindUpVertices(primitiveType, pVertexData, vertexCount,
                              vertexStride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = 0;
  DispatchDraw(device_guest, primitiveType, "DrawVerticesUP", args);
  return 0;
}

// The streaming draw-up pair. Their recompiled bodies drive the PM4 ring buffer
// reblue stubs out, so an unhooked BeginVertices spins forever in
// D3DDevice_RingBufferFlush. EndVertices copies synchronously, so the grow-only
// scratch is free again for the next BeginVertices.
struct {
  u32 va = 0;       // guest VA of the scratch buffer, 0 until first use
  u32 capacity = 0; // bytes currently allocated
} g_begin_vertices_scratch;

struct {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data_va = 0; // 0 when nothing is pending to draw
} g_begin_vertices_pending;

u32 D3DDevice_BeginVertices_hook(u32 device_guest, u32 primitiveType,
                                 u32 vertexCount, u32 vertexStride) {
  g_begin_vertices_pending = {};
  const u64 size =
      static_cast<u64>(vertexCount) * static_cast<u64>(vertexStride);
  // Nothing to draw: return null so the caller skips its memcpy + EndVertices
  // (Visual__DrawVerticesUP gates both on a non-null BeginVertices result).
  if (size == 0 || size > 0xFFFFFFFFull)
    return 0;

  auto *memory = REX_KERNEL_MEMORY();
  if (g_begin_vertices_scratch.capacity < size) {
    // Safe to free the old block here: EndVertices copies the bytes out
    // synchronously, so nothing references the previous scratch by now.
    const u32 new_capacity = static_cast<u32>(size);
    const u32 va = memory->SystemHeapAlloc(new_capacity, 0x20);
    if (!va)
      return 0;
    if (g_begin_vertices_scratch.va)
      memory->SystemHeapFree(g_begin_vertices_scratch.va);
    g_begin_vertices_scratch.va = va;
    g_begin_vertices_scratch.capacity = new_capacity;
  }

  g_begin_vertices_pending.device = device_guest;
  g_begin_vertices_pending.primitive_type = primitiveType;
  g_begin_vertices_pending.vertex_count = vertexCount;
  g_begin_vertices_pending.stride = vertexStride;
  g_begin_vertices_pending.data_va = g_begin_vertices_scratch.va;
  return g_begin_vertices_scratch.va;
}

u32 D3DDevice_EndVertices_hook(u32 /*device_guest*/) {
  const auto p = g_begin_vertices_pending;
  g_begin_vertices_pending = {}; // consume: a stray EndVertices must not redraw
  if (!p.data_va || !p.vertex_count)
    return 0;

#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogVertices(p.data_va, p.vertex_count, p.stride);
  SvrUpFit fit = SvrUpFit::kNone;
  const bool ok = UploadAndBindUpVertices(p.primitive_type, p.data_va, p.vertex_count, p.stride,
                                         p.device, &fit);
#else
  const bool ok = UploadAndBindUpVertices(p.primitive_type, p.data_va,
                                         p.vertex_count, p.stride);
#endif
  DrawArgs args{};
  args.is_up = ok;
#if defined(SVR_NATIVE_RENDERER)
  args.svrFit = ok ? fit : SvrUpFit::kNone;
#endif
  args.vertexOrIndexCount = p.vertex_count;
  args.startVertex = 0;
  DispatchDraw(p.device, p.primitive_type, "BeginVertices", args);
  return 0;
}

// All five params must be marshaled: a 4-arg signature drops the real
// IndexCount in r7 and submits zero count draws.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device_guest, u32 primitiveType,
                                       u32 baseVertexIndex, u32 startIndex,
                                       u32 indexCount) {
#if defined(SVR_NATIVE_RENDERER)
  // Streams and indices exactly as SvR's D3D left them in the device for this draw (its own
  // SetStreamSource/SetIndices are not observed any more).
  bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/true);
#endif
  DrawArgs args{};
  args.indexed = true;
  args.vertexOrIndexCount = indexCount;
  args.baseVertexIndex = static_cast<i32>(baseVertexIndex);
  args.startIndex = startIndex;
  DispatchDraw(device_guest, primitiveType, "DrawIndexedVertices", args);
  return 0;
}

u32 D3DDevice_Resolve_hook(u32 device_guest, u32 Flags, u32 pSourceRect,
                           u32 pDestTexture, u32 pDestPoint, u32 DestLevel,
                           u32 DestSliceOrFace, u32 /*pClearColor*/,
                           u32 /*ClearZHi*/, u32 /*ClearZLo*/,
                           u32 /*ClearStencil*/, u32 /*pParameters*/) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrDrawLogResolve(device_guest, Flags, pSourceRect, pDestTexture, pDestPoint);
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_Resolve: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
  bd::gpu::Video::TrackResolveSource(Flags, dst, DestLevel, DestSliceOrFace);
  bd::gpu::Video::ResolveRtToTexture(dst);
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrWriteBackResolve(pDestTexture, dst);
#endif
  return 0;
}

u32 D3DDevice_BeginTiling_hook(u32 /*device_guest*/, u32 /*Flags*/,
                               u32 /*Count*/, u32 /*pTileRects*/,
                               mapped_f32 pClearColor, f64 ClearZ,
                               u32 /*z_gpr_slot*/, u32 ClearStencil) {
  u32 color_argb = 0;
  if (const be_f32 *color_vec = pClearColor) {
    const auto pack = [](float v) -> u32 {
      return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    color_argb = (pack(color_vec[3]) << 24) | (pack(color_vec[0]) << 16) |
                 (pack(color_vec[1]) << 8) | (pack(color_vec[2]) << 0);
  }
  bd::gpu::Video::Clear(bd::gpu::kClearAll, color_argb, float(ClearZ),
                        ClearStencil);
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrNoteTargetCleared(bd::gpu::state().render_target);
#endif
  return 0;
}

u32 D3DDevice_EndTiling_hook(u32 /*device_guest*/, u32 ResolveFlags,
                             u32 /*pResolveRects*/, u32 pDestTexture,
                             u32 /*pClearColor*/, u32 /*ClearZ*/,
                             u32 /*ClearStencil*/, u32 /*pParameters*/) {
  if (!pDestTexture) {
    return 0;
  }
#if defined(SVR_NATIVE_RENDERER)
  auto *dst = bd::gpu::SvrResolveTarget(pDestTexture);
#else
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
#endif
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_EndTiling: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
  bd::gpu::Video::TrackResolveSource(ResolveFlags, dst);
  bd::gpu::Video::ResolveRtToTexture(dst);
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrWriteBackResolve(pDestTexture, dst);
#endif
  return 0;
}

u32 D3DDevice_DrawVertices_hook(u32 device_guest, u32 primitiveType,
                                u32 startVertex, u32 vertexCount) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrBindDrawGeometry(device_guest, /*indexed=*/false);
#endif
  DrawArgs args{};
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = startVertex;
  DispatchDraw(device_guest, primitiveType, "DrawVertices", args);
  return 0;
}

#if defined(SVR_NATIVE_RENDERER)
// SvR: the game's BeginVertices hands out space in its own command buffer; the vertices it writes
// there are drawn when EndVertices is reached, before the game's EndVertices submits them.
void SvrBeginVerticesObserved(PPCContext &args, u32 data_va, u8 *) {
  g_begin_vertices_pending = {};
  if (!data_va)
    return;
  g_begin_vertices_pending.device = args.r3.u32;
  g_begin_vertices_pending.primitive_type = args.r4.u32;
  g_begin_vertices_pending.vertex_count = args.r5.u32;
  g_begin_vertices_pending.stride = args.r6.u32;
  g_begin_vertices_pending.data_va = data_va;
}
#endif

} // namespace

#if defined(SVR_NATIVE_RENDERER)
// SvR: the renderer draws from the state the game's draw call is about to consume, then the game's
// own D3D function runs (see src/native/svr_hook.h).
#if !defined(SVR_NO_DRAW_VERTICES)  // SvR 2009 links no DrawVertices (svr_d3d_names.h)
SVR_HOOK_BEFORE(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
#endif
SVR_HOOK_BEFORE(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
SVR_HOOK_RESULT(D3DDevice_BeginVertices, SvrBeginVerticesObserved);
SVR_HOOK_BEFORE(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
SVR_HOOK_AFTER(D3DDevice_Resolve, D3DDevice_Resolve_hook);
#else
REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook);
REX_HOOK(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook);
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook);
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook);
#endif
