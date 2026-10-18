#include "reach.h"

#include <atomic>
#include <mutex>

#include "MinHook.h"

#include "cheats.h"
#include "core/log.h"
#include "games/blam.h"
#include "offsets.h"
#include "tags.h"

using EngineFn = int64_t(__fastcall*)(int64_t, int64_t, int64_t, int64_t);
using BarrelFireFn = void(__fastcall*)(uint32_t weapon, int64_t barrel, void* data, bool flag);
using CameraUpdateFn = int64_t(__fastcall*)(void* camera, int64_t player, float dt, char* command);
using FlipEjectFn = int32_t(__fastcall*)(uint32_t unit, void* event);
using CauseDamageFn = void(__fastcall*)(void* data, float scale, uint32_t object, uint16_t region, uint64_t a5,
                                       uint64_t a6, uint64_t a7);

// The acceleration (knockback) a damage gives an object, returned as a float.
using ExplosionAccelFn = float(__fastcall*)(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6,
                                            int64_t a7, int64_t a8);

static EngineFn g_origEngine = nullptr;
static EngineFn g_origGameTick = nullptr;
static BarrelFireFn g_origBarrelFire = nullptr;
static CameraUpdateFn g_origCameraUpdate = nullptr;
static FlipEjectFn g_origFlipEject = nullptr;
static CauseDamageFn g_origCauseDamage = nullptr;
static ExplosionAccelFn g_origExplosionAccel = nullptr;
static void(__fastcall* g_origGameSave)(int32_t) = nullptr;
static void(__fastcall* g_origObjectUpdate)(uint32_t) = nullptr;
static int32_t g_tlsIndex = -1;
static std::atomic<void**> g_gameState{nullptr};  // main thread's TLS block
static std::atomic<float> g_cameraDistance{0.0f};  // extra world units, 0 = game default

// Latest settings for the game thread: written by the render thread's Tick,
// copied out by the game-tick hook.
static std::mutex g_settingsLock;
static Games::Settings g_threadSettings;
static Games::FrameInput g_threadInput;
static bool g_haveThreadSettings = false;

// Reach's main loop: called on the game thread when a session starts. Only
// used to grab that thread's TLS block, where the game state lives.
static int64_t __fastcall EngineDetour(int64_t a, int64_t b, int64_t c, int64_t d) {
    void** state = Blam::ThreadTlsBlock(g_tlsIndex);
    if (g_gameState.exchange(state) != state) LOG("Reach: captured game state at %p", (void*)state);
    return g_origEngine(a, b, c, d);
}

// One game tick. Cheats that call engine functions run just before it,
// inside the frame, where the engine's per-thread context is valid.
static int64_t __fastcall GameTickDetour(int64_t a, int64_t b, int64_t c, int64_t d) {
    void** state = g_gameState;
    if (state && Blam::ThreadTlsBlock(g_tlsIndex) == state) {
        Games::Settings settings;
        Games::FrameInput input;
        bool have;
        {
            std::lock_guard<std::mutex> lock(g_settingsLock);
            have = g_haveThreadSettings;
            settings = g_threadSettings;
            input = g_threadInput;
        }
        if (have) ReachCheats::MainThread(state, settings, input);
    }
    int64_t result = g_origGameTick(a, b, c, d);
    if (state && Blam::ThreadTlsBlock(g_tlsIndex) == state) {
        Games::Settings settings;
        bool have;
        {
            std::lock_guard<std::mutex> lock(g_settingsLock);
            have = g_haveThreadSettings;
            if (have) settings = g_threadSettings;
        }
        if (have) ReachCheats::AfterTick(state, settings);
    }
    return result;
}

// Object update: a frozen object (time stop) isn't updated at all, so it
// keeps its pose: no animation, no physics, nothing held in its hands fires.
static void __fastcall ObjectUpdateDetour(uint32_t datum) {
    if (ReachCheats::ObjectFrozen(datum)) return;
    g_origObjectUpdate(datum);
}

// Barrel fire: skipped for AI-held weapons while "AI can't shoot" is on.
// The chaos gun and recoil push act around the players' shots.
static void __fastcall BarrelFireDetour(uint32_t weapon, int64_t barrel, void* data, bool flag) {
    if (!ReachCheats::WeaponMayFire(g_gameState, weapon)) return;
    ReachCheats::Shot shot;
    ReachCheats::BeforeShot(g_gameState, weapon, (int16_t)barrel, shot);
    g_origBarrelFire(weapon, barrel, data, flag);
    ReachCheats::AfterShot(shot);
}

// Explosion acceleration slider: scales the knockback every damage gives.
static float __fastcall ExplosionAccelDetour(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6,
                                             int64_t a7, int64_t a8) {
    float result = g_origExplosionAccel(a1, a2, a3, a4, a5, a6, a7, a8);
    float scale = ReachCheats::ExplosionScale();
    return scale == 1.0f ? result : result * scale;
}

// The following (third-person) camera's update. Adds to the distance it
// hands the observer, which backs the camera off from the player.
static int64_t __fastcall CameraUpdateDetour(void* camera, int64_t player, float dt, char* command) {
    int64_t result = g_origCameraUpdate(camera, player, dt, command);
    float extra = g_cameraDistance;
    if (command && extra > 0.0f) *(float*)(command + REACH_OBSERVER_COMMAND_DISTANCE) += extra;
    return result;
}

// The flipped-vehicle eject handler: see FlipEjectAllowed. 0 = not handled.
static int32_t __fastcall FlipEjectDetour(uint32_t unit, void* event) {
    if (!ReachCheats::FlipEjectAllowed(g_gameState, unit, event)) return 0;
    return g_origFlipEject(unit, event);
}

