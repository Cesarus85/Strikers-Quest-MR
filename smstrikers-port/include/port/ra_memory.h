// strikers-quest r46: RetroAchievements' view of GameCube memory (src/platform/ra_memory.cpp).
#ifndef PORT_RA_MEMORY_H
#define PORT_RA_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once per game frame, before rcheevos evaluates: rebuilds the console images from the game's live
// objects. Safe before they exist (menus, boot): their bytes then read as 0.
void PortRaMemoryFrame(void);

// rcheevos' read callback, GameCube map: RA address A is console 0x80000000 + A. Gives what a US
// console (G4QE01) holds there, big-endian, for every address the set reads; 0 for anything else.
// Reads only the images, never a game object, so it cannot crash. Returns num_bytes.
uint32_t PortRaReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes);

// Debug: every address the set reads, as of the last PortRaMemoryFrame, one
// "address size value meaning" line each, to check a headset run against the meanings.
void PortRaMemoryDump(const char* path);

// Why this run cannot count toward achievements (a debug setting that changes the match behind the
// settings the set checks: the command channel, unlock-all, forced teams/stadium/AI skill), or NULL.
const char* PortRaSetupTampered(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_RA_MEMORY_H
