// strikers-quest r46: what RetroAchievements reads as GameCube memory (include/port/ra_memory.h).
//
// The official Super Mario Strikers set is written against a console's RAM: rcheevos address A is
// GameCube 0x80000000 + A, big-endian, in the game's 32-bit struct layouts. The port runs the game
// natively (64-bit, little-endian) and has no such RAM. So once per game frame the few objects the
// set reads are copied, field by field, into big-endian images laid out at their console
// addresses, and rcheevos reads those. Nothing in the game is written.
//
// Addresses and meanings: RetroAchievements' public code notes for the game. Layouts: the decomp's
// /* 0x... */ offsets. The two statics were checked against the US DOL's code.
//   0x80323460  g_CrowdState.LastGameTime (CrowdMood.cpp), the notes' "in game Timer 2": the match
//               clock as of the crowd's last update. UpdateTiming (0x80150120) loads and stores it
//               at 0x80323418 + 0x48.
//   0x80373874  SHMainMenu::mLastMenuItem, the notes' "Game Mode" (low byte at ...877): the main
//               menu item last chosen, 0 Grudge, 1 Cup, 2 Super Cup, 3 Custom, 4 Strikers 101,
//               5 Spoils, 6 Options. It is r13 - 0x57CC, with r13 = 0x80379040 (__init_registers),
//               in Update and SceneCreated; mSnapMenuIntoPosition is r13 - 0x57D0.
//   0x81531D68  the GameInfoManager singleton (0x4C80 bytes): cup settings, cheats, spoils,
//               milestones, the current mode. main.cpp allocates it first.
//   0x815369F0  its constructor's friendly BasicGameInfo, and the demo's at 0x81536A18, each behind
//               the console allocator's 8-byte block header (notes: "Stadium Selected" 0x81536A03).
//   0x81536A40  the StatsTracker singleton, which main.cpp allocates next: this match's TeamStats[2]
//               at +0x08 (notes: "Total Goals [Left Team]" 0x81536A57 is [0] + 0x0F, and so on).
// Everything else reads as 0. The set reads nothing else.

#include "port/ra_memory.h"

#include "NL/nlConfig.h"
#include "NL/nlSingleton.h"

// Declared before the headers below instantiate them, as StatsTracker.cpp does. GameInfo.cpp and
// StatsTracker.cpp define them.
class GameInfoManager;
class StatsTracker;
template <>
GameInfoManager* nlSingleton<GameInfoManager>::s_pInstance;
template <>
StatsTracker* nlSingleton<StatsTracker>::s_pInstance;

#include "Game/Audio/CrowdMood.h"
#include "Game/DB/StatsTracker.h"
#include "Game/GameInfo.h"
#include "Game/SH/SHMainMenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

// Console addresses as rcheevos numbers them (0x80000000 off).
const uint32_t kCrowdTimeRa = 0x00323460; // g_CrowdState.LastGameTime
const uint32_t kMainMenuRa = 0x00373870;  // SHMainMenu::mSnapMenuIntoPosition, then mLastMenuItem
const uint32_t kHeapRa = 0x01531D68;      // GameInfoManager and the three blocks after it

// Offsets in the heap image.
const uint32_t kGimSize = 0x4C80;
const uint32_t kFriendlyOff = 0x4C88; // GameInfoManager::mGameInfo[GM_FRIENDLY]
const uint32_t kDemoOff = 0x4CB0;     // GameInfoManager::mGameInfo[GM_DEMO]
const uint32_t kStatsOff = 0x4CD8;    // StatsTracker
const uint32_t kStatsSize = 0x4C4;
// Each block 8 bytes after the last one ends (0x20 per BasicGameInfo).
static_assert(kFriendlyOff == kGimSize + 8 && kDemoOff == kFriendlyOff + 0x28 && kStatsOff == kDemoOff + 0x28,
              "heap image layout");

// GameInfoManager (GameInfo.h).
const uint32_t kUserInfoOff = 0x2ED4;
const uint32_t kSpoilsOff = kUserInfoOff + 0x50;
const uint32_t kSpoilSize = 0x218;
const uint32_t kDisplayTrophyOff = 0x4C39;

