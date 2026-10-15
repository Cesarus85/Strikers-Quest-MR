// strikers-quest r46: RetroAchievements in the game (Lance: "THEIR strikers achievement set"), with
// rcheevos (MIT), RA's own client library: the set on retroachievements.org for Super Mario Strikers,
// checked against the game's memory every game frame, unlocks sent to RA, and a pop-up in the headset.
// Casual (softcore) only; hardcore is for clients RA has approved. The launcher panel logs in
// (RetroAch.java); this side starts only when it finds that login, and the game is a disc image.
// Built with xr_quest.cpp in strikers_xr; Quest only.
#pragma once

#include <cstdint>
#include <vector>

namespace xrra {

constexpr uint32_t kToastW = 1024, kToastH = 300;  // the pop-up's picture (xrtext::achievement_toast)

// Once per headset loop, on the game thread, after the game frame if one ran: starts the first time
// round (when the panel has a login), hands rcheevos the server's answers and, after a game frame,
// has it check the achievements against that frame's memory (ra_memory.h).
void frame(bool ranGameFrame);

// The next pop-up's picture (kToastW x kToastH, RGBA8 sRGB, premultiplied), each one once. False
// while none is ready; asking also starts drawing the next one waiting.
bool take_toast(std::vector<uint8_t>& rgba);

void shutdown();

}  // namespace xrra
