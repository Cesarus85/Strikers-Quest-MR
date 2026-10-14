// strikers-quest r42: the life-size goal scorer (Lance, after alpha 1.2: a really good looking life
// size scorer that runs up beside you, 5 ft for every character (r44h: each its own height,
// xr_quest.cpp scorer_height_m), celebrates, then runs back to the
// field; every goal but a Super Strike; the replay keeps going until it is over, which
// Presentation.cpp's held replay sees to).
//
// A second copy of the scorer's model, posed here every game frame with the game's own animations
// and sent with a matrix handle of its own that only the scorer eye pass draws (xr_layers.cpp,
// glPlat.cpp). xr_quest.cpp places that pass in the room, scaled to PORT_SCORER_HEIGHT. Rendering
// only: no game state is written and no shared random number drawn; the game's characters and its
// cut-scene are only read. The run is the character's own jog; the celebration is the clip the
// goal cut-scene plays for this character (taken while it plays), else its wave; then idle.

#include "port/xr_scorer.h"
#include "port/xr_layers.h"
#include "port/xr_quest.h"

#include "Game/AnimInventory.h"
#include "Game/Character.h"
#include "Game/CharacterEffects.h"
#include "Game/CharacterTemplate.h"
#include "Game/DB/StatsTracker.h"
#include "Game/Player.h"
#include "Game/Drawable/DrawableCharacter.h"
#include "Game/GL/ShaderSkinMesh.h"
#include "Game/NisPlayer.h"
#include "Game/PoseAccumulator.h"
#include "Game/Render/Nis.h"
#include "Game/Render/Presentation.h"
#include "Game/SAnim.h"
#include "Game/SAnim/AnimRetargeter.h"
#include "Game/SAnim/pnSAnimController.h"
#include "Game/Team.h"
#include "NL/gl/glModel.h"
#include "NL/nlTask.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace
{

// GLOBALAnimProperties indices (globalanimproperties.cpp): every fielder has these, always loaded.
enum
{
    kAnimIdle = 0,
    kAnimJog = 12,
    kAnimWave = 99,
};

struct Show
{
    bool armed;          // a goal that gets the show
    bool started;        // its goal script has started counting
    const cCharacter* who;
    int index;           // g_pCharacters
    int side;
    int captainClass;
    bool prevHold;
    float leaveAt;       // when the run back starts (a skip brings it forward)
    cSAnim* celebrate;   // the cut-scene's clip for this character, or NULL
    bool celebrateMirror;
    bool celebrateBad;   // r43d: the clip stopped giving a sane pose (pose_sane); waves instead
    bool logged;
};

Show s_show;
DrawableCharacter* s_copy = NULL; // the copy (its accumulator is made per show)
cPN_SAnimController s_ctrl;
float s_height[NUM_FIELDER_CLASSES]; // game units, measured once per class (0 = not yet)

// r44i (Lance: Waluigi's "legs go through the map"): per class, from the idle pose: how high its
// lowest joint stands (the toes, about) and which joints sit at the origin there (the root nodes,
// which follow the pose's placement and are no part of the body), so no later pose sinks below it.
const int kMaxGroundNodes = 160;
bool s_groundMeasured[NUM_FIELDER_CLASSES];
float s_restZ[NUM_FIELDER_CLASSES];
bool s_rootNode[NUM_FIELDER_CLASSES][kMaxGroundNodes];

// The lowest body joint of the copy's last pose (root nodes left out).
float lowest_joint(int cls)
{
    const cPoseAccumulator* acc = s_copy->mPoseAccumulator;
    const int n = acc->GetNumNodes() < kMaxGroundNodes ? acc->GetNumNodes() : kMaxGroundNodes;
    float lo = 1e9f;
    for (int i = 0; i < n; i++)
    {
        if (s_rootNode[cls][i])
            continue;
        const float z = acc->GetNodeMatrix(i).m43;
        if (z == z && z < lo)
            lo = z;
    }
    return lo;
}

// r44i: the copy's last pose, built with `world`, raised back onto the ground if any of its joints
// went lower than the character's own standing feet. A jump is left alone; only sinking is undone.
void keep_on_ground(cCharacter* ch, const nlMatrix4& world)
{
    const int cls = (int)ch->m_eCharacterClass;
    if (cls < 0 || cls >= NUM_FIELDER_CLASSES || !s_groundMeasured[cls])
        return;
    const float lo = lowest_joint(cls);
    if (!(lo < s_restZ[cls] - 0.002f) || lo < -100.0f)
        return;
    nlMatrix4 lifted = world;
    lifted.m43 += s_restZ[cls] - lo;
    s_copy->mPoseAccumulator->BuildNodeMatrices(lifted);
}

cCharacter* captain_of(int side)
{
    if (side < 0 || side > 1 || g_pTeams[side] == NULL)
        return NULL;
    return (cCharacter*)g_pTeams[side]->GetCaptain();
}

void release_copy()
{
    if (s_copy != NULL && s_copy->mPoseAccumulator != NULL)
    {
        delete s_copy->mPoseAccumulator;
        s_copy->mPoseAccumulator = NULL;
    }
}

// Poses the copy with anim at `seconds` (looped or held), feet at the origin, facing +X.
void pose(cCharacter* ch, cSAnim* anim, const AnimRetarget* retarget, ePlayMode mode, bool mirror, float seconds,
          bool loop)
{
    cPoseAccumulator* acc = s_copy->mPoseAccumulator;
    const float duration = anim->GetDuration();
    float tn = duration > 0.0f ? seconds / duration : 1.0f;
    tn = loop ? tn - floorf(tn) : (tn > 1.0f ? 1.0f : (tn < 0.0f ? 0.0f : tn));
    // Placement new: the class's own operator new takes from the game's shared slot pool. Never
    // Update(): that would fire the clip's trigger scripts (sounds, effects).
    ::new (&s_ctrl) cPN_SAnimController(anim, retarget, mode, NULL, 0, mirror);
    s_ctrl.m_bIgnoreTriggers = true;
    s_ctrl.SetTime(tn);
    acc->InitAccumulators();
    s_ctrl.Evaluate(1.0f, acc);
    nlMatrix4 world;
    world.SetIdentity();
    acc->BuildNodeMatrices(world);
    keep_on_ground(ch, world);  // r44i
}

// The goal cut-scene's clip, played the way the cut-scene plays it (Nis::Render): with the clip's
// own root height and turn, so dives and knee slides stay on the ground (Lance, r42: "spun around
// inside the grass" when only the body moved). Its travel is kept to 0.6 m of the spot, so a slide
// toward you never reaches you, and it starts facing you.
void pose_celebration(cCharacter* ch, float seconds, float height)
{
    cSAnim* anim = s_show.celebrate;
    const bool mirror = s_show.celebrateMirror;
    cPoseAccumulator* acc = s_copy->mPoseAccumulator;
    const float duration = anim->GetDuration();
    float tn = duration > 0.0f ? seconds / duration : 1.0f;
    tn = tn < 0.0f ? 0.0f : (tn > 1.0f ? 1.0f : tn);
    ::new (&s_ctrl) cPN_SAnimController(anim, NULL, PM_HOLD, NULL, 0, mirror);
    s_ctrl.m_bIgnoreTriggers = true;
    s_ctrl.SetTime(tn);
    acc->InitAccumulators();
    s_ctrl.Evaluate(1.0f, acc);

    nlVector3 rt = { 0.0f, 0.0f, 0.0f }, rt0 = { 0.0f, 0.0f, 0.0f };
    unsigned short a = 0, a0 = 0;
    anim->GetRootTrans(tn, &rt);
    anim->GetRootTrans(0.0f, &rt0);
    anim->GetRootRot(tn, &a);
    anim->GetRootRot(0.0f, &a0);
    if (mirror)
    {
        rt.x = -rt.x;
        rt0.x = -rt0.x;
        a = (unsigned short)(a + (0x4000 - a) * 2);
        a0 = (unsigned short)(a0 + (0x4000 - a0) * 2);
    }
    // The travel since the clip's start, turned so its start faces +X (toward you), and capped.
    const float start = 0.0000958738f * (float)a0;
    const float dx = rt.x - rt0.x, dy = rt.y - rt0.y;
    const float c = cosf(-start), s = sinf(-start);
    float lx = dx * c - dy * s, ly = dx * s + dy * c;
    const float maxUnits = 0.6f * height / PORT_SCORER_HEIGHT;
    const float d = sqrtf(lx * lx + ly * ly);
    if (d > maxUnits)
    {
        lx *= maxUnits / d;
        ly *= maxUnits / d;
    }
    nlMatrix4 world;
    nlMakeRotationMatrixZ(world, 0.0000958738f * (float)(unsigned short)(a - a0));
    world.m41 = lx;
    world.m42 = ly;
    world.m43 = rt.z;
    world.m44 = 1.0f;
    acc->BuildNodeMatrices(world);
    // r44i: the cut-scene's root height is its own stage's: on Waluigi it put his legs under the pitch.
    keep_on_ground(ch, world);
}

// `flip`: mirrored from how the inventory plays it (r43: the wave with the other hand).
void pose_inventory(cCharacter* ch, int id, float seconds, bool loop, bool flip = false)
{
    cAnimInventory* inv = ch->m_pAnimInventory;
    cSAnim* anim = inv != NULL ? inv->GetAnim(id) : NULL;
    if (anim == NULL)
        return;
    const AnimRetarget* retarget =
        ch->m_pAnimRetargetList != NULL ? ch->m_pAnimRetargetList->GetAnimRetargetWithSignature(anim) : NULL;
    pose(ch, anim, retarget, inv->GetPlayMode(id), inv->GetMirrored(id) != flip, seconds, loop);
}

// r43d: whether the copy's last pose looks like a character on its spot: the head joint finite,
// between knee and well over head height, and within two heights of the spot sideways.
bool pose_sane(cCharacter* ch, float height)
{
    const nlMatrix4& m = s_copy->mPoseAccumulator->GetNodeMatrix(ch->m_nHeadJointIndex);
    const float x = m.m41, y = m.m42, z = m.m43;
    if (!(x == x && y == y && z == z))  // NaN
        return false;
    const float row = m.m11 * m.m11 + m.m12 * m.m12 + m.m13 * m.m13;
    if (!(row > 0.01f && row < 100.0f))
        return false;
    return z > 0.15f * height && z < 2.5f * height && fabsf(x) < 2.0f * height && fabsf(y) < 2.0f * height;
}

// The model's own height in game units: its skinned vertices in the bind pose, feet to the top of
// the head (hats, Toad's cap and all). The head joint in the idle pose checks it (a model that is
// not Z-up, or has no such vertices, falls back to that).
float measure_height(cCharacter* ch, int cls)
{
    if (cls >= 0 && cls < NUM_FIELDER_CLASSES && s_height[cls] > 0.0f)
        return s_height[cls];
    float lo = 1e9f, hi = -1e9f;
    const ShaderSkinMesh* mesh = (const ShaderSkinMesh*)ch->GetSkinMesh();
    if (mesh != NULL && mesh->softwareVertices != NULL)
    {
        for (int i = 0; i < mesh->numSoftwareVerts; i++)
        {
            const float z = mesh->softwareVertices[i].position.z;
            lo = z < lo ? z : lo;
            hi = z > hi ? z : hi;
        }
    }
    float h = hi - lo;
    pose_inventory(ch, kAnimIdle, 0.0f, true);
    const float head = s_copy->mPoseAccumulator->GetNodeMatrix(ch->m_nHeadJointIndex).m43;
    // r44i: the standing pose's ground, for keep_on_ground (before it is measured, nothing is moved).
    if (cls >= 0 && cls < NUM_FIELDER_CLASSES)
    {
        const cPoseAccumulator* acc = s_copy->mPoseAccumulator;
        const int n = acc->GetNumNodes() < kMaxGroundNodes ? acc->GetNumNodes() : kMaxGroundNodes;
        float rest = 1e9f;
        for (int i = 0; i < n; i++)
        {
            const nlMatrix4& m = acc->GetNodeMatrix(i);
            s_rootNode[cls][i] = fabsf(m.m41) < 1e-4f && fabsf(m.m42) < 1e-4f && fabsf(m.m43) < 1e-4f;
            if (!s_rootNode[cls][i] && m.m43 == m.m43 && m.m43 < rest)
                rest = m.m43;
        }
        s_restZ[cls] = rest < 1e8f ? rest : 0.0f;
        s_groundMeasured[cls] = true;
        fprintf(stderr, "[port] scorer: class %d stands with its lowest joint at %.3f units\n", cls, s_restZ[cls]);
    }
    // (Toad's skin is 2.2 x his head joint, Donkey Kong's 2.4: big heads and hunched backs.)
    if (!(h > head * 0.9f && h < head * 3.2f))
        h = head > 0.1f ? head * 1.3f : 1.4f;
    fprintf(stderr, "[port] scorer: class %d height %.3f units (skin %.3f, head joint %.3f)\n", cls, h, hi - lo,
            head);
    if (cls >= 0 && cls < NUM_FIELDER_CLASSES)
        s_height[cls] = h;
    return h;
}

// The cut-scene's clip for this character, while the goal cut-scene plays. It stays in NisPlayer's
// memory until the next-but-one cut-scene loads, well after the show; not for GoalSuddenDeath, whose
// end-of-game cut-scenes follow at once.
void take_celebration(const Presentation& pres)
{
    if (s_show.celebrate != NULL || nlTaskManager::m_pInstance->m_CurrState != 0x100 ||
        strcmp(pres.mCurrentFunction, "GoalSuddenDeath") == 0)
        return;
    NisPlayer* nis = NisPlayer::Instance();
    for (int i = 0; i < 4; i++)
    {
        Nis* n = nis->mPlaying[i];
        if (n == NULL || s_show.index < 0 || s_show.index >= Nis::MAX_NUM_CHARACTERS)
            continue;
        cPN_SAnimController* c = n->mCharacterControllers[s_show.index];
        if (c != NULL && c->m_pSAnim != NULL)
        {
            s_show.celebrate = c->m_pSAnim;
            s_show.celebrateMirror = c->m_bMirror;
            fprintf(stderr, "[port] scorer: celebration clip %.2f s from the cut-scene\n", c->m_pSAnim->GetDuration());
            return;
        }
    }
}

} // namespace