// The eight cup series, in GameInfoManager::eGameModes order (GM_MUSHROOM_CUP = 1), which is also
// UserInfo::mSpoils order.
struct CupSlot
{
    uint32_t off;
    const char* name;
};
const CupSlot kCups[8] = {
    { 0x0078, "Mushroom" },       { 0x025C, "Flower" },
    { 0x05E4, "Star" },           { 0x0B8C, "Bowser" },
    { 0x12B8, "Super Mushroom" }, { 0x1560, "Super Flower" },
    { 0x1ACC, "Super Star" },     { 0x23FC, "Super Bowser" },
};

uint8_t s_crowdTime[4];
uint8_t s_mainMenu[8];
uint8_t s_heap[kStatsOff + kStatsSize];

struct Region
{
    uint32_t address;
    uint32_t size;
    const uint8_t* bytes;
};
const Region kRegions[] = {
    { kCrowdTimeRa, (uint32_t)sizeof(s_crowdTime), s_crowdTime },
    { kMainMenuRa, (uint32_t)sizeof(s_mainMenu), s_mainMenu },
    { kHeapRa, (uint32_t)sizeof(s_heap), s_heap },
};

// GameInfoManager::mDisplayTrophy is never set by its constructor, so on the console it holds the
// 0xCD that nlInitMemory fills the heap with until the first cup match starts (OnPreCupGameState
// writes [0]) and ends (OnPostCupGameState writes [1..5]). The set's "Load Protection"
// (0x815369A1 != 0xCD) is exactly that test: no unlocks off a save that loads before any cup match
// has been played. The port's heap starts zeroed, so the fill is replayed here.
const GameInfoManager* s_trophyGim; // the instance the flags are about
bool s_cupMatchStarted;
bool s_inCupMatch;
bool s_cupMatchEnded;

// A native byte as stored. A bool is read this way so no value other than 0/1 goes through a bool.
inline uint32_t raw8(const void* p)
{
    uint8_t b;
    memcpy(&b, p, 1);
    return b;
}

// A console image being written: big-endian, and bounds-checked, so a wrong offset drops a byte
// instead of writing past the image.
struct Img
{
    uint8_t* p;
    uint32_t size;

    void u8(uint32_t off, uint32_t v) const
    {
        if (off < size)
            p[off] = (uint8_t)v;
    }
    void u16(uint32_t off, uint32_t v) const
    {
        u8(off, v >> 8);
        u8(off + 1, v);
    }
    void u32(uint32_t off, uint32_t v) const
    {
        u16(off, v >> 16);
        u16(off + 2, v);
    }
    // Enums and ints: 4 bytes on the console, the value in the last byte.
    void s32(uint32_t off, int32_t v) const { u32(off, (uint32_t)v); }
    void f32(uint32_t off, float f) const
    {
        uint32_t u;
        memcpy(&u, &f, 4);
        u32(off, u);
    }
    void b(uint32_t off, const void* native) const { u8(off, raw8(native)); }
};

void put_gameplay(const Img& m, uint32_t o, const GameplaySettings& g)
{
    m.s32(o + 0x0, (int32_t)g.SkillLevel);
    m.s32(o + 0x4, (int32_t)g.GameTime);
    m.b(o + 0x8, &g.PowerUps);
    m.b(o + 0x9, &g.Shoot2Score);
    m.b(o + 0xA, &g.BowserAttackEnabled);
    m.b(o + 0xB, &g.RumbleEnabled);
}

void put_audio(const Img& m, uint32_t o, const AudioSettings& a)
{
    m.s32(o + 0x00, a.MusicVolume);
    m.s32(o + 0x04, a.SFXVolume);
    m.s32(o + 0x08, a.VoiceVolume);
    m.s32(o + 0x0C, (int32_t)a.Mode);
    m.s32(o + 0x10, a.DefaultMusicVolume);
    m.s32(o + 0x14, a.DefaultSFXVolume);
    m.s32(o + 0x18, a.DefaultVoiceVolume);
    m.s32(o + 0x1C, (int32_t)a.DefaultMode);
}

