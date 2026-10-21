// SvR 2008 host resources for the game's own D3D textures and surfaces (see svr_resources.h).

#include "svr_resources.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"
#include "svr_frame_diag.h"

// src/native/crash_trace.cpp: hang watchdog, fed once per presented frame.
void SvrWatchdogHeartbeat();
// src/native/display_size.cpp.
bool SvrDisplaySize(uint32_t &w, uint32_t &h);
bool SvrOnSteamDeck();

REXCVAR_DEFINE_INT32(svr_render_scale, 0, "SvR",
                     "Internal resolution: 1 = 720p (original), 2 = 1440p, 3 = 4K (2160p), "
                     "6 = 8K (4320p), or a height: 480 (low-end PCs), 1080. 0 = auto: from the "
                     "display (1440p on 1080p/1440p screens, 4K on 4K screens), 720p on Steam "
                     "Deck and on built-in (laptop) graphics");

namespace bd::gpu {

// Host-side creation from re:Blue's CreateTexture/CreateSurface hooks (gpu/hooks/resource.cpp).
GuestTexture *SvrCreateHostTexture(u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                                   u32 format, u32 d3d_type);
GuestTexture *SvrCreateHostSurface(u32 width, u32 height, u32 format);

namespace {

std::mutex g_mutex;

// A host resource made for a guest surface or GPU-written texture, with the guest description it
// was made from. Road to WrestleMania unloads characters between scenes and reuses those guest
// objects for other textures (another wrestler's composited skin at another size): a host
// resource kept by address alone then showed the previous character's skin mixed into the new one.
struct HostTarget {
  GuestTexture *host = nullptr;
  u32 desc[2] = {};  // surface: size bits, format; texture: fetch dwords 1 (format, base), 2 (size)
};
std::unordered_map<u32, HostTarget> g_surfaces;  // D3DSurface VA -> host render target
std::unordered_map<u32, HostTarget> g_targets;   // texture VA -> GPU-written host texture

// The texture's GPUTEXTURE_FETCH_CONSTANT (D3DTexture+0x1C), as host-order dwords.
bool ReadFetch(u32 texture_va, rex::graphics::xenos::xe_gpu_texture_fetch_t &fetch) {
  const auto *texture = bd::mem::at<const D3DTexture>(texture_va);
  if (!texture)
    return false;
  for (int i = 0; i < 6; ++i)
    (&fetch.dword_0)[i] = u32(texture->Format.dword[i]);
  return fetch.type == rex::graphics::xenos::FetchConstantType::kTexture;
}

// Sets aside a host resource whose guest object now describes something else. It is not destroyed:
// these host-made targets are linked into the renderer's resolve bookkeeping, which the
// D3DResource_Destroy path doesn't expect for them (freeing one crashed in
// DrainResolveLinksLocked). Rare in practice (a guest object changing size or format), so the
// memory is simply kept.
std::vector<GuestTexture *> g_retired;
void RetireHost(GuestTexture *host) {
  if (!host)
    return;
  g_retired.push_back(host);
  BD_INFO("SvR: {} stale host target(s) set aside", g_retired.size());
}

// Whether g_targets' entry for texture_va still matches the texture there; a stale one is retired
// and removed. Caller holds g_mutex.
bool TargetCurrent(u32 texture_va, std::unordered_map<u32, HostTarget>::iterator it) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch) ||
      (fetch.dword_1 == it->second.desc[0] && fetch.dword_2 == it->second.desc[1]))
    return true;
  BD_INFO("SvR target 0x{:08X}: now {}x{} fmt {} (was another texture) -> rebuilt", texture_va,
          fetch.size_2d.width + 1, fetch.size_2d.height + 1, u32(fetch.format));
  RetireHost(it->second.host);
  g_targets.erase(it);
  return false;
}

}  // namespace

