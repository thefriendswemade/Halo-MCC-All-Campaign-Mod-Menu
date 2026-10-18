#include "halo3.h"

#include <atomic>
#include <intrin.h>

#include "MinHook.h"

#include "cheats.h"
#include "core/log.h"
#include "games/blam.h"
#include "offsets.h"
#include "tags.h"

using EngineFn = int64_t(__fastcall*)(int64_t, int64_t, int64_t, int64_t);

static EngineFn g_origEngine = nullptr;
static EngineFn g_origFrameInput = nullptr;
using CameraUpdateFn = int64_t(__fastcall*)(void* camera, int64_t player, float dt, char* command);
static CameraUpdateFn g_origCameraUpdate = nullptr;
static std::atomic<float> g_cameraDistance{0.0f};  // extra world units, 0 = game default
static int32_t g_tlsIndex = -1;
static std::atomic<void**> g_gameState{nullptr};  // main thread's TLS block
using UnitActionFn = uint64_t(__fastcall*)(uint32_t unit, int32_t type, uint64_t c, uint64_t d);
static UnitActionFn g_origUnitAction = nullptr;
static uintptr_t g_halo3Base = 0;
static std::atomic<bool> g_noEject{false};  // "Cannot be ejected", read on the game thread

// Latest settings for the game thread. Written from MCC's input polling and
// read in the frame-input hook; both run on the game thread.
static Games::Settings g_threadSettings;
static Games::FrameInput g_threadInput;
static bool g_haveThreadSettings = false;

// Halo 3's main loop: called once on the game thread when a session starts.
// We only use it to grab that thread's TLS block, where the game state lives.
static int64_t __fastcall EngineDetour(int64_t a, int64_t b, int64_t c, int64_t d) {
    void** state = Blam::ThreadTlsBlock(g_tlsIndex);
    if (g_gameState.exchange(state) != state)
        LOG("Halo 3: captured game state at %p", (void*)state);

    return g_origEngine(a, b, c, d);
}

// Halo 3's per-frame input update. Cheats that call engine functions run
// right after it, inside the frame, where the engine's per-thread context is
// valid.
static int64_t __fastcall FrameInputDetour(int64_t a, int64_t b, int64_t c, int64_t d) {
    int64_t result = g_origFrameInput(a, b, c, d);
    void** state = g_gameState;
    if (state && g_haveThreadSettings && Blam::ThreadTlsBlock(g_tlsIndex) == state)
        Halo3Cheats::MainThread(state, g_threadSettings, g_threadInput);
    return result;
}

