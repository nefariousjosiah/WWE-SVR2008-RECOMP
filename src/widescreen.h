// Wide and tall screens (settings menu > Screen shape: 16:10, 21:9, 32:9). See widescreen.cpp.
#pragma once

namespace svr {

// The screen shape picked in the settings menu as a width / height ratio, or 0 for the game's own
// 16:9 (and for "stretch to fill").
double WideAspect();

// Whether the frame being drawn is a scene (match, entrance, cutscene: the game set up a scene
// camera within the last half second) that fills the shape. Menus stay 16:9, in the middle.
bool WideScene();

// How much wider (Wide) or taller (Tall) than 16:9 the scene is drawn: 1 when it isn't.
float WideFactor();
float TallFactor();

}  // namespace svr