// Built-in graphics (Intel UHD / Iris Xe, the Radeon in Ryzen laptops) share the CPU's memory,
// power and heat: rendering above 720p there costs the steady 60.
bool OnBuiltInGraphics() {
  auto *device = Video::HostDevice();
  return device && (device->getDescription().type == plume::RenderDeviceType::INTEGRATED ||
                    device->getCapabilities().uma);
}

u32 SvrAutoRenderHeight() {
  static std::atomic<u32> s_auto{0};
  if (const u32 height = s_auto.load(std::memory_order_relaxed))
    return height;
  if (!Video::HostDevice())
    return 720;  // not decided before the graphics device exists
  u32 scale = 1;
  if (OnBuiltInGraphics()) {
    BD_INFO("SvR native: built-in graphics, auto resolution is 720p");
  } else if (!SvrOnSteamDeck()) {
    // The larger of the window and the display, so a window on a big screen still renders
    // sharp (downscaled at present); 720p multiples rounded to nearest: 1080/1440 -> 2, 2160 -> 3.
    // The Deck keeps 1x even when docked: its GPU holds 60 fps there.
    u32 w = 0, h = 0, dw = 0, dh = 0;
    if (!Output::RenderSize(w, h))
      h = 0;
    if (SvrDisplaySize(dw, dh))
      h = std::max(h, dh);
    if (h)
      scale = std::clamp(u32((h + 360u) / 720u), 1u, 4u);
  }
  s_auto.store(720 * scale, std::memory_order_relaxed);
  return 720 * scale;
}

u32 SvrRenderHeight() {
  static std::atomic<u32> s_height{0};
  if (const u32 height = s_height.load(std::memory_order_relaxed))
    return height;
  // svr_render_scale: a multiple of 720p (1..8; 6 = 8K) or a height (480, 1080...).
  const int configured = REXCVAR_GET(svr_render_scale);
  u32 height = configured <= 0  ? SvrAutoRenderHeight()
               : configured <= 8 ? 720u * u32(configured)
                                 : u32(std::clamp(configured, 360, 4320));
  height &= ~1u;  // even, so half sizes stay whole
  u32 expected = 0;
  if (s_height.compare_exchange_strong(expected, height)) {
    BD_INFO("SvR native: internal resolution {}x{} ({:.3g}x)", SvrScaled(1280), height,
            height / 720.0);
    return height;
  }
  return expected;
}

float SvrRenderScale() { return float(SvrRenderHeight()) / 720.0f; }

u32 SvrScaled(u32 guest) {
  return u32((u64(guest) * SvrRenderHeight() + 360u) / 720u);
}

GuestTexture *SvrResolveSurface(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(surface_va))
    return host;
  std::lock_guard lock(g_mutex);
  const auto *surface = bd::mem::at<const D3DSurface>(surface_va);
  if (!surface)
    return nullptr;
  // Size packing as decoded by D3DSurface_GetDesc (see gpu/d3d.h).
  const u32 bits = surface->SizeBits;
  const u32 format = surface->Format;
  if (auto it = g_surfaces.find(surface_va); it != g_surfaces.end()) {
    if (it->second.desc[0] == bits && it->second.desc[1] == format)
      return it->second.host;
    RetireHost(it->second.host);
    g_surfaces.erase(it);
  }
  const u32 width = (bits >> 18) + 1;
  const u32 height = ((bits >> 3) & 0x7FFF) + 1;
  SvrDiagTimer create(SvrDiag::kTargetCreate);
  GuestTexture *host = SvrCreateHostSurface(SvrScaled(width), SvrScaled(height), format);
  BD_INFO("SvR surface 0x{:08X}: {}x{} format 0x{:08X} -> host {}", surface_va, width, height,
          format, host ? "ok" : "FAILED");
  g_surfaces.emplace(surface_va, HostTarget{host, {bits, format}});
  return host;
}

