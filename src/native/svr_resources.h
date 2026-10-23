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

// Wide and tall screens (src/widescreen.cpp). In a frame that fills a screen shape other than
// 16:9, the game's 1280x720 targets (its scene and depth, resolves of them, the front buffer) are
// drawn through a second host counterpart SvrFrameShape() times the usual width (wide) or height
// (tall); every other target, and every target in a 16:9 frame, keeps its usual one. The shape is
// taken once per frame so a frame never mixes the two.
void SvrFrameShape(float &wide, float &tall);
// The factors a host target was made with (1, 1 for every target but those).
void SvrTargetShape(const GuestTexture *host, float &wide, float &tall);
// A depth-tested (3D) draw into a shaped target: the scene is still being drawn. A frame starts
// the shape when the game set up a scene camera (widescreen.cpp); it keeps it while the scene is
// drawn even without one, as behind the pause menu, and drops it on the first frame without.
void SvrNoteSceneDraw();

// Where the current draw's guest pixels land in its host target: host = guest * scale + offset
// (the internal resolution, the target's shape, and for the HUD on a wide target its 16:9 middle).
struct SvrPixelMap {
  float scale_x = 1.0f, scale_y = 1.0f, offset_x = 0.0f, offset_y = 0.0f;
};
void SvrSetPixelMap(const SvrPixelMap &map);
const SvrPixelMap &SvrCurrentPixelMap();

// Internal resolution, as the height the guest's 720 lines become: host render targets and
// resolve textures are SvrRenderHeight() / 720 times the guest's size (480p and 1080p are
// fractions), and viewports/scissors are scaled to match. Set with svr_render_scale (0 = auto, a
// multiple 1..8 of 720p, or a height such as 480 or 1080). Fixed for the session once the first
// render target exists.
u32 SvrRenderHeight();
// The scale as a factor (SvrRenderHeight() / 720), for viewports and sampling offsets.
float SvrRenderScale();
// A guest size or coordinate at the internal resolution, rounded to the nearest pixel: every edge
// rounds the same way, so rectangles that share an edge on the guest still share it here.
u32 SvrScaled(u32 guest);
// What svr_render_scale = 0 (Auto) picks on this PC, as a height: 720 on Steam Deck and built-in
// graphics, otherwise from the screen (1440 at 1080p/1440p, 2160 at 4K). Shown by the settings
// menu.
u32 SvrAutoRenderHeight();
// Frames presented so far, plus 1 (never 0); per-frame mirror checks compare against it.
u64 SvrFrameIndex();

// Shaders and vertex declarations stay the game's own D3D objects; each one the game creates is
// given a host counterpart (shared between objects with the same microcode / elements).
void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel);
GuestShader *SvrLookupShader(u32 shader_va);
void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va);
GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va);

}  // namespace bd::gpu
