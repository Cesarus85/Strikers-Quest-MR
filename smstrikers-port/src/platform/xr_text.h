// strikers-quest r42: text panels for the headset, drawn on the CPU into RGBA8 images (sRGB,
// premultiplied alpha) that xr_quest.cpp shows as quad layers. Font: Luckiest Guy (Apache-2.0,
// extern/fonts), rasterized with stb_truetype.
#pragma once

#include <cstdint>

namespace xrtext {

// The goal banner: "GOAL SCORED BY:" over the scorer's name, on a rounded panel in the team colour
// (0xRRGGBB) with a gold border and stars. `rgba` holds w * h * 4 bytes. r43d: under the name, the
// scorer's goals and assists this game (goals < 0: no stats line), "HAT TRICK!" in gold with them
// when hatTrick. Laid out for w : h = 1024 : 560.
void goal_banner(uint8_t* rgba, int w, int h, const char* name, uint32_t teamRgb, int goals, int assists,
                 bool hatTrick);

// The crowd volume panel shown while the game is paused: a title, `steps` bar segments with `level`
// of them lit, and a hint line.
void crowd_slider(uint8_t* rgba, int w, int h, int level, int steps);

}  // namespace xrtext