GuestTexture *SvrResolveTarget(u32 texture_va) {
  if (!texture_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(texture_va))
    return host;
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va); it != g_targets.end() && TargetCurrent(texture_va, it))
    return it->second.host;
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch)) {
    g_targets.emplace(texture_va, HostTarget{});
    return nullptr;
  }
  const u32 width = fetch.size_2d.width + 1;
  const u32 height = fetch.size_2d.height + 1;
  const u32 levels = fetch.mip_max_level + 1;
  // ConvertGuestFormat maps a raw GPUTEXTUREFORMAT byte by its low 6 bits; the tag keeps a small
  // byte from matching one of re:Blue's D3DFormat values (k_8_8_8_8 = 6 = its kIndex32).
  SvrDiagTimer create(SvrDiag::kTargetCreate);
  GuestTexture *host = SvrCreateHostTexture(SvrScaled(width), SvrScaled(height), 1, levels, 0,
                                            0xC0000000u | u32(fetch.format), 0);
  BD_INFO("SvR target 0x{:08X}: {}x{} fmt {} levels {} -> host {}", texture_va, width, height,
          u32(fetch.format), levels, host ? "ok" : "FAILED");
  g_targets.emplace(texture_va, HostTarget{host, {fetch.dword_1, fetch.dword_2}});
  return host;
}

// The Xbox 360 resolves into main memory, and SvR reads some of it with the CPU: the
// Create-a-Superstar photo is drawn on the GPU, resolved, then read by the CPU from that memory
// (SvR 2010 does the same with edited outfits). Here a resolve only writes the host texture, so
// the CPU found whatever the memory held before (the photo came out as scrambled pixels). The
// result is read back into guest memory, which stalls for the GPU; a texture resolved again and
// again (shadows, the screen, effects redrawn every few frames) is skipped once seen in several
// frames running, so the wait happens for one-off captures and while loading, not during play.
void SvrWriteBackResolve(u32 texture_va, GuestTexture *dst) {
  struct Seen {
    u32 desc = 0;
    u64 last_frame = 0;
    u32 streak = 0;
  };
  constexpr u32 kPerFrameStreak = 3;  // resolved in this many frames running: every frame
  // Frames apart that still count as running: 2008 redraws some screen-size targets every 5 frames
  // or so (60 -> 48 fps when each was read back); a one-off capture isn't repeated within this.
  constexpr u64 kFrameGap = 30;
  static std::unordered_map<u32, Seen> seen;  // render thread only
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!dst || !ReadFetch(texture_va, fetch) ||
      (fetch.format != rex::graphics::xenos::TextureFormat::k_8_8_8_8 &&
       fetch.format != rex::graphics::xenos::TextureFormat::k_8_8_8_8_A))
    return;
  const u64 frame = SvrFrameIndex();
  Seen &s = seen[texture_va];
  const bool first = s.desc != fetch.dword_1 || s.last_frame == 0;
  if (first) {
    s = Seen{fetch.dword_1, frame, 0};
  } else if (s.last_frame != frame) {
    s.streak = s.last_frame + kFrameGap >= frame ? s.streak + 1 : 0;
    s.last_frame = frame;
  }
  if (s.streak >= kPerFrameStreak)
    return;
  const auto start = std::chrono::steady_clock::now();
  std::vector<u8> pixels;
  u32 w = 0, h = 0, row = 0;
  const bool ok = Video::ReadBackTexture(dst, pixels, w, h, row) &&
                  WriteGuestTextureRGBA8(texture_va, pixels.data(), w, h, row);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  static u32 repeats = 0;  // render thread only
  if (first)
    BD_INFO("SvR resolve 0x{:08X}: {}x{} swizzle 0x{:03X} endian {} copied back to guest memory "
            "{} ({:.1f} ms)",
            texture_va, fetch.size_2d.width + 1, fetch.size_2d.height + 1, u32(fetch.swizzle),
            u32(fetch.endianness), ok ? "ok" : "FAILED", ms);
  else if (++repeats <= 20 || repeats % 100 == 0)
    BD_INFO("SvR resolve 0x{:08X} copied back again (streak {}, {:.1f} ms; {} repeats)", texture_va,
            s.streak, ms, repeats);
}

