// strikers-quest r44: voice chat in online matches (Lance: "enable voice chat within online matches
// (toggle-able)"). See xr_voice.cpp. Built with xr_quest.cpp in strikers_xr, Quest only.
//
// The microphone is coded with Opus in 20 ms frames, and the frames ride along on the lockstep's
// input messages (xr_net.cpp), which go out 30 times a second anyway: voice adds bytes to the room
// server's traffic but no messages, and messages are what its free plan counts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xrvoice {

// The player's switch: voice.txt in STRIKERS_HOME, "1" on (the launcher panel writes it too, and asks
// for the microphone when it turns it on). Off by default.
bool enabled();
void set_enabled(bool on);  // saved at once; an open match follows it at once

// Whether the app may use the microphone (Android's RECORD_AUDIO). Off: you still hear your friend.
bool mic_allowed();

// The online match (xr_net.cpp): voice runs from its first frame to its end.
void match_begin();
void match_end();

// Game thread, xr_net.cpp. The voice frames waiting to go, appended to an input message as
// [count u8] then per frame [seq u16][len u8][len bytes], keeping the whole message under maxTotal.
void append_outgoing(std::vector<uint8_t>& msg, size_t maxTotal);
// The friend's frames, from the same layout at the end of their input message.
void take_incoming(const uint8_t* data, size_t size);

}  // namespace xrvoice
