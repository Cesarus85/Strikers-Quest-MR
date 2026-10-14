// strikers-quest r42: the living crowd and the goal roar (src/platform/living_crowd.cpp).
#ifndef PORT_LIVING_CROWD_H
#define PORT_LIVING_CROWD_H

// Audio::Update, every game frame after the crowd mood.
void PortLivingCrowdUpdate(float dt);
// AudioScriptEventMgr::FireEvent: the game's own crowd events (AUDIO_EVENT, AUDIO_EVENT_TEAM).
void PortLivingCrowdEvent(int event, int team);
// Game.cpp at a goal: the roar after it (side 0 home / 1 away scored).
void PortCrowdRoarStart(int side);

#endif // PORT_LIVING_CROWD_H