namespace {

std::shared_mutex g_object_mutex;
std::unordered_map<u32, GuestShader *> g_shaders;  // guest shader object -> host
std::unordered_map<u64, GuestShader *> g_shaders_by_hash;
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;  // guest declaration -> host
std::unordered_map<u64, GuestVertexDeclaration *> g_decls_by_hash;

}  // namespace

void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel) {
  if (!shader_va || !function_va)
    return;
  const auto *container = bd::mem::at<const ShaderContainer>(function_va);
  if (!container)
    return;
  // Keyed like CreateShader's cache lookup (the whole container), plus the stage.
  const u32 length = u32(container->virtualSize) + u32(container->physicalSize);
  const u64 hash = XXH3_64bits(container, length) ^ (pixel ? 0x9E3779B97F4A7C15ull : 0);
  std::unique_lock lock(g_object_mutex);
  GuestShader *&host = g_shaders_by_hash[hash];
  if (!host)
    host = CreateShader(reinterpret_cast<const be_u32 *>(container),
                        pixel ? ResourceType::PixelShader : ResourceType::VertexShader);
  g_shaders[shader_va] = host;
}

GuestShader *SvrLookupShader(u32 shader_va) {
  if (!shader_va)
    return nullptr;
  std::shared_lock lock(g_object_mutex);
  auto it = g_shaders.find(shader_va);
  return it != g_shaders.end() ? it->second : nullptr;
}

void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va) {
  if (!decl_va || !elements_va)
    return;
  auto *elements = bd::mem::at<GuestVertexElement>(elements_va);
  if (!elements)
    return;
  // Elements up to the D3DDECL_END terminator (stream 0xFF), padding zeroed for a stable key.
  std::vector<GuestVertexElement> normalized;
  for (const auto *e = elements; normalized.size() < 64; ++e) {
    normalized.push_back(*e);
    normalized.back().padding = 0;
    if (u16(e->stream) == 0xFF)
      break;
  }
  const u64 hash =
      XXH3_64bits(normalized.data(), normalized.size() * sizeof(GuestVertexElement));
  std::unique_lock lock(g_object_mutex);
  GuestVertexDeclaration *&host = g_decls_by_hash[hash];
  if (!host)
    host = CreateVertexDeclaration(normalized.data());
  g_decls[decl_va] = host;
}

GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va) {
  if (!decl_va)
    return nullptr;
  std::shared_lock lock(g_object_mutex);
  auto it = g_decls.find(decl_va);
  return it != g_decls.end() ? it->second : nullptr;
}

