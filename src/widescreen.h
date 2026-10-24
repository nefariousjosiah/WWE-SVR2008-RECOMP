// Wide and tall screens (settings menu > Screen shape: 16:10, 21:9, 32:9). See widescreen.cpp.
#pragma once

namespace svr {

// The screen shape picked in the settings menu as a width / height ratio, or 0 for the game's own
// 16:9 (and for "stretch to fill").
double WideAspect();

// How much wider (Wide) or taller (Tall) than 16:9 the game is drawn: 1 when it isn't.
float WideFactor();
float TallFactor();

}  // namespace svr