// Every hit on an object: dropped between the player and their allies when
// "disable friendly fire" is on.
static void __fastcall CauseDamageDetour(void* data, float scale, uint32_t object, uint16_t region, uint64_t a5,
                                         uint64_t a6, uint64_t a7) {
    if (!ReachCheats::DamageAllowed(g_gameState, data, object)) return;
    g_origCauseDamage(data, scale, object, region, a5, a6, a7);
}

// The game's checkpoint request (game_save and friends): ignored while checkpoints are locked.
static void __fastcall GameSaveDetour(int32_t priority) {
    if (ReachCheats::CheckpointsLocked()) return;
    g_origGameSave(priority);
}

static void* Hook(HMODULE module, int rva, void* detour, void** original, const char* what) {
    void* target = (char*)module + rva;
    if (MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK) return target;
    MH_RemoveHook(target);
    LOG("Reach: failed to hook %s at %p", what, target);
    return nullptr;
}

bool Games::Reach::Load(HMODULE module) {
    g_tlsIndex = Blam::ModuleTlsIndex(module);
    if (g_tlsIndex < 0) {
        LOG("Reach: no TLS directory; not hooking");
        return false;
    }

    m_hooks[0] = Hook(module, OFFSET_REACH_ENGINE, (void*)&EngineDetour, (void**)&g_origEngine, "main loop");
    if (!m_hooks[0]) return false;
    m_hooks[1] = Hook(module, OFFSET_REACH_GAME_TICK, (void*)&GameTickDetour, (void**)&g_origGameTick,
                      "game tick (noclip/damage/give weapon/aimbot pull unavailable)");
    m_hooks[2] = Hook(module, OFFSET_REACH_BARREL_FIRE, (void*)&BarrelFireDetour, (void**)&g_origBarrelFire,
                      "barrel fire (AI can't shoot unavailable)");
    m_hooks[3] = Hook(module, OFFSET_REACH_FOLLOWING_CAMERA_UPDATE, (void*)&CameraUpdateDetour,
                      (void**)&g_origCameraUpdate, "following camera (camera distance unavailable)");
    m_hooks[4] = Hook(module, OFFSET_REACH_FLIP_EJECT_HANDLER, (void*)&FlipEjectDetour, (void**)&g_origFlipEject,
                      "flip eject handler (cannot be ejected unavailable)");
    m_hooks[5] = Hook(module, OFFSET_REACH_OBJECT_CAUSE_DAMAGE, (void*)&CauseDamageDetour,
                      (void**)&g_origCauseDamage, "cause damage (disable friendly fire unavailable)");
    m_hooks[6] = Hook(module, OFFSET_REACH_GAME_SAVE, (void*)&GameSaveDetour, (void**)&g_origGameSave,
                      "game save (lock checkpoints unavailable)");

    m_hooks[7] = Hook(module, OFFSET_REACH_OBJECT_UPDATE, (void*)&ObjectUpdateDetour, (void**)&g_origObjectUpdate,
                      "object update (time stop unavailable)");

    m_hooks[8] = Hook(module, OFFSET_REACH_EXPLOSION_ACCEL, (void*)&ExplosionAccelDetour,
                      (void**)&g_origExplosionAccel, "damage acceleration (explosion size unavailable)");

    ReachTags::SetModule(module);
    ReachCheats::Load(module);
    return true;
}

void Games::Reach::Unload() {
    for (void*& target : m_hooks) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
    g_gameState = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_settingsLock);
        g_haveThreadSettings = false;
    }
    g_cameraDistance = 0.0f;
    ReachCheats::Unload();
    ReachTags::SetModule(nullptr);
}

uint64_t Games::Reach::Features2() const {
    return kDoubleJump | kAiScale | kRandomSwap | kDot | kFov | kGrenadeDrop | kExplosionSize | kBoundaryView;
}

uint64_t Games::Reach::Features() const {
    // Not in Reach: the Acrophobia skull (fly) and equipment that despawns.
    return kGodMode | kInfiniteAmmo | kBottomlessClip | kInfiniteGrenades | kOneHitKill | kSpeed | kJump |
           kGravity | kGameSpeed | kNoclip | kNoBoundaries | kDamage | kGiveWeapon | kAmmoType | kAimbot |
           kAiBlind | kAiCantShoot | kFullAuto | kRapidFire | kNoSpread | kNoRecoil | kNoOverheat |
           kInfiniteEquipment | kAiDeaf | kThirdPerson | kCameraDistance | kNoHud | kVehicleSpeed |
           kAiStayDead | kVehicleRapidFire | kVehicleGodMode | kVehicleNoHijack | kVehicleNoEmp |
           kVehicleInfiniteBoost | kVehicleNoRecoil | kVehicleNoEject | kCheckpoints | kSpawning | kPilotAiVehicles | kUpgradeVehicle | kTakePilotSeat |
           kSkulls | kChaosGun | kRain | kRecoilPush | kTeleport | kUpgradeAi | kRandomTeams | kPlayerTeam | kSquadSpawning | kNuke | kDespawnAi | kNoFriendlyFire | kNoBetrayal | kGiveEquipment | kPoisonTouch | kPerfectCamo | kTimeStop | kLaunchOnHit | kCheckpointLock | kCheckpointHistory;
}

void Games::Reach::Tick(const Settings& settings, const FrameInput& input, Status& status) {
    {
        std::lock_guard<std::mutex> lock(g_settingsLock);
        g_threadSettings = settings;
        g_threadInput = input;
        g_haveThreadSettings = true;
    }
    g_cameraDistance = settings.thirdPerson ? settings.cameraDistance : 0.0f;

    void** state = g_gameState;
    if (!state) return;
    status.inGame = ReachCheats::Apply(state, settings, status);
}