// The header every Cup<>/Knockout<> starts with. The console's vtable pointer at 0x1C (MWCC puts it
// after the members) is a code address with no copy here, so it stays 0.
void put_basecup(const Img& m, uint32_t o, const BaseCup& c)
{
    m.s32(o + 0x00, (int32_t)c.mUserSelectedTeam);
    m.s32(o + 0x04, (int32_t)c.mUserSelectedSidekick);
    m.u16(o + 0x08, (uint16_t)c.mRoundNumber);
    m.u16(o + 0x0A, (uint16_t)c.mGameNumber);
    m.u16(o + 0x0C, c.mHumanTeams);
    m.b(o + 0x0E, &c.mCupStarted);
    put_gameplay(m, o + 0x10, c.mCupSettings);
}

void put_basicgameinfo(const Img& m, uint32_t o, const BasicGameInfo& g)
{
    m.s32(o + 0x00, (int32_t)g.mTeamIndex[0]);
    m.s32(o + 0x04, (int32_t)g.mTeamIndex[1]);
    m.s32(o + 0x08, (int32_t)g.mSidekickIndex[0]);
    m.s32(o + 0x0C, (int32_t)g.mSidekickIndex[1]);
    m.s32(o + 0x10, (int32_t)g.mStadiumIndex);
    for (uint32_t i = 0; i < 4; i++)
        m.u16(o + 0x14 + i * 2, (uint16_t)g.mPadSides[i]);
    m.u16(o + 0x1C, (uint16_t)g.mFinalScore[0]);
    m.u16(o + 0x1E, (uint16_t)g.mFinalScore[1]);
}

// 0x34 on the console; natively the two unsigned longs make it larger.
void put_playerstats(const Img& m, uint32_t o, const PlayerStats& s)
{
    m.u16(o + 0x00, s.mNumShotsOnGoal);
    m.u16(o + 0x02, s.mNumGoalsFor);
    m.u16(o + 0x04, s.mNumGoalsAgainst);
    m.u16(o + 0x06, s.mNumAssists);
    m.u16(o + 0x08, s.mNumFouls);
    m.u16(o + 0x0A, s.mNumGamesPlayed);
    m.u16(o + 0x0C, s.mNumPowerupsUsed);
    m.u16(o + 0x0E, s.mNumPowerupsHit);
    m.u16(o + 0x10, s.mNumShootToScoreGoals);
    m.u16(o + 0x12, s.mNumPassesMade);
    m.u16(o + 0x14, s.mNumPassesReceived);
    m.u16(o + 0x16, s.mNumPassesIntercepted);
    m.u16(o + 0x18, s.mNumHitsMade);
    m.u16(o + 0x1A, s.mNumSteals);
    m.u16(o + 0x1C, s.mNumGoalsOneTimers);
    m.u16(o + 0x1E, s.mNumSTSAttempts);
    m.u16(o + 0x20, s.mNumPerfectPasses);
    m.u32(o + 0x24, (uint32_t)s.mBallPossessionTime);
    m.u32(o + 0x28, (uint32_t)s.mNumButtonPresses);
    int32_t record;
    memcpy(&record, &s.mRecordType, sizeof(record)); // team, character or pad: one int either way
    m.s32(o + 0x2C, record);
    m.s32(o + 0x30, (int32_t)s.mType);
}

void put_teamstats(const Img& m, uint32_t o, const TeamStats& t)
{
    m.s32(o + 0x0, (int32_t)t.mTeamIndex);
    m.u16(o + 0x4, t.mNumWins);
    m.u16(o + 0x6, t.mNumLosses);
    m.u16(o + 0x8, t.mNumOTLosses);
    m.u16(o + 0xA, t.mNumPoints);
    put_playerstats(m, o + 0xC, t.mPlayerTotalStats);
}

