/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(SVR_NATIVE_RENDERER)
REXCVAR_DECLARE(bool, svr_edge_blur);       // gpu/hooks/draw.cpp
REXCVAR_DECLARE(bool, svr_depth_of_field);  // gpu/hooks/draw.cpp
#endif

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, so the slot remembers the sampler it resolved for the fetch
// constant's sampler fields. Keyed on just those fields (DecodeFromFetch reads
// nothing else): the full constant also holds the texture's address and size,
// which change with every texture (a crowd frame binds hundreds) while the
// sampler state stays the same. Sampler heap slots are never reclaimed, so a
// cached index stays valid until device teardown.
struct SamplerSlotCache {
  u32 key = 0;
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

// The fetch constant fields DecodeFromFetch reads, packed.
u32 SamplerFieldsKey(const u32 fc[6]) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fc, sizeof(f));
  return u32(f.clamp_x) | u32(f.clamp_y) << 3 | u32(f.clamp_z) << 6 |
         u32(f.mag_filter) << 9 | u32(f.min_filter) << 11 |
         u32(f.mip_filter) << 13 | u32(f.border_color) << 15;
}

// The last vertex/pixel constant block uploaded on this command list, as the
// guest stored it. SvR uploads both blocks every draw (inline constant writes
// have no hook), but consecutive draws mostly share one or both, so an
// unchanged block keeps the binding it has instead of another 4 KiB copy.
struct StageConstantCache {
  alignas(64) u8 raw[256 * 16];
  bool valid = false;
  u64 pin = 0;  // which registers the upload overrode (0: none); part of the match
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  u64 epoch = 1;  // bumped by every ResetFrame
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[kTextureSlots];
  StageConstantCache vsConstants, psConstants;
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

void RecomputeShadowPcfScale(UploadState &s) {
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunk(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                                plume::RenderBufferFlag::VERTEX |
                                plume::RenderBufferFlag::INDEX |
                                plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    if constexpr (kFlushNaN) {
      const __m128i is_nan =
          _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
      v = _mm_andnot_si128(is_nan, v);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), v);
  }
#endif
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  ++s.epoch;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() {
  auto &s = upload_state();
  s.sharedBound = false;
  s.vsConstants.valid = false;
  s.psConstants.valid = false;
}

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

#if defined(SVR_NATIVE_RENDERER)
// SvR 2008's depth of field: its end-of-frame composites blend each pixel toward a blurred
// quarter-size copy of the frame by depth (the factor comes from g_f4DOFCalcInfo and
// g_f4DOFCalcInfo2; up to 0.55 in matches), which softens the crowd and the arena behind the
// wrestlers. With g_f4DOFCalcInfo and g_f4DOFCalcInfo2.z (the factor's floor) zero, every variant
// keeps the sharp frame. The match composite also runs the edge-blur filter (svr_edge_blur,
// hooks/draw.cpp) over the 3x3 taps g_f2SampleOffsets: zero offsets make every tap the pixel
// itself, so nothing reads as an edge. Registers from each shader's constant table.
struct SvrPostShader {
  u64 hash;
  u8 dof_reg;      // g_f4DOFCalcInfo; g_f4DOFCalcInfo2 is the next register
  u8 offsets_reg;  // g_f2SampleOffsets, 0: none
};
constexpr SvrPostShader kSvrPostShaders[] = {
    {0x07B19DE4D20E66C6ull, 13, 18},  // the match composite: edge blur, depth of field, bloom
    {0xF186A60B65C7C686ull, 13, 0},  {0xCB7943BE1FD89EC4ull, 13, 0},
    {0x1EF0E08FFC584AA1ull, 13, 0},  {0xE46003160C512987ull, 13, 0},
    {0x1F30F417D8BAFC3Dull, 13, 0},  {0xFE75275397BC63C3ull, 12, 0},
    {0x58B3B4BB16F7581Dull, 12, 0},
};

// The current pixel shader's entry when one of its effects is turned off, else null.
const SvrPostShader *SvrPostOverride() {
  const bool dof = REXCVAR_GET(svr_depth_of_field), edge = REXCVAR_GET(svr_edge_blur);
  if (dof && edge)
    return nullptr;
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  if (!ps || !ps->shaderCacheEntry)
    return nullptr;
  for (const SvrPostShader &e : kSvrPostShaders)
    if (e.hash == ps->shaderCacheEntry->hash)
      return (!dof || e.offsets_reg) ? &e : nullptr;
  return nullptr;
}

void PinSvrPostConstants(u8 *block, const SvrPostShader &e) {
  if (!REXCVAR_GET(svr_depth_of_field)) {
    auto *info = reinterpret_cast<float *>(block + e.dof_reg * 16);
    info[0] = info[1] = info[2] = info[3] = 0.0f;
    info[4 + 2] = 0.0f;  // g_f4DOFCalcInfo2.z
  }
  if (!REXCVAR_GET(svr_edge_blur) && e.offsets_reg) {
    auto *offsets = reinterpret_cast<float *>(block + e.offsets_reg * 16);
    offsets[0] = offsets[1] = 0.0f;
  }
}
#endif

namespace {
// True when the guest block equals the last one uploaded on this command list
// (the caller keeps that binding); otherwise remembers it for next time.
// (The D3D runtime's own dirty masks, m_Mask[0]/[1], can't replace this compare: over a match
// about 1.3% of draws had new constants with the mask clean.)
bool SameAsLastUpload(StageConstantCache &cache, u32 guest_va, u64 pin) {
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    cache.valid = false;
    return false;
  }
  if (cache.valid && cache.pin == pin &&
      std::memcmp(cache.raw, src, kConstantBlockBytes) == 0)
    return true;
  std::memcpy(cache.raw, src, kConstantBlockBytes);
  cache.pin = pin;
  cache.valid = true;
  return false;
}

bool PinsScreenUVScale() {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  return ps && ps->shaderCacheEntry && ps->shaderCacheEntry->hash == kBDBlurPSHash;
}
}  // namespace