namespace {

// A mirrored texture as last checked: the fetch constant and content it was built from.
struct MirrorCheck {
  u32 fetch[6] = {};
  u64 hash = 0;
  u64 sample = 0;
  u64 frame = 0;
  u32 stable = 0;  // full checks in a row that found the content unchanged
  GuestTexture *host = nullptr;
};
std::unordered_map<u32, MirrorCheck> g_mirror_checks;  // D3D texture VA -> last check
std::atomic<u64> g_frame{1};

// Hashing every bound texture in full each frame was ~20 MB a frame in a full arena, several ms
// on CPUs with smaller caches. A texture is hashed in full once every kFullCheckFrames frames
// (staggered by address) and spot-checked in between. The game rewrites a texture wholesale
// (Bink frames, menus reloading into reused memory), which the spot check catches at once; a
// partial rewrite it misses shows at the next full check, at most kFullCheckFrames later.
// A texture that changed is hashed in full every frame until two full checks in a row find it
// unchanged: the game may still have been writing it when it was copied (a Bink frame half
// decoded on another thread), and the rest of that write can land where no sample looks. Spot
// checks missed exactly that on slower CPUs (green video, stale crowd).
constexpr u64 kFullCheckFrames = 8;
constexpr u32 kStableBeforeSpotChecks = 2;
constexpr size_t kAlwaysFullBytes = 16 * 1024;
constexpr size_t kSamples = 16, kSampleBytes = 64;

u64 HashSource(const NativeMirrorSource &src) {
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  if (src.base_size)
    hash = XXH3_64bits_withSeed(src.base, src.base_size, hash);
  if (src.mips_size)
    hash = XXH3_64bits_withSeed(src.mips, src.mips_size, hash);
  return hash;
}

u64 SampleRange(const u8 *data, size_t size, u64 seed) {
  if (size <= kSamples * kSampleBytes)
    return XXH3_64bits_withSeed(data, size, seed);
  u8 buf[kSamples * kSampleBytes];
  const size_t step = (size - kSampleBytes) / (kSamples - 1);
  for (size_t i = 0; i < kSamples; ++i)
    std::memcpy(buf + i * kSampleBytes, data + i * step, kSampleBytes);
  return XXH3_64bits_withSeed(buf, sizeof(buf), seed);
}

u64 SampleSource(const NativeMirrorSource &src) {
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  if (src.base_size)
    hash = SampleRange(src.base, src.base_size, hash);
  if (src.mips_size)
    hash = SampleRange(src.mips, src.mips_size, hash);
  return hash;
}

}  // namespace

void SvrOnFrame() {
  SvrDiagEndFrame();
  SvrWatchdogHeartbeat();
  const u64 frame = g_frame.fetch_add(1, std::memory_order_relaxed);
  // Present rate in the log every 600 frames (pacing check).
  static auto last = std::chrono::steady_clock::now();
  if (frame % 600 == 0) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    BD_INFO("SvR native: {:.1f} presents/s", 600.0 / seconds);
  }
}

u64 SvrFrameIndex() { return g_frame.load(std::memory_order_relaxed); }

// The game writes texture memory with the CPU whenever it likes (every Bink movie frame, menus
// reloading into reused memory), so a mirror is checked against its source once per frame it's
// used and rebuilt when the fetch constant or the bytes changed. A texture that can't be mirrored
// is remembered the same way, so it isn't retried (or logged) until it changes.
GuestTexture *SvrResolveTexture(u32 texture_va) {
  if (!texture_va)
    return nullptr;
  SvrDiagTimer diag(SvrDiag::kTextureCheck);
  const u64 frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va);
      it != g_targets.end() && it->second.host && TargetCurrent(texture_va, it))
    return it->second.host;
  MirrorCheck &check = g_mirror_checks[texture_va];
  if (check.frame == frame)
    return check.host;
  NativeMirrorSource src;
  if (!GetNativeMirrorSource(texture_va, src)) {
    check.frame = frame;
    return check.host = nullptr;
  }
  const size_t bytes = src.base_size + src.mips_size;
  const u64 sample = SampleSource(src);
  const bool full_due = bytes <= kAlwaysFullBytes || check.stable < kStableBeforeSpotChecks ||
                        (frame + (texture_va >> 4)) % kFullCheckFrames == 0;
  if (check.frame != 0 && !full_due && sample == check.sample) {
    check.frame = frame;
    return check.host;
  }
  const u64 hash = HashSource(src);
  SvrDiagAdd(SvrDiag::kHashBytes, bytes);
  check.sample = sample;
  if (check.frame != 0 && check.hash == hash) {
    check.frame = frame;
    ++check.stable;
    return check.host;
  }
  check.stable = 0;
  {
    SvrDiagTimer mirror(SvrDiag::kTextureMirror);
    check.host = GetOrCreateNativeMirror(texture_va, 0);
  }
  check.hash = hash;
  check.frame = frame;
  return check.host;
}

}  // namespace bd::gpu