void put_spoil(const Img& m, uint32_t o, const Spoil& s)
{
    for (uint32_t i = 0; i < 10; i++)
    {
        const CupRecord& r = s.mCupHistory[i];
        const uint32_t ro = o + i * 0x34;
        m.s32(ro + 0x00, r.mDate.sec);
        m.s32(ro + 0x04, r.mDate.min);
        m.s32(ro + 0x08, r.mDate.hour);
        m.s32(ro + 0x0C, r.mDate.mday);
        m.s32(ro + 0x10, r.mDate.mon);
        m.s32(ro + 0x14, r.mDate.year);
        m.s32(ro + 0x18, r.mDate.wday);
        m.s32(ro + 0x1C, r.mDate.yday);
        m.s32(ro + 0x20, r.mDate.msec);
        m.s32(ro + 0x24, r.mDate.usec);
        m.u8(ro + 0x28, (uint8_t)r.mPlace);
        m.s32(ro + 0x2C, (int32_t)r.mTeam);
        m.s32(ro + 0x30, (int32_t)r.mDifficulty);
    }
    m.u8(o + 0x208, s.mNumRecords);
    m.u16(o + 0x20A, s.mNumWins);
    m.u16(o + 0x20C, s.mNumLosses);
    m.u16(o + 0x20E, s.mNumCupWins);
    m.s32(o + 0x210, (int32_t)s.mCurrentChamp);
    m.b(o + 0x214, &s.mIsCPUChamp);
}

void put_userinfo(const Img& m, uint32_t o, const UserInfo& u)
{
    m.u32(o + 0x0000, (uint32_t)u.mSaveID);
    put_audio(m, o + 0x0004, u.mAudioOptions);

    m.b(o + 0x0024, &u.mVisualOptions.mIsAutoZoomCamera);
    m.f32(o + 0x0028, u.mVisualOptions.mCameraZoomLevel);
    m.b(o + 0x002C, &u.mVisualOptions.mIsWidescreen);

    put_gameplay(m, o + 0x0030, u.mGameplayOptions);

    const PowerupSettings& p = u.mPowerupOptions;
    m.b(o + 0x003C, &p.RedShells);
    m.b(o + 0x003D, &p.GreenShells);
    m.b(o + 0x003E, &p.BlueShells);
    m.b(o + 0x003F, &p.SpinyShells);
    m.b(o + 0x0040, &p.Starman);
    m.b(o + 0x0041, &p.Twister);
    m.b(o + 0x0042, &p.Bobombs);
    m.b(o + 0x0043, &p.Bananas);

    // The set's cheat guard: 0x81534C83 (custom power-ups) to ...C87 (perfect strikes).
    const CheatSettings& c = u.mCheatOptions;
    m.s32(o + 0x0044, (int32_t)c.mCustomPowerups);
    m.b(o + 0x0048, &c.mStunnedGoalies);
    m.b(o + 0x0049, &c.mInfinitePowerups);
    m.b(o + 0x004A, &c.mCheatTBD1Enabled);
    m.b(o + 0x004B, &c.mCheatTBD2Enabled);

    m.u8(o + 0x004C, u.mTrophies[0]);
    m.u8(o + 0x004D, u.mTrophies[1]);
    for (uint32_t i = 0; i < 8; i++)
        put_spoil(m, o + 0x0050 + i * kSpoilSize, u.mSpoils[i]);

    m.b(o + 0x1110, &u.mIsFlowerCupUnlocked);
    m.b(o + 0x1111, &u.mIsStarCupUnlocked);
    // The milestones.
    m.u16(o + 0x1112, u.mNumGamesPlayed);
    m.u16(o + 0x1114, u.mNumGoalsScored);
    m.u16(o + 0x1116, u.mNumSTSAttempts);
    m.u16(o + 0x1118, u.mNumPerfectPasses);
    m.u16(o + 0x111A, u.mNumHits);
    put_basicgameinfo(m, o + 0x111C, u.mBowserCupFinalRound);
}

const BaseCup& series(const GameInfoManager& g, int i)
{
    switch (i)
    {
    case 0: return g.mMushroomCupSeries;
    case 1: return g.mFlowerCupSeries;
    case 2: return g.mStarCupSeries;
    case 3: return g.mBowserCupSeries;
    case 4: return g.mSuperMushroomCupSeries;
    case 5: return g.mSuperFlowerCupSeries;
    case 6: return g.mSuperStarCupSeries;
    default: return g.mSuperBowserCupSeries;
    }
}