ConstantAllocation UploadVertexShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  const u32 va = device_guest + offsetof(D3DDevice, vsFloatConstants);
  if (SameAsLastUpload(s.vsConstants, va, false))
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory) {
    s.vsConstants.valid = false;
    return {};
  }
  CopyByteSwap32FlushNaN(alloc.memory, va, kConstantBlockBytes);
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  const u32 va = device_guest + offsetof(D3DDevice, psFloatConstants);
  u64 pin = PinsScreenUVScale() ? kBDBlurPSHash : 0;
#if defined(SVR_NATIVE_RENDERER)
  const SvrPostShader *post = SvrPostOverride();
  if (post)
    pin = post->hash ^ (REXCVAR_GET(svr_depth_of_field) ? 1 : 0) ^ (REXCVAR_GET(svr_edge_blur) ? 2 : 0);
#endif
  if (SameAsLastUpload(s.psConstants, va, pin))
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory) {
    s.psConstants.valid = false;
    return {};
  }
  CopyByteSwap32FlushNaN(alloc.memory, va, kConstantBlockBytes);
  PinScreenUVScaleReg(alloc.memory);
#if defined(SVR_NATIVE_RENDERER)
  if (post)
    PinSvrPostConstants(alloc.memory, *post);
#endif
  return alloc;
}

