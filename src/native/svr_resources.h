// SvR 2008: host resources for the game's own D3D textures and surfaces.
//
// With the null GPU, SvR's D3D library creates real guest resources (headers + physical memory)
// itself; the native renderer gives each a host counterpart the first time it's used. Surfaces
// become host render targets; resolve destinations (including the front buffer) become host
// textures the GPU writes; any other texture is mirrored from guest memory (native_texture_mirror).

#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct GuestTexture;
struct GuestShader;
struct GuestVertexDeclaration;

// Guest D3DSurface (render or depth target) -> host render target.
GuestTexture *SvrResolveSurface(u32 surface_va);
// Guest texture that the GPU writes (Resolve destination, front buffer) -> host texture.
GuestTexture *SvrResolveTarget(u32 texture_va);
// After a resolve into texture_va (host texture dst): copies the result back into guest memory,
// for the game reading it with the CPU, unless the texture is resolved every frame.
void SvrWriteBackResolve(u32 texture_va, GuestTexture *dst);
// Guest texture bound for sampling -> host texture (a resolve target, else a mirror).
GuestTexture *SvrResolveTexture(u32 texture_va);
// Called once per presented frame (mirror checks are per frame).
void SvrOnFrame();

// Internal resolution as a multiple of the guest's (1280x720): host render targets and resolve
// textures are this many times larger, and viewports/scissors are scaled to match. Set with
// svr_render_scale (0 = from the window height: 2 at 1440p, 3 at 4K, 1 at 720p/800p). Fixed for
// the session once the first render target exists.
u32 SvrRenderScale();
// What svr_render_scale = 0 (Auto) picks on this PC: 1 on Steam Deck and built-in graphics,
// otherwise from the screen (2 at 1080p/1440p, 3 at 4K). Shown by the settings menu.
u32 SvrAutoRenderScale();
// Frames presented so far, plus 1 (never 0); per-frame mirror checks compare against it.
u64 SvrFrameIndex();

// Shaders and vertex declarations stay the game's own D3D objects; each one the game creates is
// given a host counterpart (shared between objects with the same microcode / elements).
void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel);
GuestShader *SvrLookupShader(u32 shader_va);
void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va);
GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va);

}  // namespace bd::gpu