// Only members the constructors set on the console too: the rest hold 0xCD there and nothing in the
// set reads them, so they stay 0 rather than be guessed.
void put_gim(const Img& m, const GameInfoManager& g)
{
    put_gameplay(m, 0x0004, g.mCurGameGameplayOptions);
    m.b(0x006C, &g.mDoingKnockout);
    m.b(0x006D, &g.mDidRoundJustEnd);

    for (int i = 0; i < 8; i++)
        put_basecup(m, kCups[i].off, series(g, i));
    put_basecup(m, 0x1134, g.mBowserCupKnockout);
    put_basecup(m, 0x2D2C, g.mSuperBowserCupKnockout);

    put_userinfo(m, kUserInfoOff, g.mUserInfo);

    m.s32(0x4954, (int32_t)g.mCurrentMode); // "Cup Chosen" is its low byte, 0x815366BF
    m.b(0x4958, &g.mDemoEnabled);
    m.b(0x4959, &g.mIsInStrikers101Mode);
    m.b(0x495A, &g.mGoToChooseCaptains);
    m.s32(0x495C, (int32_t)g.mMainUserPadNumber);
    for (uint32_t i = 0; i < 4; i++)
        put_playerstats(m, 0x4968 + i * 0x34, g.mUserStats[i]);
    for (uint32_t i = 0; i < 8; i++)
        put_teamstats(m, 0x4A38 + i * 0x40, g.mPreviousTeamStats[i]); // the notes' cup standings
    m.b(0x4C38, &g.mUseCurGameSettings);                                // the notes' "In match?"

    // OnPreGameState sets mUseCurGameSettings and, in a cup, runs OnPreCupGameState in the same
    // call; OnPostGameState clears it. So a frame seeing it set in a cup mode has [0] written.
    if (s_trophyGim != &g)
    {
        s_trophyGim = &g;
        s_cupMatchStarted = s_inCupMatch = s_cupMatchEnded = false;
    }
    const int mode = (int)g.mCurrentMode;
    const bool cupMode = mode >= GameInfoManager::GM_MUSHROOM_CUP && mode <= GameInfoManager::GM_SUPER_BOWSER_CUP;
    const bool inMatch = raw8(&g.mUseCurGameSettings) != 0;
    if (inMatch && cupMode)
    {
        s_cupMatchStarted = true;
        s_inCupMatch = true;
    }
    else if (!inMatch && s_inCupMatch)
    {
        s_cupMatchEnded = true;
        s_inCupMatch = false;
    }
    m.u8(kDisplayTrophyOff, s_cupMatchStarted ? raw8(&g.mDisplayTrophy[0]) : 0xCD);
    for (uint32_t i = 1; i < 6; i++)
        m.u8(kDisplayTrophyOff + i, s_cupMatchEnded ? raw8(&g.mDisplayTrophy[i]) : 0xCD);

    m.s32(0x4C5C, (int32_t)g.mLastHumanStadium);
    put_audio(m, 0x4C60, g.mCurGameAudioSettings);

    const BasicGameInfo* friendly = g.mGameInfo[GameInfoManager::GM_FRIENDLY];
    if (friendly != NULL)
        put_basicgameinfo(m, kFriendlyOff, *friendly);
    const BasicGameInfo* demo = g.mGameInfo[GameInfoManager::GM_DEMO];
    if (demo != NULL)
        put_basicgameinfo(m, kDemoOff, *demo);
}

void put_stats(const Img& m, const StatsTracker& s)
{
    for (uint32_t i = 0; i < 2; i++)
    {
        put_teamstats(m, kStatsOff + 0x008 + i * 0x40, s.mCurrentTeamStats[i]); // this match
        put_teamstats(m, kStatsOff + 0x088 + i * 0x40, s.mCumulativeTeamStats[i]);
    }
}

// --- PortRaMemoryDump ------------------------------------------------------------------------

enum RaSize
{
    kRa8,
    kRa16,    // rcheevos "0x" with no size letter: 16-bit little-endian
    kRa16BE,  // 0xI
    kRa32BE,  // 0xG
    kRaFloatBE // fB
};