// Per-slot entries of the shared block: slots 0-15 in the base arrays, SvR's 16-31 after them.
#if defined(SVR_NATIVE_RENDERER)
static u32 &Tex2DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture2DIndices[i] : c.texture2DIndicesHi[i - 16];
}
static u32 &Tex3DIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.texture3DIndices[i] : c.texture3DIndicesHi[i - 16];
}
static u32 &TexCubeIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.textureCubeIndices[i] : c.textureCubeIndicesHi[i - 16];
}
static u32 &SamplerIndex(SharedConstants &c, u32 i) {
  return i < 16 ? c.samplerIndices[i] : c.samplerIndicesHi[i - 16];
}
#else
static u32 &Tex2DIndex(SharedConstants &c, u32 i) { return c.texture2DIndices[i]; }
static u32 &Tex3DIndex(SharedConstants &c, u32 i) { return c.texture3DIndices[i]; }
static u32 &TexCubeIndex(SharedConstants &c, u32 i) { return c.textureCubeIndices[i]; }
static u32 &SamplerIndex(SharedConstants &c, u32 i) { return c.samplerIndices[i]; }
#endif

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (device+0x2FF0+slot*4)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  for (u32 i = 0; i < bd::gpu::kTextureSlots; ++i) {
    SamplerIndex(s.shared, i) = 0;
    Tex2DIndex(s.shared, i) = bd::gpu::kNullTexture2DDescriptorIndex;
    Tex3DIndex(s.shared, i) = bd::gpu::kNullTexture3DDescriptorIndex;
    TexCubeIndex(s.shared, i) = bd::gpu::kNullTextureCubeDescriptorIndex;

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        Tex3DIndex(s.shared, i) = tex->descriptorIndex;
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                    bd::gpu::kInvalidDescriptorIndex) {
          Tex2DIndex(s.shared, i) = tex->companion2D->descriptorIndex;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        TexCubeIndex(s.shared, i) = tex->descriptorIndex;
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        Tex2DIndex(s.shared, i) = tex->descriptorIndex;
        // BD static reflection cubes bind as a 2D atlas yet the water/glass
        // shader cube-fetches the slot, so publish the sliced TextureCube
        // companion so tfetchCube resolves a real cube, not the null cube.
        if (tex->companionCube && tex->companionCube->descriptorIndex !=
                                      bd::gpu::kInvalidDescriptorIndex) {
          TexCubeIndex(s.shared, i) = tex->companionCube->descriptorIndex;
        }
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        const u32 fc[6] = {
            u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
            u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
        };
        const u32 key = SamplerFieldsKey(fc);
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
            sc.key == key) {
          SamplerIndex(s.shared, i) = sc.sampler;
        } else {
          auto desc = DecodeFromFetch(fc);

          // Shell fur volumes encode shell depth in W, and X360-default WRAP
          // wraps a z=0 fetch's second tap to the tip slice and halves density,
          // so force CLAMP to keep both taps on the dense base slice.
          if (clamp3d) {
            desc.addressW = plume::RenderTextureAddressMode::CLAMP;
          }
          const u32 resolved = ResolveSlotLocked(desc);
          sc.key = key;
          sc.sampler = resolved;
          sc.aniso = aniso_now;
          sc.clamp3d = clamp3d;
          sc.valid = true;
          SamplerIndex(s.shared, i) = resolved;
        }
      }
    }
  }

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      s.shared.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  s.shared.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  s.shared.halfPixelOffsetX = 0.0f;
  s.shared.halfPixelOffsetY = 0.0f;
#if defined(SVR_NATIVE_RENDERER)
  // Host clip space is Vulkan's (pixel centres at +0.5; re:Blue's viewport puts NDC +1 at the top).
  s.shared.screenSpaceScaleX = 1.0f;
  s.shared.screenSpaceScaleY = 1.0f;
  s.shared.screenSpaceOffsetX = 0.0f;
  s.shared.screenSpaceOffsetY = 0.0f;
  // In guest pixels: the game's own viewport (host targets may be scaled up, the conversion to
  // clip space is the same).
  const float gx = device_p ? float(device_p->viewport.X) : 0.0f;
  const float gy = device_p ? float(device_p->viewport.Y) : 0.0f;
  const float gw = device_p ? float(device_p->viewport.Width) : 0.0f;
  const float gh = device_p ? float(device_p->viewport.Height) : 0.0f;
  if (device_guest && gw > 0.0f && gh > 0.0f) {
    const float w = gw;
    const float h = gh;
    // PA_CL_VTE_CNTL (device+0x294C, SetRenderState_ViewportEnable sub_8223EC88: 0x43F on,
    // 0x400 off). With the viewport transform off the vertex shader outputs window pixels.
    const u32 vte = bd::mem::load<u32>(device_guest + 0x294C);
    // PA_SU_VTX_CNTL.pix_center (device+0x29C0 bit 0, SetRenderState_HalfPixelOffset
    // sub_8223F040): clear means Direct3D 9 pixel centres, on integer coordinates.
    const bool d3d9_centres = (bd::mem::load<u32>(device_guest + 0x29C0) & 1u) == 0;
    const float centre = d3d9_centres ? 0.5f : 0.0f;
    if ((vte & 0x3Fu) == 0) {
      s.shared.screenSpaceScaleX = 2.0f / w;
      s.shared.screenSpaceScaleY = -2.0f / h;
      s.shared.screenSpaceOffsetX = (centre - gx) * 2.0f / w - 1.0f;
      s.shared.screenSpaceOffsetY = 1.0f - (centre - gy) * 2.0f / h;
    } else if (d3d9_centres) {
      // Half a pixel right and down, so D3D9-centred geometry samples and covers as on Xenos.
      s.shared.halfPixelOffsetX = 1.0f / w;
      s.shared.halfPixelOffsetY = -1.0f / h;
    }
  }
#endif
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  return alloc;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

ConstantAllocation AllocateUploadBytes(u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !size)
    return {};
  return Allocate(s, size, alignment);
}

u64 UploadEpoch() { return upload_state().epoch; }

} // namespace bd::gpu