// The following (third-person) camera's update. Adds to the distance it hands
// the observer, which backs the camera off from the player. (Adds rather than
// multiplies: the campaign Master Chief's distance comes out near 0.)
static bool IsPlayerUnit(uint32_t unit) {
    void** state = g_gameState;
    if (!state) return false;
    __try {
        auto objects = (Blam::DataArray*)state[HALO3_TLS_SLOT_OBJECTS];
        char* object = objects ? Blam::ObjectFromDatum(objects, unit) : nullptr;
        return object && *(int32_t*)(object + HALO3_UNIT_PLAYER_INDEX) != -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Queuing a unit action: "Cannot be ejected" drops the flipped-vehicle eject
// for players (only from the flip check; action 0x14 has other senders).
// Barrel fire: the chaos gun and recoil push act around the players' shots.
using BarrelFireFn = void(__fastcall*)(uint32_t weapon, int64_t barrel, void* c, int64_t d, int64_t e);
static BarrelFireFn g_origBarrelFire = nullptr;
static void __fastcall BarrelFireDetour(uint32_t weapon, int64_t barrel, void* c, int64_t d, int64_t e) {
    Fun::Shot shot;
    Halo3Cheats::BeforeShot(g_gameState, weapon, (int16_t)barrel, shot);
    Halo3Cheats::ShotSpeed(true);
    g_origBarrelFire(weapon, barrel, c, d, e);
    Halo3Cheats::ShotSpeed(false);
    Fun::AfterShot(shot);
}

static uint64_t __fastcall UnitActionDetour(uint32_t unit, int32_t type, uint64_t c, uint64_t d) {
    if (g_noEject && type == HALO3_ACTION_EJECT &&
        (uintptr_t)_ReturnAddress() - g_halo3Base == HALO3_FLIP_EJECT_RETURN && IsPlayerUnit(unit)) {
        static int s_logged = 0;
        if (s_logged++ < 3) LOG("Halo 3: kept player unit %08X in its flipped vehicle", unit);
        return 0;  // the unit update ignores the result here
    }
    return g_origUnitAction(unit, type, c, d);
}

static int64_t __fastcall CameraUpdateDetour(void* camera, int64_t player, float dt, char* command) {
    int64_t result = g_origCameraUpdate(camera, player, dt, command);
    float extra = g_cameraDistance;
    if (command && extra > 0.0f) {
        float& distance = *(float*)(command + HALO3_OBSERVER_COMMAND_DISTANCE);
        static ULONGLONG lastLog = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastLog > 5000) {
            lastLog = now;
            float* focus = (float*)(command + HALO3_OBSERVER_COMMAND_FOCUS_OFFSET);
            LOG("Halo 3: follow camera distance %.3f (+%.2f), focus offset %.3f %.3f %.3f", distance, extra,
                focus[0], focus[1], focus[2]);
        }
        distance += extra;
    }
    return result;
}

bool Games::Halo3::Load(HMODULE module) {
    g_tlsIndex = Blam::ModuleTlsIndex(module);
    if (g_tlsIndex < 0) {
        LOG("Halo 3: no TLS directory; not hooking");
        return false;
    }

    void* target = (char*)module + OFFSET_HALO3_ENGINE;
    if (MH_CreateHook(target, (void*)&EngineDetour, (void**)&g_origEngine) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Halo 3: failed to hook engine function at %p", target);
        MH_RemoveHook(target);
        return false;
    }

    m_engineTarget = target;

    void* frameInput = (char*)module + OFFSET_HALO3_FRAME_INPUT;
    if (MH_CreateHook(frameInput, (void*)&FrameInputDetour, (void**)&g_origFrameInput) == MH_OK &&
        MH_EnableHook(frameInput) == MH_OK) {
        m_frameInputTarget = frameInput;
    } else {
        LOG("Halo 3: failed to hook frame input at %p; noclip/damage/give weapon unavailable", frameInput);
    }

    void* cameraUpdate = (char*)module + OFFSET_HALO3_FOLLOWING_CAMERA_UPDATE;
    if (MH_CreateHook(cameraUpdate, (void*)&CameraUpdateDetour, (void**)&g_origCameraUpdate) == MH_OK &&
        MH_EnableHook(cameraUpdate) == MH_OK) {
        m_cameraUpdateTarget = cameraUpdate;
    } else {
        LOG("Halo 3: failed to hook following camera update at %p; camera distance unavailable", cameraUpdate);
    }

    g_halo3Base = (uintptr_t)module;
    void* unitAction = (char*)module + OFFSET_HALO3_UNIT_ACTION_QUEUE;
    if (MH_CreateHook(unitAction, (void*)&UnitActionDetour, (void**)&g_origUnitAction) == MH_OK &&
        MH_EnableHook(unitAction) == MH_OK) {
        m_unitActionTarget = unitAction;
    } else {
        MH_RemoveHook(unitAction);
        LOG("Halo 3: failed to hook unit actions at %p; cannot be ejected unavailable", unitAction);
    }

    void* barrelFire = (char*)module + OFFSET_HALO3_BARREL_FIRE;
    if (MH_CreateHook(barrelFire, (void*)&BarrelFireDetour, (void**)&g_origBarrelFire) == MH_OK &&
        MH_EnableHook(barrelFire) == MH_OK) {
        m_barrelFireTarget = barrelFire;
    } else {
        MH_RemoveHook(barrelFire);
        LOG("Halo 3: failed to hook barrel fire at %p; chaos gun and recoil push unavailable", barrelFire);
    }

    Halo3Tags::SetModule(module);
    Halo3Cheats::Load(module);
    return true;
}

void Games::Halo3::Unload() {
    MH_DisableHook(m_engineTarget);
    MH_RemoveHook(m_engineTarget);
    m_engineTarget = nullptr;
    if (m_frameInputTarget) {
        MH_DisableHook(m_frameInputTarget);
        MH_RemoveHook(m_frameInputTarget);
        m_frameInputTarget = nullptr;
    }
    if (m_cameraUpdateTarget) {
        MH_DisableHook(m_cameraUpdateTarget);
        MH_RemoveHook(m_cameraUpdateTarget);
        m_cameraUpdateTarget = nullptr;
    }
    if (m_unitActionTarget) {
        MH_DisableHook(m_unitActionTarget);
        MH_RemoveHook(m_unitActionTarget);
        m_unitActionTarget = nullptr;
    }
    if (m_barrelFireTarget) {
        MH_DisableHook(m_barrelFireTarget);
        MH_RemoveHook(m_barrelFireTarget);
        m_barrelFireTarget = nullptr;
    }
    g_noEject = false;
    g_cameraDistance = 0.0f;
    g_gameState = nullptr;
    g_haveThreadSettings = false;
    Halo3Cheats::Unload();
    Halo3Tags::SetModule(nullptr);
}

uint64_t Games::Halo3::Features() const {
    return kGodMode | kInfiniteAmmo | kBottomlessClip | kInfiniteGrenades | kOneHitKill | kSpeed | kJump |
           kGravity | kGameSpeed | kFly | kNoclip | kNoBoundaries | kDamage | kGiveWeapon | kAmmoType |
           kAimbot | kAiBlind | kAiCantShoot | kFullAuto | kRapidFire | kNoSpread | kNoRecoil | kNoOverheat |
           kInfiniteEquipment | kNoEquipmentDespawn | kAiDeaf | kThirdPerson | kCameraDistance | kNoHud |
           kVehicleSpeed | kVehicleRapidFire | kVehicleGodMode | kVehicleInfiniteBoost | kVehicleNoHijack |
           kVehicleNoEmp | kVehicleNoRecoil | kVehicleNoEject | kCheckpoints | kSpawning | kSkulls |
           kChaosGun | kRain | kRecoilPush | kTeleport |
           // Ported from Reach 2026-10-10 (see NOTES.md, "Halo 3 port of the Reach additions")
           kNuke | kDespawnAi | kPoisonTouch | kRandomTeams | kNoFriendlyFire | kNoBetrayal | kPlayerTeam |
           kTimeStop | kLaunchOnHit | kUpgradeAi | kSquadSpawning | kCheckpointLock | kCheckpointHistory |
           kGiveEquipment | kPerfectCamo | kAiStayDead | kPilotAiVehicles | kUpgradeVehicle | kTakePilotSeat;
}

uint64_t Games::Halo3::Features2() const {
    return kAiScale | kRandomSwap | kDoubleJump | kDot | kBombingRun;
}

void Games::Halo3::Tick(const Settings& settings, const FrameInput&, Status& status) {
    g_noEject = settings.vehicleNoEject;
    void** state = g_gameState;
    if (!state) return;

    status.inGame = Halo3Cheats::Apply(state, settings, status);
}

void Games::Halo3::MainThreadTick(const Settings& settings, const FrameInput& input) {
    void** state = g_gameState;
    if (!state) return;

    if (Blam::ThreadTlsBlock(g_tlsIndex) != state) return;

    // Hand the settings to FrameInputDetour for the in-frame cheats, and run
    // noclip here.
    g_threadSettings = settings;
    g_threadInput = input;
    g_cameraDistance = settings.thirdPerson ? settings.cameraDistance : 0.0f;
    g_haveThreadSettings = true;
    Halo3Cheats::InputThread(state, settings, input);
}
