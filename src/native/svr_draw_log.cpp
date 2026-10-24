// Native renderer dev aid: everything one frame draws, in order.
//
//   SVR_DRAW_LOG=7000   (frame index, counted in presents; logs/game.log gets a [drawlog] line per
//                        draw, clear and resolve of that frame)
//   F9 in game          logs the next frame the same way (svr_drawlog_key)
//
// Per draw: the game's render target / depth surface and the host target they map to, viewport,
// scissor, shaders, and every bound texture (the game's D3D texture and its host counterpart).

#include "svr_draw_log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/resources.h"
#include "gpu/shaders/shader_constants.h"
#include "gpu/vertex_declaration.h"
#include "svr_resources.h"

namespace bd::gpu {
namespace {

u64 TargetFrame() {
  static const u64 frame = [] {
    const char *env = std::getenv("SVR_DRAW_LOG");
    return env ? std::strtoull(env, nullptr, 0) : 0ull;
  }();
  return frame;
}

// SetTexture (sub_82237868) keeps the bound D3D texture per slot at device + (slot + 3134) * 4.
constexpr u32 kBoundTextureTable = 3134u * 4u;

std::string Host(const GuestTexture *t) {
  if (!t)
    return "-";
  return fmt::format("{}x{} f{}", t->width, t->height, u32(t->format));
}

u64 ShaderHash(const GuestShader *s) {
  return s && s->shaderCacheEntry ? s->shaderCacheEntry->hash : 0ull;
}

}  // namespace

std::atomic<u64> g_key_frame{0};  // the frame F9 asked for

bool SvrDrawLogActive() {
  const u64 frame = SvrFrameIndex();
  const u64 target = TargetFrame();
  return (target && frame == target) || frame == g_key_frame.load(std::memory_order_relaxed);
}

void SvrDrawLogNextFrame() {
  const u64 next = SvrFrameIndex() + 1;
  g_key_frame.store(next, std::memory_order_relaxed);
  BD_INFO("[drawlog] logging frame {} (F9)", next);
}

void SvrDrawLogDraw(u32 device_guest, const char *name, u32 primitive, bool indexed, u32 count,
                    u32 start, i32 base) {
  if (!SvrDrawLogActive())
    return;
  static u32 s_index = 0;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  auto &s = state();
  std::string textures;
  for (u32 slot = 0; slot < kTextureSlots; ++slot) {
    const u32 va = bd::mem::load<u32>(device_guest + kBoundTextureTable + slot * 4u);
    if (!va && !s.textures[slot])
      continue;
    textures += fmt::format(" t{}=0x{:08X}[{}]", slot, va, Host(s.textures[slot]));
    // The fetch constant (D3DTexture+0x1C, 6 dwords): mip filter, min/max level, LOD bias.
    if (va && slot < 8) {
      if (const auto *t = bd::mem::try_at<const D3DTexture>(va))
        textures += fmt::format("{{fc {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}}}",
                                u32(t->Format.dword[0]), u32(t->Format.dword[1]),
                                u32(t->Format.dword[2]), u32(t->Format.dword[3]),
                                u32(t->Format.dword[4]), u32(t->Format.dword[5]));
    }
  }
  // Register shadows: the 0x2100 block at +0x28CC, the 0x2200 block at +0x2934 (one dword each).
  const auto reg = [&](u32 r) -> u32 {
    const u32 off = r >= 0x2200 ? 0x2934u + (r - 0x2200u) * 4u : 0x28CCu + (r - 0x2100u) * 4u;
    return bd::mem::load<u32>(device_guest + off);
  };
  const auto regf = [&](u32 r) {
    const u32 v = reg(r);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  if (const auto *decl = s.vertex_declaration) {
    std::string elements;
    for (u32 i = 0; i < decl->inputElementCount; ++i) {
      const auto &e = decl->inputElements[i];
      if (e.slotIndex == 15)
        continue;
      elements += fmt::format(" {}{}:s{}+{}:f{}", e.semanticName ? e.semanticName : "?",
                              e.semanticIndex, e.slotIndex, e.alignedByteOffset, u32(e.format));
    }
    BD_INFO("[drawlog]   decl{}", elements);
  }
  BD_INFO("[drawlog]   blendctl 0x{:08X} colorctl 0x{:08X} alpharef {:.4g} | host blend {} {}/{} "
          "alphatest {} threshold {:.4g} zfunc {} zwrite {}",
          reg(0x2201), reg(0x2202), regf(0x210E), s.pipelineState.alphaBlendEnable ? "on" : "off",
          u32(s.pipelineState.srcBlend), u32(s.pipelineState.destBlend),
          (s.pipelineState.specConstants & kSpecConstantAlphaTest) ? "on" : "off",
          Video::AlphaThreshold(), u32(s.pipelineState.zFunc),
          s.pipelineState.zWriteEnable ? "on" : "off");
  BD_INFO("[drawlog]   vte 0x{:X} clip 0x{:X} mode 0x{:X} depthctl 0x{:08X} colormask 0x{:X} "
          "ps-c13 {:.4g},{:.4g},{:.4g},{:.4g} ps-c14 {:.4g},{:.4g},{:.4g},{:.4g}",
          reg(0x2206), reg(0x2204), reg(0x2205), reg(0x2200), reg(0x2104),
          float(dev->psFloatConstants[13][0]), float(dev->psFloatConstants[13][1]),
          float(dev->psFloatConstants[13][2]), float(dev->psFloatConstants[13][3]),
          float(dev->psFloatConstants[14][0]), float(dev->psFloatConstants[14][1]),
          float(dev->psFloatConstants[14][2]), float(dev->psFloatConstants[14][3]));
  BD_INFO("[drawlog] #{} {} prim {} {} count {} start {} base {} | rt 0x{:08X}[{}] ds 0x{:08X}[{}] "
          "| vp {:.0f},{:.0f} {:.0f}x{:.0f} z {:.2f}-{:.2f} sc {},{}-{},{} | vs {:016X} ps {:016X} "
          "|{}",
          s_index++, name, primitive, indexed ? "indexed" : "", count, start, base,
          u32(dev->renderTargetShadow[0]), Host(s.render_target), u32(dev->depthStencilShadow),
          Host(s.depth_stencil), float(dev->viewport.X), float(dev->viewport.Y),
          float(dev->viewport.Width), float(dev->viewport.Height), float(dev->viewport.MinZ),
          float(dev->viewport.MaxZ), i32(dev->scissorRect.left), i32(dev->scissorRect.top),
          i32(dev->scissorRect.right), i32(dev->scissorRect.bottom), ShaderHash(s.vertex_shader),
          ShaderHash(s.pixel_shader), textures);
}

void SvrDrawLogResolve(u32 device_guest, u32 flags, u32 source_rect_va, u32 dest_va,
                       u32 dest_point_va) {
  if (!SvrDrawLogActive())
    return;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  std::string rect = "full";
  if (const auto *r = bd::mem::try_at<const D3DRect>(source_rect_va))
    rect = fmt::format("{},{}-{},{}", i32(r->left), i32(r->top), i32(r->right), i32(r->bottom));
  std::string point = "0,0";
  if (const auto *p = bd::mem::try_at<const be_i32>(dest_point_va))
    point = fmt::format("{},{}", i32(p[0]), i32(p[1]));
  BD_INFO("[drawlog] resolve flags 0x{:X} from rt 0x{:08X} rect {} -> 0x{:08X} at {}", flags,
          dev ? u32(dev->renderTargetShadow[0]) : 0u, rect, dest_va, point);
}

void SvrDrawLogVertices(u32 data_va, u32 count, u32 stride) {
  if (!SvrDrawLogActive() || !data_va || !stride)
    return;
  for (u32 v = 0; v < count && v < 4; ++v) {
    std::string line;
    for (u32 k = 0; k < stride / 4 && k < 8; ++k)
      line += fmt::format(" {:08X}", bd::mem::load<u32>(data_va + v * stride + k * 4));
    BD_INFO("[drawlog]   v{}:{}", v, line);
  }
}

void SvrDrawLogClear(u32 device_guest, u32 count, u32 rects_va, u32 flags, u32 color) {
  if (!SvrDrawLogActive())
    return;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  std::string rects;
  for (u32 i = 0; i < count && i < 8; ++i) {
    if (const auto *r = bd::mem::try_at<const D3DRect>(rects_va + i * 16u))
      rects += fmt::format(" {},{}-{},{}", i32(r->left), i32(r->top), i32(r->right),
                           i32(r->bottom));
  }
  BD_INFO("[drawlog] clear flags 0x{:X} color 0x{:08X} rt 0x{:08X} rects {}{}", flags, color,
          dev ? u32(dev->renderTargetShadow[0]) : 0u, count, rects);
}

}  // namespace bd::gpu
