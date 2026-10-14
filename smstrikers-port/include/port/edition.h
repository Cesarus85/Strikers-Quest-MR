// strikers-quest r43: the game's Version, Classic or Plus (src/platform/edition.cpp). Lance, after
// alpha 1.2: "from 1.3 and up im basically gonna be adding on top of the game ... give the people an
// option as to what version theyd wanna play".
//
// Picked in the launcher panel (LobbyActivity "Version"), handed to the game as STRIKERS_EDITION
// (StrikersHome.prepare, like the game file) and fixed for the whole run.
//   Classic = alpha 1.2's game: no life-size goal scorer or its banner, no held goal replay, no
//             living crowd, goal roar or crowd placed round the stands.
//   Plus    = the default: all of those.
// Bug fixes, the frame-rate work, the big screen by hand and the paused crowd volume are in both.
// Both headsets of an online match must be on the same one (xr_net.cpp's hello checks).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = Plus (STRIKERS_EDITION unset or anything but "classic"), 0 = Classic. Read once. */
int PortPlusFeatures(void);

#ifdef __cplusplus
}
#endif