void XrScorerGoal(const cCharacter* scorer, int side, bool superStrike)
{
    release_copy();
    memset(&s_show, 0, sizeof(s_show));
    s_show.index = -1;
    if (superStrike)
        return;
    int index = scorer != NULL ? GetCharacterIndex(scorer) : -1;
    // Keepers (Kritter) and anyone unknown: the team's captain comes over instead.
    if (index < 0 || index >= 8 || scorer->m_eClassType == GOALIE)
    {
        const cCharacter* captain = captain_of(side);
        index = captain != NULL ? GetCharacterIndex(captain) : -1;
    }
    if (index < 0 || index >= 8 || g_pCharacters[index] == NULL)
        return;
    const cCharacter* captain = captain_of(side);
    s_show.armed = true;
    s_show.who = g_pCharacters[index];
    s_show.index = index;
    s_show.side = side;
    s_show.captainClass = captain != NULL ? (int)captain->m_eCharacterClass : -1;
    s_show.prevHold = true;
    s_show.leaveAt = PORT_SCORER_LEAVE;
}

void XrScorerRender()
{
    const Presentation& pres = Presentation::Instance();
    const bool goalFn = strncmp(pres.mCurrentFunction, "Goal", 4) == 0;
    const int frames = pres.mGoalShowFrames;
    int phase = 0;
    float phaseTime = 0.0f;

    if (s_show.armed && goalFn && frames >= 0)
    {
        s_show.started = true;
        if (s_show.who != g_pCharacters[s_show.index])
        {
            s_show.armed = false; // the match went away under us
        }
        else
        {
            const float t = frames / 60.0f;
            // A skip (the hold dropped) sends the scorer straight back, or, before it came, calls the
            // show off.
            if (s_show.prevHold && !pres.mGoalShowHold && t < s_show.leaveAt)
            {
                if (t < PORT_SCORER_APPEAR)
                    s_show.armed = false;
                s_show.leaveAt = t;
            }
            s_show.prevHold = pres.mGoalShowHold;
            take_celebration(pres);
            if (t >= PORT_SCORER_APPEAR)
            {
                if (t < PORT_SCORER_ARRIVE && t < s_show.leaveAt)
                {
                    phase = 1;
                    phaseTime = t - PORT_SCORER_APPEAR;
                }
                else if (t < s_show.leaveAt)
                {
                    phase = 2;
                    phaseTime = t - PORT_SCORER_ARRIVE;
                }
                else if (t < s_show.leaveAt + PORT_SCORER_RUNOUT)
                {
                    phase = 3;
                    phaseTime = t - s_show.leaveAt;
                }
            }
        }
    }
    else if (s_show.armed && s_show.started && !goalFn)
    {
        s_show.armed = false; // the goal script is over
    }

    if (phase == 0)
    {
        if (!s_show.armed)
            release_copy();
        PortXrScorerFrame(0, 0.0f, 0.0f, -1, 0, -1);
        return;
    }

    cCharacter* ch = g_pCharacters[s_show.index];
    if (s_copy == NULL)
        s_copy = new DrawableCharacter();
    if (s_copy->mPoseAccumulator == NULL)
        s_copy->mPoseAccumulator = new cPoseAccumulator(ch->m_pPoseAccumulator->m_BaseSHierarchy, false);
    const int cls = (int)ch->m_eCharacterClass;
    const float height = measure_height(ch, cls);

    if (phase == 1 || phase == 3)
    {
        // The jog at the pace the life-size copy covers the ground (PORT_SCORER_SPEED): no foot slide.
        cSAnim* jog = ch->m_pAnimInventory != NULL ? ch->m_pAnimInventory->GetAnim(kAnimJog) : NULL;
        const float unitsPerSecond = PORT_SCORER_SPEED * height / PORT_SCORER_HEIGHT;
        float rate = jog != NULL && jog->m_fLinearSpeed > 0.01f ? unitsPerSecond / jog->m_fLinearSpeed : 1.0f;
        rate = rate < 0.6f ? 0.6f : (rate > 1.8f ? 1.8f : rate);
        pose_inventory(ch, kAnimJog, phaseTime * rate, true);
    }
    else
    {
        // r43 (Lance: "simply approach the player and wave and/or dance"): the character's own goal
        // celebration as its dance (from the cut-scene, kept on its spot), a wave, a wave with the
        // other hand, then idle until it runs back. Without a cut-scene clip, two waves.
        // r43d: the dance first, on arrival, and checked every frame. In r43c it came after the first
        // wave and the copy vanished for the length of the clip (Lance's video: Koopa gone 47.9-51.0 s),
        // so a clip that no longer gives a sane pose is dropped for the rest of the show.
        cSAnim* wave = ch->m_pAnimInventory != NULL ? ch->m_pAnimInventory->GetAnim(kAnimWave) : NULL;
        const float wd = wave != NULL ? wave->GetDuration() : 0.0f;
        float t = phaseTime;
        bool posed = false;
        if (s_show.celebrate != NULL && !s_show.celebrateBad)
        {
            const float cd = s_show.celebrate->GetDuration();
            if (t < cd)
            {
                pose_celebration(ch, t, height);
                if (pose_sane(ch, height))
                {
                    posed = true;
                }
                else
                {
                    s_show.celebrateBad = true;
                    fprintf(stderr, "[port] scorer: celebration clip gave no sane pose at %.2f s, waving instead\n", t);
                }
            }
            if (!s_show.celebrateBad)
                t -= cd;
        }
        if (!posed)
        {
            if (t < wd)
                pose_inventory(ch, kAnimWave, t, false);
            else if ((t -= wd) < wd)
                pose_inventory(ch, kAnimWave, t, false, true);
            else
                pose_inventory(ch, kAnimIdle, t - wd, true);
        }
    }

    // Sent like the game's own characters, but with a matrix handle of its own (the scorer layer
    // draws it alone; every other send leaves it out), no camera test and no shadow.
    s_copy->mCharacter = ch;
    s_copy->mVisible = true;
    s_copy->mPosition.x = s_copy->mPosition.y = s_copy->mPosition.z = 0.0f;
    s_copy->mBip01Position = s_copy->mPosition;
    s_copy->mFacingDirection = 0;
    s_copy->mHeadSpin = s_copy->mHeadTilt = 0;
    s_copy->mDirt = 0;
    s_copy->mEffectsTexturing = fxGetTexturing(eFXTex_Nothing);
    const unsigned char shadows = DrawableCharacter::sShadowRenderingDisabled;
    DrawableCharacter::sShadowRenderingDisabled = 1;
    DrawableCharacter::sXrIdentity.SetIdentity();
    DrawableCharacter::sXrOwnMatrix = true;
    ch->PoseSkinMesh(s_copy->mPoseAccumulator);
    s_copy->SendToGl(*ch);
    DrawableCharacter::sXrOwnMatrix = false;
    DrawableCharacter::sShadowRenderingDisabled = shadows;
    const GLSkinMesh* mesh = ch->GetSkinMesh();
    PortXrSetScorerHandle(mesh != NULL && mesh->pModel != NULL && mesh->pModel->numPackets > 0
                              ? mesh->pModel->packets[0].state.matrix
                              : 0);

    if (!s_show.logged)
    {
        s_show.logged = true;
        fprintf(stderr, "[port] scorer: index %d class %d side %d, %s\n", s_show.index, cls, s_show.side,
                s_show.celebrate != NULL ? "cut-scene celebration" : "wave");
    }
    // r43d (Lance: stats per player per game on the banner; a hat trick is three regular goals, Super
    // Strike goals count in the stats only): the game's own tally (StatsTracker, the same on both
    // headsets online). A keeper's goal shows the captain, and so the captain's numbers.
    int goals = -1, assists = 0, hatTrick = 0;
    StatsTracker* stats = nlSingleton<StatsTracker>::s_pInstance;
    const cPlayer* player = ch->m_eClassType != GOALIE ? static_cast<const cPlayer*>(ch) : NULL;
    if (stats != NULL && player != NULL && player->m_pTeam != NULL)
    {
        const int team = player->m_pTeam->m_nSide, id = player->m_ID;
        if (team >= 0 && team < 2 && id >= 0 && id < 5)
        {
            const PlayerStats& ps = stats->mCurrentPlayerStats[team][id];
            goals = ps.mNumGoalsFor;
            assists = ps.mNumAssists;
            hatTrick = (int)ps.mNumGoalsFor - (int)ps.mNumShootToScoreGoals == 3 ? 1 : 0;
        }
    }
    PortXrScorerStats(goals, assists, hatTrick);
    PortXrScorerFrame(phase, phaseTime, height, cls, s_show.side, s_show.captainClass);
}