void dump_line(FILE* f, uint32_t address, RaSize size, const char* meaning)
{
    static const char* const kNames[] = { "8", "16", "16BE", "32BE", "fBE" };
    static const uint32_t kBytes[] = { 1, 2, 2, 4, 4 };
    uint8_t b[4] = { 0, 0, 0, 0 };
    PortRaReadMemory(address, b, kBytes[size]);

    uint32_t v = 0;
    switch (size)
    {
    case kRa8: v = b[0]; break;
    case kRa16: v = (uint32_t)b[0] | ((uint32_t)b[1] << 8); break;
    case kRa16BE: v = ((uint32_t)b[0] << 8) | b[1]; break;
    default: v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; break;
    }
    if (size == kRaFloatBE)
    {
        float fv;
        memcpy(&fv, &v, 4);
        fprintf(f, "0x%08X %-4s %g  %s\n", address, kNames[size], (double)fv, meaning);
    }
    else
    {
        fprintf(f, "0x%08X %-4s %u (0x%X)  %s\n", address, kNames[size], v, v, meaning);
    }
}

} // namespace

extern "C" void PortRaMemoryFrame(void)
{
    const Img crowd = { s_crowdTime, (uint32_t)sizeof(s_crowdTime) };
    crowd.f32(0, g_CrowdState.LastGameTime);

    const Img menu = { s_mainMenu, (uint32_t)sizeof(s_mainMenu) };
    menu.b(0, &SHMainMenu::mSnapMenuIntoPosition);
    menu.s32(4, (int32_t)SHMainMenu::mLastMenuItem);

    const Img heap = { s_heap, (uint32_t)sizeof(s_heap) };
    const GameInfoManager* gim = nlSingleton<GameInfoManager>::s_pInstance;
    if (gim != NULL)
        put_gim(heap, *gim);
    else
        memset(s_heap, 0, kStatsOff);
    const StatsTracker* stats = nlSingleton<StatsTracker>::s_pInstance;
    if (stats != NULL)
        put_stats(heap, *stats);
    else
        memset(s_heap + kStatsOff, 0, kStatsSize);
}

extern "C" uint32_t PortRaReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
    if (buffer == NULL)
        return 0;
    memset(buffer, 0, num_bytes);
    const uint64_t lo = address;
    const uint64_t hi = lo + num_bytes;
    for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++)
    {
        const Region& r = kRegions[i];
        const uint64_t rlo = r.address;
        const uint64_t rhi = rlo + r.size;
        const uint64_t a = lo > rlo ? lo : rlo;
        const uint64_t b = hi < rhi ? hi : rhi;
        if (a < b)
            memcpy(buffer + (a - lo), r.bytes + (a - rlo), (size_t)(b - a));
    }
    return num_bytes;
}

bool PortUnlockAll();                   // src/platform/debughooks.cpp
extern "C" int PortControlTampered(void); // src/platform/control.cpp

extern "C" const char* PortRaSetupTampered(void)
{
    // The command channel (control.cpp) ends matches, adds clock time, changes their length and
    // presses buttons; STRIKERS_DEBUG_CMD queues the same commands at boot (overlay.cpp).
    if (PortControlTampered())
        return "a press / stick / cmd line on the control channel";
    const char* queued = getenv("STRIKERS_DEBUG_CMD");
    if (queued != NULL && *queued != '\0')
        return "STRIKERS_DEBUG_CMD";
    if (PortUnlockAll() || GetConfigBool(Config::Global(), "givealltrophies", false))
        return "givealltrophies / STRIKERS_UNLOCK_ALL";
    // Each overrides what the cup settings the set checks say about the match.
    static const char* const kKeys[] = { "soak_diff", "team1", "team2", "sidekick1", "sidekick2", "stadium" };
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
    {
        if (Config::Global().Exists(kKeys[i]))
            return kKeys[i];
    }
    return NULL;
}

