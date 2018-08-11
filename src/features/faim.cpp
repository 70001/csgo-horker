#include "faim.h"

#include "../config.h"
#include "../engine.h"
#include "../offsets.h"
#include "../sdk/mathlib.h"
#include "../sdk/types.h"
#include "../sdk/vector.h"
#include "../utils/hmath.h"

#include <cstdlib>
#include <cstdio>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

static Display *internDpy = nullptr;

bool FAim::GetBonePosition(uintptr_t ePtr, int bone, Vector* out)
{
    matrix3x4_t mat;
    uintptr_t pBoneMatrix;
    if (!m_mem.Read(ePtr + Offset::Static::BoneMatrix, &pBoneMatrix)) {
        Log("[FAim/Aim] Failed to get bone matrix");
        return false;
    }
    if (!m_mem.Read(pBoneMatrix + Offset::Static::BoneDistance * bone, &mat)) {
        Log("[FAim/Aim] Failed to get bone distance");
        return false;
    }
    out->x = mat[0][3];
    out->y = mat[1][3];
    out->z = mat[2][3];
    return true;
}

void FAim::Aim(uintptr_t localPlayer, int myTeam)
{
    auto tickClock = std::chrono::high_resolution_clock::now();
    auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(tickClock - m_nLastTick);
   
    // Mouse speed is highly affected by user sensitivity
    // This lowers it so it requires less ticks per call
    // i.e. faster aiming
    if (Config::AimBot::UseMouseEvents && tickMs.count() < 1000/256) {
        return;
    } else if (!Config::AimBot::UseMouseEvents && tickMs.count() < 1000/64) {
        return;
    }

    m_nLastTick = tickClock;

    auto& eng = Engine::GetInstance();
    Vector vecEyes;
    Vector vecEyesOffset;
    Vector viewAngle;
    Vector punchAngle;
    int localIndex;
    if (m_mem.Read(localPlayer + Netvar::CBaseEntity::m_vecOrigin, &vecEyes) &&
        m_mem.Read(localPlayer + Netvar::CBaseEntity::m_vecViewOffset, &vecEyesOffset) &&
        m_mem.Read(Offset::Engine::ClientState + Offset::Static::ViewAngles , &viewAngle) &&
        m_mem.Read(localPlayer + Netvar::CBasePlayer::Local::m_aimPunchAngle, &punchAngle) &&
        m_mem.Read(localPlayer + Netvar::CBaseEntity::index, &localIndex)) {
        
        vecEyes += vecEyesOffset;
        float bestVal = FLT_MAX;
        Vector bestTarget(0.f, 0.f, 0.f);
        for (size_t i = 1; i < 64; ++i) {
            uintptr_t  pEnt;
            CBaseEntity ent;
            if (!eng.GetEntityById(i, &ent) || !eng.GetEntityPtrById(i, &pEnt)) {
                continue;
            }
            if (ent.index == localIndex) {
                continue;
            }
            if (ent.m_iTeamNum != TEAM_T && ent.m_iTeamNum != TEAM_CT) {
                continue;
            }
            if (ent.m_iTeamNum == myTeam)
                continue;

            if (ent.m_iHealth < 1) {
                continue;
            }
            Vector hitbox;
            if (!GetBonePosition(pEnt, Config::AimBot::TargetBone, &hitbox)) {
                continue;
            }
            if (Config::AimBot::TargetMode == 0) {
                float dist = vecEyes.DistTo(hitbox);
                if (dist < bestVal) {
                    bestVal = dist;
                    bestTarget = hitbox;
                }
            } else if (Config::AimBot::TargetMode == 1) {
                float fov = HMath::GetFov(viewAngle, vecEyes, hitbox);
                if (std::abs(fov) > Config::AimBot::AimFieldOfView) {
                    continue;
                }
                if (fov < bestVal) {
                    bestVal = fov;
                    bestTarget = hitbox;
                }
            }
        }

        if (bestTarget.x != 0.f) {
            vecEyes -= bestTarget;
            vecEyes.NormalizeInPlace();
            Vector anglesDir = HMath::VectorAngles(vecEyes);
            Vector clampedDir = HMath::ClampAngle(anglesDir);
            Vector diffAngles = HMath::ClampAngle(viewAngle - clampedDir + punchAngle);
            // AimBot rotation speed

            if (Config::AimBot::UseMouseEvents) {
                float outX = HMath::Clampf(diffAngles.x,
                        -Config::AimBot::AimSpeed, 
                        Config::AimBot::AimSpeed,
                        Config::AimBot::AimCorrection);
                float outY = HMath::Clampf(diffAngles.y, 
                        -Config::AimBot::AimSpeed,
                        Config::AimBot::AimSpeed,
                        Config::AimBot::AimCorrection);
                XTestFakeRelativeMotionEvent(internDpy, (int) outY, -(int) outX, 0);
            } else {
                // Don't use correction in memory mode
                float adjAimSpeed = Config::AimBot::AimSpeed/6.0f;
                float outX = HMath::Clampf(diffAngles.x,
                        -adjAimSpeed, 
                        adjAimSpeed,
                        1.f);
                float outY = HMath::Clampf(diffAngles.y, 
                        -adjAimSpeed,
                        adjAimSpeed,
                        1.f);
                viewAngle.x -= outX;
                viewAngle.y -= outY;
                m_mem.Write(Offset::Engine::ClientState + Offset::Static::ViewAngles, viewAngle);
            }
            XFlush(internDpy);
        }
    }
}

void FAim::Run()
{
    m_nLastTick = std::chrono::high_resolution_clock::now();
    internDpy = XOpenDisplay(NULL);
    
    if (internDpy == nullptr) {
        Log("[FAim] Failed to open display");
        return;
    }
    const int triggerKey = Engine::StringToKeycode(Config::AimBot::TriggerKey);
    
    Log("[FAim] Started");
    
    auto& eng = Engine::GetInstance();
    
    while (!ShouldStop()) {
        uintptr_t localPlayer;
        if (!m_mem.Read(Offset::Client::LocalPlayer, &localPlayer)) {
            LogWait("[FAim] Failed to read local player address.");
            continue;
        }

        int myTeam;
        if (!m_mem.Read(localPlayer + Netvar::CBaseEntity::m_iTeamNum, &myTeam)) {
            WaitMs(20);
            continue;
        }

        if (!eng.IsKeyDown(triggerKey) && Config::AimBot::UseTriggerKey) {
            WaitMs(20);
            continue;
        }

        // AIMBOT START
        if (Config::AimBot::AimAssist) {
            this->Aim(localPlayer, myTeam);
        }

        if (Config::AimBot::Trigger) {

            int inCrossID;
            if (!m_mem.Read(localPlayer + Netvar::CBasePlayer::m_iCrosshairID, &inCrossID)) {
                LogWait("[FAim/Trigger] Failed to read crosshair ID");
                continue;
            }

            if (inCrossID > 0 && inCrossID < 65) {
                CBaseEntity ent;
                if (!eng.GetEntityById(inCrossID, &ent)) {
                    WaitMs(20);
                    continue;
                }
                if (ent.m_iHealth > 0) {
                    if (ent.m_iTeamNum != myTeam) {
                        eng.ForceAttack(true);
                        WaitMs(10);
                        eng.ForceAttack(false);
                        WaitMs(Config::AimBot::TriggerDelay);
                    }
                }
            }
        }
        WaitMs(1);
    }
    XCloseDisplay(internDpy);
    Log("[FAim] Stopped");
}