extern "C" void PortRaMemoryDump(const char* path)
{
    if (path == NULL)
        return;
    FILE* f = fopen(path, "w");
    if (f == NULL)
        return;

    fprintf(f, "# ra_memory: GameInfoManager %s, StatsTracker %s, cup match started %d, ended %d\n",
            nlSingleton<GameInfoManager>::s_pInstance != NULL ? "live" : "absent",
            nlSingleton<StatsTracker>::s_pInstance != NULL ? "live" : "absent",
            s_cupMatchStarted ? 1 : 0, s_cupMatchEnded ? 1 : 0);
    fprintf(f, "# teams 0 Daisy 1 DK 2 Luigi 3 Mario 4 Peach 5 Waluigi 6 Wario 7 Yoshi; "
               "skill 1 Rookie 2 Pro 3 Superstar 4 Legend\n");

    dump_line(f, kCrowdTimeRa, kRaFloatBE, "in game Timer 2: match clock, s (g_CrowdState.LastGameTime)");
    dump_line(f, kMainMenuRa + 7, kRa8, "Game Mode: 0 Grudge 1 Cup 2 Super Cup 3 Custom 4 101 5 Spoils 6 Options");

    char meaning[128];
    for (uint32_t i = 0; i < 8; i++)
    {
        const uint32_t cup = kHeapRa + kCups[i].off;
        const char* n = kCups[i].name;
        snprintf(meaning, sizeof(meaning), "%s Cup: user's team", n);
        dump_line(f, cup + 0x03, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: skill level", n);
        dump_line(f, cup + 0x13, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: match time, s (300 = 5 min, 600 = 10)", n);
        dump_line(f, cup + 0x14, kRa32BE, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: power-ups on", n);
        dump_line(f, cup + 0x18, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: Super Strikes on", n);
        dump_line(f, cup + 0x19, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: Bowser attack on", n);
        dump_line(f, cup + 0x1A, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "%s Cup: rumble on", n);
        dump_line(f, cup + 0x1B, kRa8, meaning);
    }

    const uint32_t cheats = kHeapRa + kUserInfoOff + 0x44;
    dump_line(f, cheats + 3, kRa8, "Cheat: custom power-ups (0 off)");
    dump_line(f, cheats + 4, kRa8, "Cheat: weak goalies");
    dump_line(f, cheats + 5, kRa8, "Cheat: infinite power-ups");
    dump_line(f, cheats + 6, kRa8, "Cheat: field tilt");
    dump_line(f, cheats + 7, kRa8, "Cheat: perfect strike");

    for (uint32_t i = 0; i < 8; i++)
    {
        snprintf(meaning, sizeof(meaning), "Spoils: %s Cup cups won", kCups[i].name);
        dump_line(f, kHeapRa + kSpoilsOff + i * kSpoilSize + 0x20E, kRa16BE, meaning);
    }

    const uint32_t milestones = kHeapRa + kUserInfoOff + 0x1112;
    dump_line(f, milestones + 0, kRa16BE, "Milestone: cup matches played");
    dump_line(f, milestones + 2, kRa16BE, "Milestone: goals in cup matches");
    dump_line(f, milestones + 4, kRa16BE, "Milestone: Super Strikes in cup matches");
    dump_line(f, milestones + 6, kRa16BE, "Milestone: Perfect Passes in cup matches");
    dump_line(f, milestones + 8, kRa16BE, "Milestone: hits in cup matches");

    dump_line(f, kHeapRa + 0x4954 + 3, kRa8, "Cup Chosen: GameInfoManager::mCurrentMode (255 none, 0 Grudge, 1-8 cups)");
    dump_line(f, kHeapRa + kDisplayTrophyOff, kRa8, "Load Protection: 205 until a cup match has started");

    static const char* const kSide[2] = { "left/home", "right/away" };
    for (uint32_t side = 0; side < 2; side++)
    {
        const uint32_t t = kHeapRa + kStatsOff + 0x008 + side * 0x40;
        snprintf(meaning, sizeof(meaning), "This match, %s: goals", kSide[side]);
        dump_line(f, t + 0x0F, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "This match, %s: steals", kSide[side]);
        dump_line(f, t + 0x27, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "This match, %s: Perfect Passes", kSide[side]);
        dump_line(f, t + 0x2D, kRa8, meaning);
        snprintf(meaning, sizeof(meaning), "This match, %s: team", kSide[side]);
        dump_line(f, t + 0x3B, kRa8, meaning);
    }
    dump_line(f, kHeapRa + kStatsOff + 0x48 + 0x0F, kRa16, "Rich Presence's right/away goals (16-bit LE read)");

    fclose(f);
}
