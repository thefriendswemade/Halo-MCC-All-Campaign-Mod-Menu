// Halo: Reach cheats that call engine functions. Runs once per game tick on
// the game's main thread, before the tick (see reach.cpp).

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cheat_util.h"
#include "cheats.h"
#include "core/log.h"
#include "games/squads.h"
#include "tags.h"

using namespace ReachCheats;

namespace {
    constexpr float kNoclipSpeed = 6.0f;      // world units per second
    constexpr float kNoclipFastScale = 4.0f;
    constexpr int kMaxTracked = 4096;         // object table slots we track for damage
    constexpr float kPi = 3.14159265f;
    constexpr float kAimMinCone = 3.0f;       // degrees from the crosshair, at 1% strength
    constexpr float kAimMaxCone = 55.0f;      // at 100%: about the whole screen
    constexpr float kAimRange = 200.0f;       // world units

    using PlacementNewFn = void(__fastcall*)(void* placement, uint32_t tag, uint32_t owner, int64_t);
    using ObjectNewFn = uint32_t(__fastcall*)(void* placement);
    using ObjectDeleteFn = void(__fastcall*)(uint32_t object);
    using UnitAddWeaponFn = bool(__fastcall*)(uint32_t unit, uint32_t weapon, int mode);
    using UnitRemoveEquipmentFn = void(__fastcall*)(uint32_t unit);
    using UnitAddEquipmentFn = void(__fastcall*)(uint32_t unit, uint32_t equipment);
    using EquipmentInitFn = void(__fastcall*)(uint32_t equipment);
    using EquipmentSetFlagsFn = void(__fastcall*)(uint32_t equipment, uint64_t flags);
    using UnitKillFn = void(__fastcall*)(uint32_t unit, uint32_t, bool, int, int);
    using HeadPositionFn = void(__fastcall*)(uint32_t unit, float* out);
    using SetScaleFn = void(__fastcall*)(uint32_t object, float scale, uint32_t ticks);
    using SetVelocityFn = void(__fastcall*)(uint32_t object, const float* linear, const float* angular);
    using SetPositionFn = void(__fastcall*)(uint32_t object, const float* position, const float* forward,
                                            const float* up, int64_t, bool);
    using ActorRequestInitFn = void(__fastcall*)(void* request);
    using ActorSpawnFn = uint32_t(__fastcall*)(int64_t spawnPoint, void* request);
    using ActorSetActiveFn = void(__fastcall*)(uint32_t actor, bool active);
    using ActorSetTeamFn = void(__fastcall*)(uint32_t actor, int64_t team);
    using ActorEraseFn = void(__fastcall*)(uint32_t actor, bool);
    using ActorMigrateFn = void(__fastcall*)(uint32_t actor, int16_t squad);
    using ObjectsAttachFn = void(__fastcall*)(uint32_t parent, uint32_t parentMarker, uint32_t child,
                                              uint32_t childMarker);
    using UnitEnterVehicleFn = void(__fastcall*)(uint32_t unit, uint32_t vehicle, uint32_t seatName, bool immediate);
    using ObjectDetachFn = void(__fastcall*)(uint32_t object);
    using SeatOccupantFn = uint32_t(__fastcall*)(uint32_t vehicle, uint32_t seat);
    using CollisionTestFn = bool(__fastcall*)(uint64_t flags, bool, const float* point, const float* vector,
                                              int64_t ignore1, int64_t ignore2, int64_t ignore3, void* result);
    constexpr float kSpawnRange = 300.0f;   // world units the spawn ray reaches
    constexpr float kSpawnBackOff = 0.3f;   // back along the ray from the hit, so it isn't inside the surface
    constexpr float kSpawnLift = 0.25f;     // and up, so it doesn't clip into the ground
    constexpr float kVehicleLift = 0.5f;    // vehicles: above the higher of the hit and the backed-off point

    char* g_module = nullptr;  // set by ReachCheats::Load via SetMainThreadModule

    template <typename Fn>
    Fn Engine(int rva) { return (Fn)(g_module + rva); }

    // Noclip
    bool g_noclipActive = false;
    uint32_t g_noclipUnit = 0xFFFFFFFF;
    float g_anchor[3] = {};
    LARGE_INTEGER g_lastTick{};

    // Damage multiplier: last seen vitality per object slot.
    struct Tracked {
        uint32_t datum;
        float health;
        float shield;
    };
    Tracked g_tracked[kMaxTracked];

    uint32_t g_giveSerial = 0;
    uint32_t g_giveEquipmentSerial = 0;
    uint32_t g_spawnSerial = 0;

    // AI we spawned, followed after death so "Enable respawn" decides
    // whether they come back (see FollowSpawnedAi).
    struct SpawnedAi {
        uint32_t actor = 0xFFFFFFFF;
        uint32_t unit = 0xFFFFFFFF;
        uint32_t character = 0xFFFFFFFF;  // 'char' tag, to respawn it
        int team = 0;                     // spawnTeam it was made with
        ULONGLONG goneAt = 0;             // when its actor went, 0 while it has one
    };
    constexpr int kMaxSpawnedAi = 512;
    constexpr ULONGLONG kRespawnMs = 5000;  // dead this long: respawned or removed
    SpawnedAi g_spawnedAi[kMaxSpawnedAi];

    // Spawning results for the menu (game thread writes, render thread reads).
    std::mutex g_spawnLock;
    uint32_t g_spawnDone = 0;
    char g_spawnMessage[96] = "";

    void SpawnResult(const char* text) {
        std::lock_guard<std::mutex> lock(g_spawnLock);
        sprintf_s(g_spawnMessage, "%s", text);
        ++g_spawnDone;
        LOG("Reach: spawn: %s", text);
    }
    uint32_t g_saveSerial = 0;
    uint32_t g_loadSerial = 0;

    // Designer zones (see offsets.h): a spawn or give whose tag is only in
    // a zone that isn't loaded loads that zone, then retries until it
    // works. Zones the mod loaded are kept loaded for the rest of the map,
    // so what it made isn't deleted at the next zone set switch.
    constexpr ULONGLONG kZoneLoadMs = 5000;  // how long a spawn waits for its zone
    uint32_t g_forcedZones = 0;
    char* g_forcedScenario = nullptr;  // the scenario g_forcedZones belong to

    struct ZoneWait {
        bool active = false;
        uint32_t tag = 0xFFFFFFFF;
        uint32_t zone = 0;  // the zone's bit
        ULONGLONG deadline = 0;
        int team = 0;       // spawn only
        float position[3] = {};
        float yaw = 0.0f;
    };
    ZoneWait g_spawnWait;
    ZoneWait g_giveWait;

    float TickSeconds(LARGE_INTEGER& last) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
        last = now;
        return dt > 0.1f ? 0.1f : dt;
    }

    // Holds player 1 in place, and flies them where they're looking while
    // the noclip button is held. The engine's teleport ignores collision.
    void Noclip(void** state, const Players& p, bool on, const Games::FrameInput& in, float dt) {
        // Paused while seated (vehicle, turret): re-anchor where they get out.
        bool seated = p.count > 0 && Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT) != 0xFFFFFFFF;
        bool sameUnit = p.count > 0 && p.datums[0] == g_noclipUnit && !seated;
        if (g_noclipActive && (!on || !sameUnit)) {
            g_noclipActive = false;
            LOG("Reach: noclip off");
        }
        if (!on || p.count == 0 || seated) return;

        char* unit = p.units[0];
        float* pos = &Field<float>(unit, REACH_OBJECT_POSITION);
        if (!g_noclipActive) {
            memcpy(g_anchor, pos, sizeof(g_anchor));
            g_noclipUnit = p.datums[0];
            g_noclipActive = true;
            LOG("Reach: noclip on at %.2f %.2f %.2f", pos[0], pos[1], pos[2]);
        }

        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        if (in.noclipForward) {
            float step = kNoclipSpeed * (in.fast ? kNoclipFastScale : 1.0f) * dt;
            g_anchor[0] += cosf(yaw) * cosf(pitch) * step;
            g_anchor[1] += sinf(yaw) * cosf(pitch) * step;
            g_anchor[2] += sinf(pitch) * step;
        }

        // Not object_teleport: for a player it also runs a respawn-style move
        // that snaps the first-person view level every tick. Setting only the
        // position leaves the view alone. Velocity is cleared so gravity
        // doesn't build up between ticks.
        Engine<SetPositionFn>(OFFSET_REACH_OBJECT_SET_POSITION)(p.datums[0], g_anchor, nullptr, nullptr, 0, false);
        memset(&Field<float>(unit, REACH_OBJECT_VELOCITY), 0, 3 * sizeof(float));

        static ULONGLONG s_lastLog = 0;  // look angles, to diagnose noclip's view
        if (GetTickCount64() - s_lastLog > 2000) {
            s_lastLog = GetTickCount64();
            LOG("Reach: noclip view yaw %.2f pitch %.2f, anchor %.1f %.1f %.1f, unit at %.1f %.1f %.1f", yaw, pitch,
                g_anchor[0], g_anchor[1], g_anchor[2], pos[0], pos[1], pos[2]);
        }
    }

    // The target locator's hold-to-lock (a spew-charge trigger) finishes on
    // the next tick. Other charge weapons (plasma pistol, Spartan laser) keep
    // their charge.
    void SkipLockOn(char* weapon) {
        char* tag = ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM));
        int triggers = ReachTags::BlockCount(tag, REACH_WEAP_TRIGGERS);
        for (int t = 0; t < triggers && t < REACH_WEAPON_TRIGGER_COUNT; ++t) {
            char* def = ReachTags::BlockElement(tag, REACH_WEAP_TRIGGERS, REACH_WEAP_TRIGGER_DEF_SIZE, t);
            if (!def || Field<int16_t>(def, REACH_TRIGDEF_BEHAVIOR) != REACH_TRIGGER_SPEW_CHARGE) continue;
            char* trigger = weapon + REACH_WEAPON_TRIGGERS + t * REACH_WEAPON_TRIGGER_SIZE;
            auto& ticks = Field<int16_t>(trigger, REACH_TRIGGER_CHARGE_TICKS);
            if (Field<uint8_t>(trigger, REACH_TRIGGER_STATE) == REACH_TRIGGER_STATE_CHARGING && ticks > 1) ticks = 1;
        }
    }

    // Rapid fire: the players' weapons (rapid fire) and those of the vehicle
    // or turret each is in (vehicle rapid fire) are ready to shoot every
    // tick, ignoring their rate of fire.
    void RapidFire(const Players& p, const Array& objects, bool own, bool vehicles) {
        if (!own && !vehicles) return;
        for (int i = 0; i < p.count; ++i) {
            char* parts[16];
            int count = 0;
            if (own) parts[count++] = p.units[i];
            if (vehicles) count += VehicleParts(p.units[i], objects, parts + count, _countof(parts) - count);
            for (int k = 0; k < count; ++k)
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(parts[k], objects, w);
                if (!weapon) continue;
                for (int b = 0; b < REACH_WEAPON_BARREL_COUNT; ++b) {
                    char* barrel = weapon + REACH_WEAPON_BARRELS + b * REACH_WEAPON_BARREL_SIZE;
                    Field<int8_t>(barrel, REACH_BARREL_TICKS_SINCE_SHOT) = REACH_BARREL_TICKS_MAX;
                    Field<int16_t>(barrel, REACH_BARREL_FIRE_DELAY) = 0;
                }
                SkipLockOn(weapon);
            }
        }
    }

    using AirstrikeUpdateFn = void(__fastcall*)();

    // Strikes the mod moved out of the engine's single slot so the next one
    // can be called while they finish.
    constexpr int kMaxParkedStrikes = 8;
    char g_parked[kMaxParkedStrikes][REACH_AIRSTRIKE_SIZE];
    int g_parkedCount = 0;
    char* g_airstrikeSlot = nullptr;

    // The cheats' edits to one strike. Infinite ammo makes it free (and
    // refills a used-up locator); rapid fire skips the delay, the pause and
    // the cooldown. The gaps between missiles stay: without them all 7 land
    // at once and look like one (tested 2026-10-03).
    void EditStrike(char* a, bool infinite, bool rapid) {
        auto& phase = Field<int16_t>(a, REACH_AIRSTRIKE_STATE);
        auto& launches = Field<int32_t>(a, REACH_AIRSTRIKE_LAUNCHES);
        if (infinite) {
            if (phase == REACH_AIRSTRIKE_STATE_DELAY) Field<uint8_t>(a, REACH_AIRSTRIKE_FREE) = 1;
            if (phase == REACH_AIRSTRIKE_STATE_EMPTY && launches == 0) {
                launches = 1;
                phase = REACH_AIRSTRIKE_STATE_READY;
            }
        }
        if (rapid) {
            if (phase >= REACH_AIRSTRIKE_STATE_DELAY && phase <= REACH_AIRSTRIKE_STATE_PAUSE)
                Field<uint8_t>(a, REACH_AIRSTRIKE_NO_COOLDOWN) = 1;
            if (phase == REACH_AIRSTRIKE_STATE_DELAY || phase == REACH_AIRSTRIKE_STATE_PAUSE ||
                phase == REACH_AIRSTRIKE_STATE_COOLDOWN)
                Field<int32_t>(a, REACH_AIRSTRIKE_TIMER) = 0;
        }
    }

    // The target locator's airstrikes (see offsets.h). With rapid fire, a
    // strike in progress is parked here and the engine's slot reset to ready,
    // so a new one can be called before it lands. Parked strikes are stepped
    // once a tick by swapping each into the slot and running the engine's own
    // update (as the game tick does), then swapping the real one back.
    void Airstrike(void** state, bool infinite, bool rapid) {
        char* a = (char*)state[REACH_TLS_SLOT_AIRSTRIKE];
        if (a != g_airstrikeSlot || !a || !Field<uint8_t>(a, REACH_AIRSTRIKE_ENABLED)) {
            g_airstrikeSlot = a;  // new map, or airstrikes off: drop parked strikes
            g_parkedCount = 0;
        }
        if (!a || !Field<uint8_t>(a, REACH_AIRSTRIKE_ENABLED)) return;
        if (infinite || rapid) EditStrike(a, infinite, rapid);

        auto& phase = Field<int16_t>(a, REACH_AIRSTRIKE_STATE);
        auto& launches = Field<int32_t>(a, REACH_AIRSTRIKE_LAUNCHES);
        if (rapid && phase == REACH_AIRSTRIKE_STATE_DELAY && g_parkedCount < kMaxParkedStrikes) {
            char* parked = g_parked[g_parkedCount++];
            memcpy(parked, a, REACH_AIRSTRIKE_SIZE);
            // Charge the launch now, so the slot can't hand out more strikes
            // than are left while this one waits.
            if (!Field<uint8_t>(a, REACH_AIRSTRIKE_FREE) && launches > 0) --launches;
            Field<uint8_t>(parked, REACH_AIRSTRIKE_FREE) = 1;
            phase = REACH_AIRSTRIKE_STATE_READY;
            Field<uint8_t>(a, REACH_AIRSTRIKE_FREE) = 0;
            Field<uint8_t>(a, REACH_AIRSTRIKE_NO_COOLDOWN) = 0;
            Field<int32_t>(a, REACH_AIRSTRIKE_TIMER) = -1;
            Field<int32_t>(a, REACH_AIRSTRIKE_SHOTS) = 0;
        }
        if (!g_parkedCount) return;

        char real[REACH_AIRSTRIKE_SIZE];
        memcpy(real, a, REACH_AIRSTRIKE_SIZE);
        int kept = 0;
        for (int i = 0; i < g_parkedCount; ++i) {
            char* parked = g_parked[i];
            EditStrike(parked, infinite, rapid);
            memcpy(a, parked, REACH_AIRSTRIKE_SIZE);
            __try {
                Engine<AirstrikeUpdateFn>(OFFSET_REACH_AIRSTRIKE_UPDATE)();
            } __except (Log::Exception(GetExceptionInformation(), "Reach parked airstrike")) {
                memcpy(a, real, REACH_AIRSTRIKE_SIZE);
                g_parkedCount = 0;
                return;
            }
            memcpy(parked, a, REACH_AIRSTRIKE_SIZE);
            int16_t p = Field<int16_t>(parked, REACH_AIRSTRIKE_STATE);
            // Done once it's past the strike (cooldown, ready or out of launches).
            if (p >= REACH_AIRSTRIKE_STATE_DELAY && p <= REACH_AIRSTRIKE_STATE_PAUSE) {
                if (kept != i) memcpy(g_parked[kept], parked, REACH_AIRSTRIKE_SIZE);
                ++kept;
            }
        }
        g_parkedCount = kept;
        memcpy(a, real, REACH_AIRSTRIKE_SIZE);
    }

    // "Invulnerable AI can die": the mission's AI lose the scripts' cannot-die
    // flag and come off the revive list, so they die and stay dead. Undone
    // when turned off, for those still alive. AI the mod spawned are left to
    // "Enable respawn" (FollowSpawnedAi).
    // Sword Base's scripts also place a new Kat some time after she dies (a
    // new unit, on the revive list again; seen 2026-10-03). So the character
    // of each AI that dies is remembered, and a new protected unit of that
    // character is deleted as it appears.
    struct RemovedRevive {
        uint32_t unit;
        char entry[REACH_AI_REVIVE_SIZE];
    };
    RemovedRevive g_removedRevives[REACH_AI_REVIVE_COUNT];
    int g_removedReviveCount = 0;
    struct Unprotected {
        uint32_t unit;
        uint32_t character;  // 'char' tag, -1 if its actor wasn't found
        bool cannotDie;      // we cleared its cannot-die flag
        bool dead;
    };
    constexpr int kMaxUnprotected = 64;
    Unprotected g_unprotected[kMaxUnprotected];
    int g_unprotectedCount = 0;
    constexpr int kMaxDeadCharacters = 32;
    uint32_t g_deadCharacters[kMaxDeadCharacters];
    int g_deadCharacterCount = 0;
    bool g_stayDeadApplied = false;

    bool IsSpawnedAi(uint32_t unit) {
        for (auto& ai : g_spawnedAi)
            if (ai.unit == unit) return true;
        return false;
    }

    void ClearRevive(char* entry) {
        static const uint8_t kEmpty[REACH_AI_REVIVE_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                             0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
                                                             0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        memcpy(entry, kEmpty, sizeof(kEmpty));
    }

    uint32_t CharacterOfUnit(void** state, uint32_t unit) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return 0xFFFFFFFF;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            if (*(uint16_t*)actor && Field<uint32_t>(actor, REACH_ACTOR_UNIT) == unit)
                return Field<uint32_t>(actor, REACH_ACTOR_CHARACTER);
        }
        return 0xFFFFFFFF;
    }

    Unprotected* FindUnprotected(uint32_t unit) {
        for (int i = 0; i < g_unprotectedCount; ++i)
            if (g_unprotected[i].unit == unit) return &g_unprotected[i];
        return nullptr;
    }

    // A protected AI unit: tracked, or deleted if its character already died
    // while the toggle was on. Returns false if it was deleted.
    bool Unprotect(void** state, uint32_t unit) {
        if (FindUnprotected(unit)) return true;
        uint32_t character = CharacterOfUnit(state, unit);
        for (int i = 0; i < g_deadCharacterCount && character != 0xFFFFFFFF; ++i) {
            if (g_deadCharacters[i] != character) continue;
            LOG("Reach: AI stay dead: deleted %s (unit %08X), placed again after dying",
                ReachTags::FullName(character).c_str(), unit);
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(unit);
            return false;
        }
        if (g_unprotectedCount < kMaxUnprotected) g_unprotected[g_unprotectedCount++] = {unit, character, false, false};
        LOG("Reach: AI stay dead: %s (unit %08X) can die now",
            character != 0xFFFFFFFF ? ReachTags::FullName(character).c_str() : "unknown character", unit);
        return true;
    }

    void AiStayDead(void** state, const Array& objects, bool on) {
        char* ai = (char*)state[REACH_TLS_SLOT_AI_GLOBALS];
        if (!ai) return;
        auto alive = [&](uint32_t datum) -> char* {
            char* unit = ObjectFromDatum(objects, datum);
            return unit && !(Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS) & REACH_DAMAGE_FLAG_DEAD) ? unit : nullptr;
        };

        if (!on) {
            if (!g_stayDeadApplied) return;
            for (int i = 0; i < g_unprotectedCount; ++i) {
                char* unit = alive(g_unprotected[i].unit);
                if (unit && g_unprotected[i].cannotDie)
                    Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS) |= REACH_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE;
            }
            for (int r = 0; r < g_removedReviveCount; ++r) {
                char* unit = alive(g_removedRevives[r].unit);
                if (!unit) continue;
                for (int i = 0; i < REACH_AI_REVIVE_COUNT; ++i) {
                    char* entry = ai + REACH_AI_REVIVES + i * REACH_AI_REVIVE_SIZE;
                    if (Field<uint32_t>(entry, REACH_AI_REVIVE_UNIT) != 0xFFFFFFFF) continue;
                    memcpy(entry, g_removedRevives[r].entry, REACH_AI_REVIVE_SIZE);
                    Field<uint8_t>(unit, REACH_OBJECT_REVIVABLE) |= 1;
                    break;
                }
            }
            LOG("Reach: AI stay dead off: restored %d AI, %d revive entries", g_unprotectedCount,
                g_removedReviveCount);
            g_unprotectedCount = g_removedReviveCount = g_deadCharacterCount = 0;
            g_stayDeadApplied = false;
            return;
        }
        g_stayDeadApplied = true;

        // Deaths: remember the character so the mission can't bring it back.
        for (int i = 0; i < g_unprotectedCount; ++i) {
            Unprotected& u = g_unprotected[i];
            if (u.dead || alive(u.unit)) continue;
            u.dead = true;
            bool known = u.character == 0xFFFFFFFF;
            for (int k = 0; k < g_deadCharacterCount && !known; ++k) known = g_deadCharacters[k] == u.character;
            if (!known && g_deadCharacterCount < kMaxDeadCharacters) g_deadCharacters[g_deadCharacterCount++] = u.character;
            LOG("Reach: AI stay dead: unit %08X died", u.unit);
        }

        for (int i = 0; i < REACH_AI_REVIVE_COUNT; ++i) {
            char* entry = ai + REACH_AI_REVIVES + i * REACH_AI_REVIVE_SIZE;
            uint32_t datum = Field<uint32_t>(entry, REACH_AI_REVIVE_UNIT);
            if (datum == 0xFFFFFFFF || IsSpawnedAi(datum)) continue;
            char saved[REACH_AI_REVIVE_SIZE];
            memcpy(saved, entry, REACH_AI_REVIVE_SIZE);
            ClearRevive(entry);
            if (char* unit = ObjectFromDatum(objects, datum)) Field<uint8_t>(unit, REACH_OBJECT_REVIVABLE) &= ~1;
            LOG("Reach: AI stay dead: took unit %08X off the revive list", datum);
            if (Unprotect(state, datum) && g_removedReviveCount < REACH_AI_REVIVE_COUNT) {
                g_removedRevives[g_removedReviveCount].unit = datum;
                memcpy(g_removedRevives[g_removedReviveCount++].entry, saved, REACH_AI_REVIVE_SIZE);
            }
        }

        // Script immortality, on AI bipeds only (scripted vehicles and
        // objects keep theirs).
        for (int i = 0; i < objects.HighWater(); ++i) {
            char* e = objects.Entry(i);
            uint16_t salt = *(uint16_t*)e;
            char* unit = Field<char*>(e, REACH_OBJECT_ENTRY_OBJECT);
            if (!salt || !unit || Field<uint8_t>(e, REACH_OBJECT_ENTRY_TYPE) != REACH_OBJECT_TYPE_BIPED) continue;
            if (Field<int32_t>(unit, REACH_UNIT_PLAYER_INDEX) != -1) continue;
            auto& flags = Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS);
            if (!(flags & REACH_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE)) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (IsSpawnedAi(datum)) continue;
            flags &= ~REACH_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE;
            if (!Unprotect(state, datum)) continue;
            if (Unprotected* u = FindUnprotected(datum)) u->cannotDie = true;
        }
    }

    // Scales the health and shield enemies lost since last tick. Kills them
    // properly if the scaled damage is fatal.
    void Damage(const Players& p, const Array& objects, float m) {
        if (m == 1.0f) {
            memset(g_tracked, 0, sizeof(g_tracked));
            return;
        }

        for (int i = 0; i < objects.HighWater() && i < kMaxTracked; ++i) {
            char* entry = objects.Entry(i);
            uint16_t salt = *(uint16_t*)entry;
            int type = Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE);
            char* object = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            Tracked& t = g_tracked[i];
            float* health = object ? Health(object) : nullptr;
            if (salt == 0 || !health || !IsKillableEnemy(object, type, p)) {
                t.datum = 0;
                continue;
            }

            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            float* shield = Shield(object);
            float shieldNow = shield ? *shield : 0.0f;
            if (t.datum != datum) {
                t = {datum, *health, shieldNow};
                continue;
            }
            if (*health <= 0.0f) {  // already dead
                t.datum = 0;
                continue;
            }

            if (shield && shieldNow < t.shield) {
                float s = t.shield - (t.shield - shieldNow) * m;
                *shield = s > 0.0f ? s : 0.0f;
            }
            if (*health < t.health) {
                float h = t.health - (t.health - *health) * m;
                if (h <= 0.0f) {
                    *health = 0.0001f;
                    Engine<UnitKillFn>(OFFSET_REACH_UNIT_KILL)(datum, 0, true, 2, 0);
                    t.datum = 0;
                    continue;
                }
                *health = h;
            }
            t.health = *health;
            t.shield = shield ? *shield : 0.0f;
        }
    }

    // Launch on hit: an enemy whose health or shield dropped since last tick
    // (or who just died) gets `speed` added to its upward velocity through the
    // engine's set-velocity, so the physics (a standing biped's too) takes it.
    // Enemies are followed from when they're first seen as AI with an actor;
    // a unit that dies loses its actor, so a tracked one needs only to exist.
    Tracked g_launchTracked[kMaxTracked];

    void LaunchOnHit(const Players& p, const Array& objects, float speed, bool on) {
        if (!on) {
            memset(g_launchTracked, 0, sizeof(g_launchTracked));
            return;
        }
        for (int i = 0; i < objects.HighWater() && i < kMaxTracked; ++i) {
            char* entry = objects.Entry(i);
            uint16_t salt = *(uint16_t*)entry;
            int type = Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE);
            char* object = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            Tracked& t = g_launchTracked[i];
            float* health = object && type == REACH_OBJECT_TYPE_BIPED ? Health(object) : nullptr;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            bool known = salt && t.datum == datum;
            if (!salt || !health || p.Contains(object) ||
                (!known && (Friends(Field<int8_t>(object, REACH_UNIT_TEAM), PlayerTeam(p)) ||
                            Field<uint32_t>(object, REACH_UNIT_ACTOR) == 0xFFFFFFFF))) {
                t.datum = 0;
                continue;
            }
            float* shield = Shield(object);
            float shieldNow = shield ? *shield : 0.0f;
            if (!known) {
                t = {datum, *health, shieldNow};
                continue;
            }
            bool hit = *health < t.health - 1e-4f || shieldNow < t.shield - 1e-4f;
            bool died = *health <= 0.0f;
            t.health = *health;
            t.shield = shieldNow;
            if (died) t.datum = 0;  // launched once, on the blow that killed it
            if (!hit || ObjectFrozen(datum)) continue;  // not while time is stopped
            const float* now = &Field<float>(object, REACH_OBJECT_VELOCITY);
            float velocity[3] = {now[0], now[1], (now[2] > 0.0f ? now[2] : 0.0f) + speed};
            Engine<SetVelocityFn>(OFFSET_REACH_OBJECT_SET_VELOCITY)(datum, velocity, nullptr);
        }
    }

    // A give waiting for the game to free the held weapon's slot. Deleting
    // an object takes effect a tick or so later.
    struct PendingGive {
        bool active = false;
        uint32_t weapon = 0xFFFFFFFF;  // new weapon, already created
        uint32_t held = 0xFFFFFFFF;    // weapon being replaced (deleted)
        int frames = 0;
    };
    PendingGive g_give;

    int SlotOf(char* unit, uint32_t weapon) {
        for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w)
            if (Field<uint32_t>(unit, REACH_UNIT_WEAPONS + w * 4) == weapon) return w;
        return -1;
    }

    // Adds the weapon into a free slot and switches to it. Deletes it if the
    // game won't take it.
    void AddAndSelect(char* unit, uint32_t unitDatum, uint32_t weapon) {
        bool added = Engine<UnitAddWeaponFn>(OFFSET_REACH_UNIT_ADD_WEAPON)(unitDatum, weapon, REACH_ADD_WEAPON_FREE_SLOT);
        int slot = added ? SlotOf(unit, weapon) : -1;
        if (slot >= 0) Field<int8_t>(unit, REACH_UNIT_DESIRED_WEAPON) = (int8_t)slot;
        else Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(weapon);
        LOG("Reach: give weapon: %s (object %08X, slot %d)", slot >= 0 ? "added" : "add FAILED", weapon, slot);
    }

    uint32_t& ActiveZones() { return *(uint32_t*)(g_module + OFFSET_REACH_DESIGNER_ZONES_ACTIVE); }

    // The designer zones whose lists include a tag, as a bit mask.
    uint32_t DesignerZonesOf(uint32_t tag) {
        char* scnr = ReachTags::Scenario();
        if (!scnr) return 0;
        int list, palette;
        switch (ReachTags::GroupOf(tag)) {
            case REACH_TAG_CHAR: list = REACH_DESIGNER_ZONE_CHARACTERS; palette = REACH_SCNR_CHARACTER_PALETTE; break;
            case REACH_TAG_WEAP: list = REACH_DESIGNER_ZONE_WEAPONS; palette = REACH_SCNR_WEAPON_PALETTE; break;
            case REACH_TAG_VEHI: list = REACH_DESIGNER_ZONE_VEHICLES; palette = REACH_SCNR_VEHICLE_PALETTE; break;
            case REACH_TAG_BIPD: list = REACH_DESIGNER_ZONE_BIPEDS; palette = REACH_SCNR_BIPED_PALETTE; break;
            case REACH_TAG_EQIP: list = REACH_DESIGNER_ZONE_EQUIPMENT; palette = REACH_SCNR_EQUIPMENT_PALETTE; break;
            case REACH_TAG_SCEN: list = REACH_DESIGNER_ZONE_SCENERY; palette = REACH_SCNR_SCENERY_PALETTE; break;
            case REACH_TAG_MACH: list = REACH_DESIGNER_ZONE_MACHINES; palette = REACH_SCNR_MACHINE_PALETTE; break;
            case REACH_TAG_BLOC: list = REACH_DESIGNER_ZONE_CRATES; palette = REACH_SCNR_CRATE_PALETTE; break;
            default: return 0;
        }
        uint32_t zones = 0;
        int count = ReachTags::BlockCount(scnr, REACH_SCNR_DESIGNER_ZONES);
        for (int z = 0; z < count && z < 32; ++z) {
            char* zone = ReachTags::BlockElement(scnr, REACH_SCNR_DESIGNER_ZONES, REACH_SCNR_DESIGNER_ZONE_SIZE, z);
            int entries = zone ? ReachTags::BlockCount(zone, list) : 0;
            for (int i = 0; i < entries; ++i) {
                char* index = ReachTags::BlockElement(zone, list, sizeof(int16_t), i);
                char* entry = index ? ReachTags::BlockElement(scnr, palette, REACH_PALETTE_ENTRY_SIZE, *(int16_t*)index)
                                    : nullptr;
                if (entry && Field<uint32_t>(entry, REACH_PALETTE_ENTRY_DATUM) == tag) zones |= 1u << z;
            }
        }
        return zones;
    }

    // The designer zone to load so a tag the game couldn't make can be
    // made, as a bit; 0 if no zone would help (it's in none, or in one
    // that's already loaded).
    uint32_t ZoneToLoad(uint32_t tag) {
        uint32_t zones = DesignerZonesOf(tag);
        if (!zones || (zones & ActiveZones())) return 0;
        return zones & (0u - zones);  // the first
    }

    // Has the game load designer zones on its next frame (and keeps them).
    void LoadZones(uint32_t zones) {
        g_forcedZones |= zones;
        uint32_t missing = zones & ~ActiveZones();
        if (!missing) return;
        uint32_t* pending = (uint32_t*)(g_module + OFFSET_REACH_DESIGNER_ZONES_PENDING);
        pending[0] &= ~missing;  // deactivate
        pending[1] |= missing;   // activate
        *(uint8_t*)(g_module + OFFSET_REACH_ZONE_UPDATE_PENDING) = 1;
    }

    // Keeps the zones the mod loaded: no zone set unloads them, and any the
    // game dropped anyway (a checkpoint load) are loaded again.
    void KeepForcedZones() {
        char* scnr = ReachTags::Scenario();
        if (scnr != g_forcedScenario) {  // a new map
            g_forcedScenario = scnr;
            g_forcedZones = 0;
        }
        if (!g_forcedZones || !scnr) return;
        int count = ReachTags::BlockCount(scnr, REACH_SCNR_ZONE_SETS);
        for (int i = 0; i < count; ++i)
            if (char* set = ReachTags::BlockElement(scnr, REACH_SCNR_ZONE_SETS, REACH_SCNR_ZONE_SET_SIZE, i))
                Field<uint32_t>(set, REACH_ZONE_SET_DESIGNER_UNLOADED) &= ~g_forcedZones;
        if (g_forcedZones & ~ActiveZones()) {
            LOG("Reach: designer zones %08X were unloaded; loading them again", g_forcedZones & ~ActiveZones());
            LoadZones(g_forcedZones);
        }
    }

    std::string ShortName(uint32_t tag) {
        std::string name = ReachTags::FullName(tag);
        size_t slash = name.find_last_of('\\');
        return slash != std::string::npos ? name.substr(slash + 1) : name;
    }

    // A tag in no designer zone that the game couldn't make: it only loads
    // with another part of the level.
    void NotLoaded(uint32_t tag) {
        char text[96];
        sprintf_s(text, "%s: not loaded in this area", ShortName(tag).c_str());
        SpawnResult(text);
    }

    // Replaces player 1's held weapon with a new one of the chosen type (or
    // fills a free slot). The new weapon is created first, so a type the
    // game can't create costs nothing.
    // False if the weapon couldn't be created (`quiet`: don't log that).
    bool GiveWeapon(const Players& p, const Array& objects, uint32_t weaponTag, bool quiet = false) {
        if (p.count == 0) { LOG("Reach: give weapon: no player unit"); return true; }
        if (!ReachTags::Get(weaponTag)) { LOG("Reach: give weapon %08X: tag not loaded", weaponTag); return true; }
        char* unit = p.units[0];
        uint32_t unitDatum = p.datums[0];

        if (g_give.active) {  // replaced by this newer request
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(g_give.weapon);
            g_give = {};
        }

        static uint8_t placement[REACH_PLACEMENT_DATA_SIZE];
        memset(placement, 0, sizeof(placement));
        Engine<PlacementNewFn>(OFFSET_REACH_OBJECT_PLACEMENT_NEW)(placement, weaponTag, unitDatum, 0);
        uint32_t weapon = Engine<ObjectNewFn>(OFFSET_REACH_OBJECT_NEW)(placement);
        if (weapon == 0xFFFFFFFF) {
            if (!quiet) LOG("Reach: give weapon %08X: the game couldn't create it here", weaponTag);
            return false;
        }

        bool freeSlot = false;
        for (int w = 0; w < REACH_PLAYER_WEAPON_SLOTS; ++w)
            freeSlot |= Field<uint32_t>(unit, REACH_UNIT_WEAPONS + w * 4) == 0xFFFFFFFF;

        uint32_t held = 0xFFFFFFFF;
        if (freeSlot || !HeldWeapon(unit, objects, &held)) {
            AddAndSelect(unit, unitDatum, weapon);
            return true;
        }

        Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(held);
        g_give = {true, weapon, held, 0};
        LOG("Reach: give weapon %08X: removing held %08X, adding once its slot frees", weaponTag, held);
        return true;
    }

    // Replaces player 1's armor ability with the chosen equipment, the way
    // the starting-loadout code hands one out. The new one is created first,
    // so a tag the game can't create costs nothing. False if it couldn't be.
    bool GiveEquipment(const Players& p, const Array& objects, uint32_t equipmentTag) {
        if (p.count == 0) { LOG("Reach: give equipment: no player unit"); return true; }
        if (!ReachTags::Get(equipmentTag)) { LOG("Reach: give equipment %08X: tag not loaded", equipmentTag); return true; }
        char* unit = p.units[0];
        uint32_t unitDatum = p.datums[0];

        static uint8_t placement[REACH_PLACEMENT_DATA_SIZE];
        memset(placement, 0, sizeof(placement));
        Engine<PlacementNewFn>(OFFSET_REACH_OBJECT_PLACEMENT_NEW)(placement, equipmentTag, unitDatum, 0);
        uint32_t equipment = Engine<ObjectNewFn>(OFFSET_REACH_OBJECT_NEW)(placement);
        if (equipment == 0xFFFFFFFF) {
            LOG("Reach: give equipment %08X: the game couldn't create it here", equipmentTag);
            return false;
        }

        if (Field<uint32_t>(unit, REACH_UNIT_EQUIPMENT) != 0xFFFFFFFF)
            Engine<UnitRemoveEquipmentFn>(OFFSET_REACH_UNIT_REMOVE_EQUIPMENT)(unitDatum);

        if (char* object = ObjectOfType(objects, equipment, REACH_OBJECT_TYPE_EQUIPMENT))
            Field<float>(object, REACH_EQUIPMENT_ENERGY) = 1.0f;
        Engine<EquipmentInitFn>(OFFSET_REACH_EQUIPMENT_INIT)(equipment);
        Engine<EquipmentSetFlagsFn>(OFFSET_REACH_EQUIPMENT_SET_FLAGS)(equipment, REACH_EQUIPMENT_FLAGS_VALUE);
        Engine<UnitAddEquipmentFn>(OFFSET_REACH_UNIT_ADD_EQUIPMENT)(unitDatum, equipment);
        LOG("Reach: give equipment %s (object %08X): unit now holds %08X", ShortName(equipmentTag).c_str(), equipment,
            Field<uint32_t>(unit, REACH_UNIT_EQUIPMENT));
        return true;
    }

    // Finishes a pending give once the held weapon has left the inventory.
    void ContinueGive(const Players& p) {
        if (!g_give.active) return;
        if (p.count == 0) {  // died: drop the request
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(g_give.weapon);
            g_give = {};
            return;
        }
        char* unit = p.units[0];
        if (SlotOf(unit, g_give.held) < 0) {
            AddAndSelect(unit, p.datums[0], g_give.weapon);
            g_give = {};
        } else if (++g_give.frames > 120) {
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(g_give.weapon);
            LOG("Reach: give weapon: held weapon never left the inventory; gave up");
            g_give = {};
        }
    }

    // Aimbot strength: while firing, turns player 1's view toward the enemy
    // head nearest the crosshair (see the Halo 3 version).
    void AimPull(void** state, const Players& p, const Array& objects, float strength) {
        if (strength <= 0.0f || p.count == 0) return;
        if (IsMeleeOnly(HeldWeapon(p.units[0], objects))) return;  // sword or hammer: no pull
        float s = strength / 100.0f;
        auto head = Engine<HeadPositionFn>(OFFSET_REACH_UNIT_HEAD_POSITION);

        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float& yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float& pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        float forward[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        head(p.datums[0], eye);

        float minCos = cosf((kAimMinCone + (kAimMaxCone - kAimMinCone) * s) * kPi / 180.0f);
        float best[3] = {};
        float bestCos = minCos;
        bool found = false;
        for (int i = 0; i < objects.HighWater(); ++i) {
            char* entry = objects.Entry(i);
            uint16_t salt = *(uint16_t*)entry;
            int type = Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE);
            char* object = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            if (salt == 0 || !object || !IsKillableEnemy(object, type, p)) continue;
            float* health = Health(object);
            if (!health || *health <= 0.0f) continue;

            float pos[3];
            head(((uint32_t)salt << 16) | (uint32_t)i, pos);
            float d[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 0.1f || len > kAimRange) continue;
            float c = (d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2]) / len;
            if (c <= bestCos) continue;
            bestCos = c;
            memcpy(best, d, sizeof(best));
            found = true;
        }
        if (!found) return;

        float targetYaw = atan2f(best[1], best[0]);
        float targetPitch = atan2f(best[2], sqrtf(best[0] * best[0] + best[1] * best[1]));
        float dyaw = remainderf(targetYaw - yaw, 2.0f * kPi);  // shortest way round
        float turn = s * s;                                      // fraction of the gap closed per tick
        yaw = remainderf(yaw + dyaw * turn, 2.0f * kPi);
        pitch += (targetPitch - pitch) * turn;
    }

    // Spawning: creates the selected tag where player 1 is aiming. A ray from
    // their head along the view (the engine's own ray test, the observer
    // camera's flags) finds the spot; the object is placed a little back
    // from it and above it.
    // An AI character (a 'char' tag) as a squadless actor, the way the
    // engine's own free spawns make them (0x73152C): full AI, its own
    // weapons, facing the player. Returns the actor datum or -1.
    // Player 1's team this tick: "Ally" and "Enemy" picks follow it, so with
    // "Your team" on Covenant an ally is a Covenant and an enemy a human.
    int g_myTeam = REACH_TEAM_PLAYER;

    int AllyTeam() { return HumanSide(g_myTeam) ? REACH_TEAM_HUMAN : g_myTeam; }
    int EnemyTeam() { return g_myTeam == REACH_TEAM_COVENANT ? REACH_TEAM_HUMAN : REACH_TEAM_COVENANT; }

    // Random teams (the Random AI teams feature, and the spawn picker's Random):
    // humans are one faction; covenant, brute, mule, spare and covenant_player
    // are all enemies of each other and of humans (see offsets.h).
    const int kFactionTeams[] = {REACH_TEAM_HUMAN, REACH_TEAM_COVENANT, REACH_TEAM_BRUTE,
                                 REACH_TEAM_MULE,  REACH_TEAM_SPARE,    REACH_TEAM_COVENANT_PLAYER};
    float Random01();

    // The one resolver for the spawn picker (Settings::spawnTeam): 0 their own
    // team, 1 Ally, 2 Covenant (the enemy side), 3 Flood (not in Reach: own
    // team), 4 Rogue, 5 Random. Returns the engine team to set, or -1 to leave
    // the AI on its own. Random and Rogue roll per call (a respawn rolls again).
    // Rogue: a team nobody is friends with (Friends() is only same team or the
    // human side), spare or, if player 1 is on spare, mule; the two alternate
    // at random so rogues also fight each other (same team would be friends).
    int ResolveSpawnTeam(int choice) {
        switch (choice) {
        case 1: return AllyTeam();
        case 2: return EnemyTeam();
        case 4: {
            int a = g_myTeam == REACH_TEAM_SPARE ? REACH_TEAM_MULE : REACH_TEAM_SPARE;
            int b = g_myTeam == REACH_TEAM_MULE ? REACH_TEAM_SPARE : REACH_TEAM_MULE;
            return Random01() < 0.5f ? a : b;
        }
        case 5: return kFactionTeams[(int)(Random01() * _countof(kFactionTeams)) % (int)_countof(kFactionTeams)];
        default: return -1;
        }
    }

    // Random guns: AI that can hold anything (Elites, Brutes, Grunts,
    // marines) get a random weapon from this map's list instead of the
    // character's own (Elites come with just an energy sword). Jackals and
    // Skirmishers carry a shield arm, so they keep theirs.
    bool g_randomGuns = true;
    uint32_t NewObject(uint32_t tag, uint32_t variant, const float* position, float yaw);

    bool WieldsAnything(uint32_t character) {
        std::string name = ShortName(character);
        if (name.find("chieftain") != std::string::npos) return false;
        for (const char* prefix : {"elite", "brute", "grunt", "trooper"})
            if (name.rfind(prefix, 0) == 0) return true;
        return false;
    }

    float Random01();

    // Character/weapon pairs the engine refused (the unit ended up unarmed),
    // so they aren't picked again this session.
    std::set<uint64_t> g_badGuns;
    uint64_t GunKey(uint32_t character, uint32_t gun) { return ((uint64_t)character << 32) | gun; }

    // Characters that come with no weapon, or the wrong one, when spawned
    // alone (jackal snipers get a plasma pistol, chieftains nothing: a
    // mission squad normally gives them theirs): the weapons they normally
    // carry, one picked at random.
    struct FixedGun {
        const char* character;
        const char* weapons[4];  // short names; null ends the list
    };
    const FixedGun kFixedGuns[] = {
        {"jackal_sniper", {"focus_rifle"}},
        {"brute_chieftain_weapon", {"flak_cannon", "plasma_launcher", "gravity_hammer"}},  // fuel rod, Covenant grenade launcher
        {"brute_chieftain_armor", {"flak_cannon", "plasma_launcher", "gravity_hammer"}},
    };

    // One of a character's usual weapons at random, or -1.
    uint32_t FixedGunFor(uint32_t character) {
        std::string name = ShortName(character);
        for (const FixedGun& fixed : kFixedGuns) {
            if (name != fixed.character) continue;
            std::vector<uint32_t> all;
            for (uint32_t weapon : WeaponTags()) {
                std::string weaponName = ShortName(weapon);
                for (const char* w : fixed.weapons)
                    if (w && weaponName == w && !g_badGuns.count(GunKey(character, weapon))) all.push_back(weapon);
            }
            if (!all.empty()) return all[(size_t)(Random01() * all.size()) % all.size()];
        }
        return 0xFFFFFFFF;
    }

    // The designer zones to load for every usual weapon of a character that
    // isn't loaded (as a mask), so a random pick never has to wait twice.
    uint32_t FixedGunZones(uint32_t character) {
        std::string name = ShortName(character);
        uint32_t need = 0;
        for (const FixedGun& fixed : kFixedGuns) {
            if (name != fixed.character) continue;
            for (uint32_t weapon : WeaponTags()) {
                std::string weaponName = ShortName(weapon);
                for (const char* w : fixed.weapons)
                    if (w && weaponName == w) {
                        uint32_t zones = DesignerZonesOf(weapon);
                        if (zones && !(zones & ActiveZones())) need |= zones & (0u - zones);
                    }
            }
        }
        return need;
    }

    // The zone to load before a character can be spawned: its own, or the one
    // holding the weapon it must carry (see kFixedGuns).
    uint32_t SpawnZone(uint32_t tag) {
        uint32_t zone = ZoneToLoad(tag);
        if (ReachTags::GroupOf(tag) == REACH_TAG_CHAR) zone |= FixedGunZones(tag);
        return zone;
    }

    // Weapons Elites, Brutes, Grunts and marines may be given at random: the
    // real guns. (The weapon list also has turrets, knives, shields, grenades.)
    bool RandomGunName(const std::string& name) {
        for (const char* gun : {"assault_rifle", "concussion_rifle", "dmr", "energy_sword", "flak_cannon",
                                "focus_rifle", "grenade_launcher", "magnum", "needle_rifle", "needler",
                                "plasma_launcher", "plasma_pistol", "plasma_rifle", "rocket_launcher", "shotgun",
                                "sniper_rifle", "spike_rifle", "spiker", "carbine", "plasma_repeater",
                                "gravity_hammer", "brute_shot", "fuel_rod_gun"})
            if (name == gun) return true;
        return false;
    }

    void** g_state = nullptr;  // this tick's TLS block, for checks inside SpawnActor

    // A random weapon tag this map can really make (made once and deleted
    // to check, since some are in resources that aren't loaded), or -1.
    uint32_t RandomGun(uint32_t character, const float* position, float yaw) {
        for (int tries = 0; tries < 30; ++tries) {
            uint32_t gun = RainPick(4, 0xFFFFFFFF);
            if (gun == 0xFFFFFFFF) return gun;
            if (!RandomGunName(ShortName(gun))) continue;
            if (g_badGuns.count(GunKey(character, gun))) continue;
            uint32_t zones = DesignerZonesOf(gun);
            if (zones && !(zones & ActiveZones())) continue;
            uint32_t object = NewObject(gun, 0xFFFFFFFF, position, yaw);
            if (object == 0xFFFFFFFF) continue;
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(object);
            return gun;
        }
        return 0xFFFFFFFF;
    }

    // Does the actor's unit hold any weapon? (The spawner adds them before it returns.)
    bool ActorArmed(uint32_t actor) {
        if (!g_state) return true;
        Array actors{(char*)g_state[REACH_TLS_SLOT_ACTORS]};
        Array objects{(char*)g_state[REACH_TLS_SLOT_OBJECTS]};
        char* a = actors.Valid() ? actors.Get(actor) : nullptr;
        char* unit = a ? ObjectFromDatum(objects, Field<uint32_t>(a, REACH_ACTOR_UNIT)) : nullptr;
        return !unit || Field<uint32_t>(unit, REACH_UNIT_WEAPONS) != 0xFFFFFFFF;
    }

    uint32_t SpawnActorWith(uint32_t character, const float* position, float yaw, uint32_t gun) {
        alignas(16) static uint8_t request[0x100];
        Engine<ActorRequestInitFn>(OFFSET_REACH_ACTOR_REQUEST_INIT)(request);
        memcpy(request + REACH_ACTOR_REQUEST_POSITION, position, 3 * sizeof(float));
        Field<float>((char*)request, REACH_ACTOR_REQUEST_YAW) = yaw;
        Field<float>((char*)request, REACH_ACTOR_REQUEST_PITCH) = 0.0f;
        Field<uint16_t>((char*)request, REACH_ACTOR_REQUEST_FLAGS) |= REACH_ACTOR_REQUEST_FLAG_FREE;
        Field<uint16_t>((char*)request, REACH_ACTOR_REQUEST_1E) = 2;
        Field<uint16_t>((char*)request, REACH_ACTOR_REQUEST_22) = 1;
        Field<uint32_t>((char*)request, REACH_ACTOR_REQUEST_CHARACTER) = character;
        if (gun != 0xFFFFFFFF) Field<uint32_t>((char*)request, REACH_ACTOR_REQUEST_PRIMARY) = gun;
        return Engine<ActorSpawnFn>(OFFSET_REACH_ACTOR_SPAWN)(-1, request);
    }

    uint32_t SpawnActor(uint32_t character, const float* position, float yaw, int team) {
        // The weapon to force, if any: a fixed one (jackal snipers), or a
        // random one. A pick the engine doesn't give the unit is dropped
        // (the AI deleted and made again) and not picked again.
        uint32_t actor = 0xFFFFFFFF;
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint32_t gun = 0xFFFFFFFF;
            if (attempt < 3) {
                gun = FixedGunFor(character);
                if (gun != 0xFFFFFFFF) {  // only if the map can make it, else the character's own
                    uint32_t zones = DesignerZonesOf(gun);
                    if (zones && !(zones & ActiveZones())) {
                        // Its designer zone isn't loaded: load it and fail this spawn; the callers'
                        // zone waits (SpawnZone) try again once it is.
                        LOG("Reach: %s needs %s, whose designer zone %08X isn't loaded; loading it",
                            ShortName(character).c_str(), ShortName(gun).c_str(), zones);
                        LoadZones(FixedGunZones(character) | (zones & (0u - zones)));
                        return 0xFFFFFFFF;
                    }
                    uint32_t object = g_badGuns.count(GunKey(character, gun)) ? 0xFFFFFFFF
                                                                               : NewObject(gun, 0xFFFFFFFF, position, yaw);
                    if (object != 0xFFFFFFFF) Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(object);
                    else gun = 0xFFFFFFFF;
                } else if (g_randomGuns && WieldsAnything(character)) {
                    gun = RandomGun(character, position, yaw);
                }
            }
            actor = SpawnActorWith(character, position, yaw, gun);
            if (actor == 0xFFFFFFFF) return actor;
            if (gun == 0xFFFFFFFF || ActorArmed(actor)) break;
            LOG("Reach: %s came out unarmed with %s; trying another", ShortName(character).c_str(),
                ShortName(gun).c_str());
            g_badGuns.insert(GunKey(character, gun));
            Engine<ActorEraseFn>(OFFSET_REACH_ACTOR_ERASE)(actor, false);
            actor = 0xFFFFFFFF;
        }
        if (actor == 0xFFFFFFFF) return actor;
        Engine<ActorSetActiveFn>(OFFSET_REACH_ACTOR_SET_ACTIVE)(actor, true);
        int resolved = ResolveSpawnTeam(team);
        if (resolved >= 0) Engine<ActorSetTeamFn>(OFFSET_REACH_ACTOR_SET_TEAM)(actor, resolved);
        return actor;
    }

    // Fills in a followed AI for an actor just spawned. False if it has no unit.
    bool Follow(SpawnedAi& ai, const Array& actors, const Array& objects, uint32_t actorDatum, uint32_t character,
                int team) {
        char* actor = actorDatum != 0xFFFFFFFF ? actors.Get(actorDatum) : nullptr;
        if (!actor) return false;
        ai = {};
        ai.actor = actorDatum;
        ai.unit = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
        ai.character = character;
        ai.team = team;
        return ObjectFromDatum(objects, ai.unit) != nullptr;
    }

    // What a spawn selection means: a tag, or a vehicle variant from the
    // Vehicles list (see VehicleChoice).
    VehicleChoice ChoiceFor(uint32_t selection) {
        VehicleChoice c;
        if (!FindVehicleChoice(selection, c)) c.vehicle = selection;
        return c;
    }

    std::string SelectionName(uint32_t selection) {
        VehicleChoice c = ChoiceFor(selection);
        return c.name.empty() ? ShortName(c.vehicle) : c.name;
    }

    uint32_t NewObject(uint32_t tag, uint32_t variant, const float* position, float yaw) {
        static uint8_t placement[REACH_PLACEMENT_DATA_SIZE];
        memset(placement, 0, sizeof(placement));
        Engine<PlacementNewFn>(OFFSET_REACH_OBJECT_PLACEMENT_NEW)(placement, tag, 0xFFFFFFFF, 0);
        Field<uint32_t>((char*)placement, REACH_PLACEMENT_VARIANT) = variant;
        memcpy(placement + REACH_PLACEMENT_POSITION, position, 3 * sizeof(float));
        float* forward = (float*)(placement + REACH_PLACEMENT_FORWARD);
        forward[0] = cosf(yaw);
        forward[1] = sinf(yaw);
        forward[2] = 0.0f;
        return Engine<ObjectNewFn>(OFFSET_REACH_OBJECT_NEW)(placement);
    }

    // Makes a spawn selection at a position, facing `yaw`: an AI (followed,
    // for respawn) or an object (a vehicle variant gets its turret
    // attached). Returns the actor or object datum, or -1.
    uint32_t Create(void** state, uint32_t selection, int team, const float* position, float yaw) {
        VehicleChoice c = ChoiceFor(selection);
        uint32_t tag = c.vehicle;
        if (ReachTags::GroupOf(tag) != REACH_TAG_CHAR) {
            uint32_t object = NewObject(tag, c.variant, position, yaw);
            if (object != 0xFFFFFFFF && c.turret != 0xFFFFFFFF) {
                uint32_t turret = NewObject(c.turret, 0, position, yaw);
                if (turret != 0xFFFFFFFF)
                    Engine<ObjectsAttachFn>(OFFSET_REACH_OBJECTS_ATTACH)(object, c.parentMarker, turret, c.childMarker);
                LOG("Reach: spawn %s: turret %08X -> %08X, attached at '%s'", c.name.c_str(), c.turret, turret,
                    ReachTags::StringName(c.parentMarker).c_str());
            }
            return object;
        }
        uint32_t actor = SpawnActor(tag, position, yaw, team);
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        Array objects{(char*)state[REACH_TLS_SLOT_OBJECTS]};
        SpawnedAi ai;
        if (Follow(ai, actors, objects, actor, tag, team)) {
            // A free slot, else the body that's been dead longest.
            SpawnedAi* free = nullptr;
            for (auto& slot : g_spawnedAi) {
                if (slot.unit == 0xFFFFFFFF || !ObjectFromDatum(objects, slot.unit)) { free = &slot; break; }
                if (slot.goneAt && (!free || slot.goneAt < free->goneAt)) free = &slot;
            }
            if (free) *free = ai;
        }
        return actor;
    }

    void Spawn(void** state, const Players& p, uint32_t selection, int team) {
        uint32_t tag = ChoiceFor(selection).vehicle;
        if (p.count == 0) { SpawnResult("No player to spawn for"); return; }
        if (!ReachTags::Get(tag)) { SpawnResult("Selection isn't loaded in this map"); return; }
        g_spawnWait = {};  // replaced by this newer request

        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        float dir[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        Engine<HeadPositionFn>(OFFSET_REACH_UNIT_HEAD_POSITION)(p.datums[0], eye);
        float ray[3] = {dir[0] * kSpawnRange, dir[1] * kSpawnRange, dir[2] * kSpawnRange};

        alignas(16) static uint8_t result[REACH_COLLISION_RESULT_SIZE];
        memset(result, 0, sizeof(result));
        uint32_t vehicle = Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT);
        bool hit = Engine<CollisionTestFn>(OFFSET_REACH_COLLISION_TEST_VECTOR)(
            REACH_COLLISION_FLAGS_CAMERA, false, eye, ray, (int32_t)p.datums[0], (int32_t)vehicle, -1, result);
        if (!hit) { SpawnResult("Nothing in range to spawn on"); return; }
        const float* at = (const float*)(result + REACH_COLLISION_RESULT_POINT);
        float position[3] = {at[0] - dir[0] * kSpawnBackOff, at[1] - dir[1] * kSpawnBackOff,
                             at[2] - dir[2] * kSpawnBackOff + kSpawnLift};
        float facing = remainderf(yaw + kPi, 2.0f * kPi);  // AI face the player
        uint32_t group = ReachTags::GroupOf(tag);
        if (group == REACH_TAG_VEHI || group == REACH_TAG_SCEN || group == REACH_TAG_MACH || group == REACH_TAG_BLOC) {
            // Backed off by its size so it isn't in a wall, kept off the
            // floor, and facing the way the player looks (they walk up
            // behind a vehicle).
            float radius = Field<float>(ReachTags::Get(tag), REACH_OBJE_BOUNDING_RADIUS);
            if (!(radius > 0.0f && radius < 100.0f)) radius = 2.0f;
            for (int k = 0; k < 3; ++k) position[k] = at[k] - dir[k] * radius;
            position[2] = (position[2] > at[2] ? position[2] : at[2]) + kVehicleLift;
            facing = yaw;
        } else if (ReachTags::GroupOf(tag) != REACH_TAG_CHAR) {
            facing = yaw;
        }

        uint32_t object = Create(state, selection, team, position, facing);
        LOG("Reach: spawn %08X at %.2f %.2f %.2f (hit %.2f %.2f %.2f, fraction %.3f) -> object/actor %08X", tag,
            position[0], position[1], position[2], at[0], at[1], at[2],
            *(const float*)(result + REACH_COLLISION_RESULT_FRACTION), object);
        std::string name = SelectionName(selection);
        char text[96];
        uint32_t zone = object == 0xFFFFFFFF ? SpawnZone(tag) : 0;
        if (zone) {
            LoadZones(zone);
            g_spawnWait.active = true;
            g_spawnWait.tag = selection;
            g_spawnWait.zone = zone;
            g_spawnWait.deadline = GetTickCount64() + kZoneLoadMs;
            g_spawnWait.team = team;
            memcpy(g_spawnWait.position, position, sizeof(position));
            g_spawnWait.yaw = facing;
            LOG("Reach: spawn %08X: loading designer zone %08X first", tag, zone);
            sprintf_s(text, "Loading %s...", name.c_str());
        } else if (object == 0xFFFFFFFF) {
            // In no designer zone: it only loads with another part of the level.
            sprintf_s(text, "%s: not loaded in this area", name.c_str());
        } else {
            sprintf_s(text, "Spawned %s", name.c_str());
        }
        SpawnResult(text);
    }

    // Squads: every member is made around the aim point, a loose cluster
    // (a sunflower spiral, so any size fits). Members whose character is in
    // a designer zone that isn't loaded wait for it, as a single spawn does.
    struct SquadMember {
        uint32_t tag;
        float position[3];
        float yaw;
    };
    std::vector<SquadMember> g_squadPending;
    uint32_t g_squadZones = 0;
    ULONGLONG g_squadDeadline = 0;
    int g_squadTeam = 0;
    std::string g_squadName;
    int g_squadMade = 0, g_squadTotal = 0;

    void SpawnSquad(void** state, const Players& p, int index, int team) {
        if (index < 0 || index >= Games::kSquadCount) return;
        const Games::Squad& squad = Games::kSquads[index];
        if (p.count == 0) { SpawnResult("No player to spawn for"); return; }
        std::vector<Games::TagChoice> characters;
        for (uint32_t tag : SpawnableCharacters()) characters.push_back({tag, ShortName(tag)});
        std::vector<uint32_t> tags = Games::SquadTags(squad, characters);
        if (tags.empty()) { SpawnResult("This squad isn't in this map"); return; }
        g_squadPending.clear();
        g_squadZones = 0;

        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];
        float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        float dir[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        Engine<HeadPositionFn>(OFFSET_REACH_UNIT_HEAD_POSITION)(p.datums[0], eye);
        float ray[3] = {dir[0] * kSpawnRange, dir[1] * kSpawnRange, dir[2] * kSpawnRange};
        alignas(16) static uint8_t result[REACH_COLLISION_RESULT_SIZE];
        memset(result, 0, sizeof(result));
        uint32_t vehicle = Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT);
        bool hit = Engine<CollisionTestFn>(OFFSET_REACH_COLLISION_TEST_VECTOR)(
            REACH_COLLISION_FLAGS_CAMERA, false, eye, ray, (int32_t)p.datums[0], (int32_t)vehicle, -1, result);
        if (!hit) { SpawnResult("Nothing in range to spawn on"); return; }
        const float* at = (const float*)(result + REACH_COLLISION_RESULT_POINT);
        float center[3] = {at[0] - dir[0] * kSpawnBackOff, at[1] - dir[1] * kSpawnBackOff,
                           at[2] - dir[2] * kSpawnBackOff + kSpawnLift};
        float facing = remainderf(yaw + kPi, 2.0f * kPi);
        float spacing = squad.large ? 1.5f : 0.9f;  // world units between neighbours

        int made = 0;
        for (size_t i = 0; i < tags.size(); ++i) {
            float radius = spacing * sqrtf((float)i);
            float angle = (float)i * 2.39996323f;  // the golden angle
            SquadMember m{tags[i], {center[0] + cosf(angle) * radius, center[1] + sinf(angle) * radius, center[2]},
                          facing};
            if (Create(state, m.tag, team, m.position, m.yaw) != 0xFFFFFFFF) {
                ++made;
                continue;
            }
            g_squadPending.push_back(m);
            g_squadZones |= SpawnZone(m.tag);
        }
        g_squadName = squad.name;
        g_squadTeam = team;
        g_squadMade = made;
        g_squadTotal = (int)tags.size();
        LOG("Reach: squad %s at %.2f %.2f %.2f: %d of %zu made, %zu waiting (zones %08X)", squad.name, center[0],
            center[1], center[2], made, tags.size(), g_squadPending.size(), g_squadZones);
        char text[96];
        if (!g_squadPending.empty() && g_squadZones) {
            LoadZones(g_squadZones);
            g_squadDeadline = GetTickCount64() + kZoneLoadMs;
            sprintf_s(text, "Loading the rest of %s...", squad.name);
        } else {
            g_squadPending.clear();
            sprintf_s(text, made == g_squadTotal ? "Spawned %s" : "Spawned %s (some couldn't be made here)", squad.name);
        }
        SpawnResult(text);
    }

    void ContinueSquadWait(void** state) {
        if (g_squadPending.empty()) return;
        ULONGLONG now = GetTickCount64();
        if ((ActiveZones() & g_squadZones) == g_squadZones) {
            for (size_t i = 0; i < g_squadPending.size();) {
                SquadMember& m = g_squadPending[i];
                if (Create(state, m.tag, g_squadTeam, m.position, m.yaw) != 0xFFFFFFFF) {
                    ++g_squadMade;
                    g_squadPending.erase(g_squadPending.begin() + i);
                } else {
                    ++i;
                }
            }
        }
        if (g_squadPending.empty() || now > g_squadDeadline) {
            char text[96];
            sprintf_s(text, g_squadMade == g_squadTotal ? "Spawned %s" : "Spawned %s (%d of %d)", g_squadName.c_str(),
                      g_squadMade, g_squadTotal);
            SpawnResult(text);
            g_squadPending.clear();
        }
    }

    // Nuke: kills every AI. Units that can't take damage or die (story Spartans,
    // scripted invulnerables) are made mortal first. Players and, if asked,
    // the player's side are left alone.
    void Nuke(void** state, const Players& p, const Array& objects, bool spareAllies) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        int killed = 0;
        int myTeam = PlayerTeam(p);
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            if (!*(uint16_t*)actor) continue;
            uint32_t datum = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
            char* unit = ObjectFromDatum(objects, datum);
            if (!unit || p.Contains(unit)) continue;
            if (spareAllies && Friends(Field<int8_t>(unit, REACH_UNIT_TEAM), myTeam)) continue;
            uint32_t& flags = Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS);
            if (flags & REACH_DAMAGE_FLAG_DEAD) continue;
            flags &= ~(REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE | REACH_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE);
            if (float* shield = Shield(unit)) *shield = 0.0f;
            if (float* health = Health(unit)) *health = 0.0001f;
            Engine<UnitKillFn>(OFFSET_REACH_UNIT_KILL)(datum, 0, true, 2, 0);
            ++killed;
        }
        char text[96];
        sprintf_s(text, killed ? "Nuked %d AI" : "No AI to nuke", killed);
        SpawnResult(text);
    }

    // Drop shield bubble: the carried drop_shield_shield only looks like a
    // bubble; being attached to the unit it has no collision, so shots went
    // straight through. While it's active (equipment +0xCC set), every
    // projectile inside the bubble's bounding sphere that is moving toward the player is
    // deleted. Shots fired from inside (moving away) pass out.
    void DropShieldBlocks(const Players& p, const Array& objects) {
        if (p.count == 0) return;
        char* unit = p.units[0];
        char* equipment = ObjectOfType(objects, Field<uint32_t>(unit, REACH_UNIT_EQUIPMENT), REACH_OBJECT_TYPE_EQUIPMENT);
        if (!equipment || Field<uint32_t>(equipment, REACH_EQUIPMENT_ACTIVE_HANDLE) == 0xFFFFFFFF) return;
        static uint32_t s_tag = 0xFFFFFFFF;
        static bool s_isBubble = false;
        uint32_t tag = Field<uint32_t>(equipment, REACH_OBJECT_TAG_DATUM);
        if (tag != s_tag) {
            s_tag = tag;
            s_isBubble = ShortName(tag) == "drop_shield_shield";
        }
        if (!s_isBubble) return;

        // The object's own bounding sphere (+0x20 centre, +0x3C radius; the
        // placed bubble and the carried one both read 1.8 live), which tracks
        // the player.
        const float* centre = (const float*)(equipment + REACH_OBJECT_SPHERE_CENTRE);
        float radius = Field<float>(equipment, REACH_OBJECT_SPHERE_RADIUS);
        if (!(radius >= 0.5f && radius <= 8.0f)) radius = 2.0f;
        int blocked = 0;
        uint32_t lastShot = 0xFFFFFFFF;
        for (int i = 0; i < objects.HighWater(); ++i) {
            char* entry = objects.Entry(i);
            uint16_t salt = *(uint16_t*)entry;
            if (salt == 0 || Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE) != REACH_OBJECT_TYPE_PROJECTILE) continue;
            char* shot = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            if (!shot) continue;
            const float* pos = (const float*)(shot + REACH_OBJECT_POSITION);
            const float* vel = (const float*)(shot + REACH_OBJECT_VELOCITY);
            // Fast bullets cross the whole bubble in a tick or two, so test
            // the path they're about to take (velocity is per second; look
            // 3 ticks ahead), not just where they are now.
            float d[3] = {pos[0] - centre[0], pos[1] - centre[1], pos[2] - centre[2]};
            float along = d[0] * vel[0] + d[1] * vel[1] + d[2] * vel[2];
            if (along >= 0.0f) continue;  // moving away
            float step[3] = {vel[0] * (3.0f / 60.0f), vel[1] * (3.0f / 60.0f), vel[2] * (3.0f / 60.0f)};
            float stepLen2 = step[0] * step[0] + step[1] * step[1] + step[2] * step[2];
            float t = stepLen2 > 0.0f ? -(d[0] * step[0] + d[1] * step[1] + d[2] * step[2]) / stepLen2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            float c[3] = {d[0] + step[0] * t, d[1] + step[1] * t, d[2] + step[2] * t};
            if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] > radius * radius) continue;
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(((uint32_t)salt << 16) | (uint32_t)i);
            ++blocked;
            lastShot = Field<uint32_t>(shot, REACH_OBJECT_TAG_DATUM);
        }
        static ULONGLONG s_lastLog = 0;
        if (blocked && GetTickCount64() - s_lastLog > 2000) {
            s_lastLog = GetTickCount64();
            LOG("Reach: drop shield blocked %d shots (last: %s)", blocked, ShortName(lastShot).c_str());
        }
    }

    // --- Checkpoints: lock and history -------------------------------------
    // The game keeps its checkpoint as a double-buffered snapshot (see
    // OFFSET_REACH_CHECKPOINT_SLOTS in offsets.h). Every checkpoint that
    // shows up in the current slot is copied out (up to kMaxCheckpoints per
    // mission); going back writes a copy into the current slot and requests
    // a revert.
    constexpr size_t kMaxCheckpoints = 8;
    constexpr size_t kCheckpointHashBytes = 0x20000;  // the header differs between saves
    constexpr int kCheckpointPollTicks = 30;          // twice a second

    struct StoredCheckpoint {
        uint32_t id = 0;
        int64_t when = 0;
        uint64_t hash = 0;
        std::string map;
        std::vector<uint8_t> data;
    };
    std::vector<StoredCheckpoint> g_checkpoints;
    std::mutex g_checkpointLock;  // g_checkpoints is read by the render thread through CheckpointHistory
    uint32_t g_nextCheckpointId = 1;
    uint64_t g_lastCheckpointHash = 0;
    uint64_t g_pendingCheckpointHash = 0;
    int g_checkpointStable = 0;
    int g_checkpointPoll = 0;
    uint32_t g_restoreSerial = 0;
    std::atomic<bool> g_checkpointsLocked{false};
    std::atomic<bool> g_ownSave{false};  // the mod's own Save button is calling the game's save

    // The current slot's snapshot and its size, or nullptr.
    uint8_t* CurrentCheckpoint(size_t* size) {
        __try {
            int32_t current = *(int32_t*)(g_module + OFFSET_REACH_CHECKPOINT_CURRENT);
            if (current < 0 || current > 1) return nullptr;
            char* slots = g_module + OFFSET_REACH_CHECKPOINT_SLOTS;
            char* mine = slots + current * REACH_CHECKPOINT_SLOT_SIZE;
            char* other = slots + (1 - current) * REACH_CHECKPOINT_SLOT_SIZE;
            if (!*(uint8_t*)(mine + REACH_CHECKPOINT_SLOT_VALID)) return nullptr;
            uint8_t* snapshot = *(uint8_t**)(mine + REACH_CHECKPOINT_SLOT_POINTER);
            uint8_t* neighbour = *(uint8_t**)(other + REACH_CHECKPOINT_SLOT_POINTER);
            if (!snapshot || !neighbour) return nullptr;
            // The slots are the same size and sit one after the other.
            size_t span = (size_t)(snapshot > neighbour ? snapshot - neighbour : neighbour - snapshot);
            if (span < 0x100000 || span > 0x4000000 || span < kCheckpointHashBytes) return nullptr;
            if (*(uint32_t*)snapshot == 0) return nullptr;  // never written
            *size = span;
            return snapshot;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // memcpy that survives the game freeing the buffer under it.
    bool SafeCopy(void* to, const void* from, size_t size) {
        __try {
            memcpy(to, from, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // FNV-1a over the header plus a sample every 64 KB, so a change anywhere shows.
    bool CheckpointHash(const uint8_t* snapshot, size_t size, uint64_t* out) {
        __try {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i + 8 <= kCheckpointHashBytes; i += 8) {
                hash ^= *(const uint64_t*)(snapshot + i);
                hash *= 1099511628211ull;
            }
            for (size_t at = kCheckpointHashBytes; at + 64 <= size; at += 0x10000)
                for (size_t i = 0; i < 64; i += 8) {
                    hash ^= *(const uint64_t*)(snapshot + at + i);
                    hash *= 1099511628211ull;
                }
            *out = hash;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string CheckpointMap(const uint8_t* snapshot) {
        char name[64] = {};
        SafeCopy(name, snapshot + REACH_CHECKPOINT_MAP_NAME, sizeof(name) - 1);
        return name;
    }

    // Twice a second: a new snapshot in the current slot that has stayed
    // the same for two looks (a save in progress isn't copied half done) is
    // kept.
    void PollCheckpoints() {
        if (++g_checkpointPoll < kCheckpointPollTicks) return;
        g_checkpointPoll = 0;
        size_t size = 0;
        uint8_t* snapshot = CurrentCheckpoint(&size);
        if (!snapshot) return;
        uint64_t hash = 0;
        if (!CheckpointHash(snapshot, size, &hash)) return;
        if (hash == g_lastCheckpointHash) { g_checkpointStable = 0; return; }
        if (hash != g_pendingCheckpointHash) {
            g_pendingCheckpointHash = hash;
            g_checkpointStable = 1;
            return;
        }
        if (++g_checkpointStable < 2) return;

        StoredCheckpoint saved;
        saved.hash = hash;
        saved.when = (int64_t)time(nullptr);
        saved.map = CheckpointMap(snapshot);
        saved.data.resize(size);
        if (!SafeCopy(saved.data.data(), snapshot, size)) {
            LOG("Reach: checkpoint history: reading the snapshot faulted");
            return;
        }
        g_lastCheckpointHash = hash;
        g_checkpointStable = 0;

        std::lock_guard<std::mutex> lock(g_checkpointLock);
        if (!g_checkpoints.empty() && g_checkpoints.back().map != saved.map) {
            LOG("Reach: checkpoint history cleared (new mission %s)", saved.map.c_str());
            g_checkpoints.clear();
        }
        saved.id = g_nextCheckpointId++;
        LOG("Reach: checkpoint %u kept (%s, %zu in history)", saved.id, saved.map.c_str(), g_checkpoints.size() + 1);
        g_checkpoints.push_back(std::move(saved));
        if (g_checkpoints.size() > kMaxCheckpoints) g_checkpoints.erase(g_checkpoints.begin());
    }

    // Goes back to a kept checkpoint: its snapshot becomes the current
    // slot's, then the game is asked to revert.
    void RestoreCheckpoint(uint32_t id) {
        size_t size = 0;
        uint8_t* snapshot = CurrentCheckpoint(&size);
        std::lock_guard<std::mutex> lock(g_checkpointLock);
        for (const StoredCheckpoint& saved : g_checkpoints) {
            if (saved.id != id) continue;
            if (!snapshot || saved.data.size() != size) {
                LOG("Reach: restore checkpoint %u: no matching slot (size %zu vs %zu)", id, saved.data.size(), size);
                return;
            }
            if (!SafeCopy(snapshot, saved.data.data(), size)) {
                LOG("Reach: restore checkpoint %u: writing the slot faulted", id);
                return;
            }
            g_lastCheckpointHash = saved.hash;  // not a new save
            g_pendingCheckpointHash = saved.hash;
            *(uint16_t*)(g_module + OFFSET_REACH_GAME_REVERT_FLAG) = REACH_GAME_REVERT_REQUEST;
            LOG("Reach: going back to checkpoint %u", id);
            return;
        }
        LOG("Reach: restore checkpoint %u: not in the history", id);
    }

    // Poison touch: any AI on foot within reach of player 1 dies, friend or
    // foe. "Touching" is the two units' bases (feet) being within
    // kPoisonRange horizontally and a body height apart vertically. Invulnerable
    // AI are made mortal first, like the nuke does.
    void PoisonTouch(void** state, const Players& p, const Array& objects) {
        constexpr float kPoisonRange = 1.0f;   // world units (a unit is ~0.6 wide)
        constexpr float kPoisonHeight = 1.5f;
        if (p.count == 0) return;
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        const float* me = (const float*)(p.units[0] + REACH_OBJECT_POSITION);
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            if (!*(uint16_t*)actor) continue;
            uint32_t datum = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
            char* unit = ObjectFromDatum(objects, datum);
            if (!unit || p.Contains(unit)) continue;
            uint32_t& flags = Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS);
            if (flags & REACH_DAMAGE_FLAG_DEAD) continue;
            const float* pos = (const float*)(unit + REACH_OBJECT_POSITION);
            float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
            if (dx * dx + dy * dy > kPoisonRange * kPoisonRange || fabsf(dz) > kPoisonHeight) continue;
            flags &= ~(REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE | REACH_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE);
            if (float* shield = Shield(unit)) *shield = 0.0f;
            if (float* health = Health(unit)) *health = 0.0001f;
            Engine<UnitKillFn>(OFFSET_REACH_UNIT_KILL)(datum, 0, true, 2, 0);
        }
    }

    // Time stop: every actor but the players' is switched off (actor_set_active
    // false, what a dormant squad gets) and its unit, or the vehicle it rides,
    // is held at zero velocity. The barrel-fire hook (cheats.cpp) stops the
    // weapons in their hands firing. Switching it off wakes only the actors
    // this froze; AI that appear later are frozen on the next tick.
    std::unordered_map<uint32_t, uint32_t> g_frozenActors;  // actor datum -> its unit datum
    std::unordered_set<uint32_t> g_frozenObjects;           // those units and the vehicles they ride
    volatile bool g_timeStopOn = false;                     // read by the object-update hook

    void TimeStop(void** state, const Players& p, const Array& objects, bool on) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) {
            g_timeStopOn = false;
            g_frozenActors.clear();
            g_frozenObjects.clear();
            return;
        }
        if (!on) {
            g_timeStopOn = false;
            for (uint32_t datum : g_frozenObjects) {
                char* entry = objects.Entry((int)(datum & 0xFFFF));
                if (entry && *(uint16_t*)entry == (uint16_t)(datum >> 16))
                    Field<uint8_t>(entry, REACH_OBJECT_ENTRY_SKIPPED_FLAGS) &= ~REACH_OBJECT_ENTRY_SKIPPED_BIT;
            }
            g_frozenObjects.clear();
            for (auto& [datum, unit] : g_frozenActors) {
                char* actor = actors.Get(datum);
                if (actor && Field<uint32_t>(actor, REACH_ACTOR_UNIT) == unit)
                    Engine<ActorSetActiveFn>(OFFSET_REACH_ACTOR_SET_ACTIVE)(datum, true);
            }
            if (!g_frozenActors.empty()) LOG("Reach: time resumed (%zu actors woken)", g_frozenActors.size());
            g_frozenActors.clear();
            return;
        }
        uint32_t myParent = p.count ? Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT) : 0xFFFFFFFF;
        g_frozenObjects.clear();
        g_timeStopOn = true;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            uint16_t salt = *(uint16_t*)actor;
            if (!salt) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            uint32_t unitDatum = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
            char* unit = ObjectFromDatum(objects, unitDatum);
            if (!unit || p.Contains(unit)) continue;
            if (Field<uint8_t>(actor, REACH_ACTOR_FLAGS) & 8) {
                Engine<ActorSetActiveFn>(OFFSET_REACH_ACTOR_SET_ACTIVE)(datum, false);
                g_frozenActors[datum] = unitDatum;
            }
            // Frozen whether or not the actor was awake: a dormant or scripted AI
            // is still animated by its unit's update.
            g_frozenObjects.insert(unitDatum);
            // Their weapons too: a melee blow is dealt by the weapon's own update.
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                uint32_t weapon = ((uint32_t*)(unit + REACH_UNIT_WEAPONS))[w];
                if (weapon != 0xFFFFFFFF) g_frozenObjects.insert(weapon);
            }
            uint32_t equipment = Field<uint32_t>(unit, REACH_UNIT_EQUIPMENT);
            if (equipment != 0xFFFFFFFF) g_frozenObjects.insert(equipment);
            memset(&Field<float>(unit, REACH_OBJECT_VELOCITY), 0, 3 * sizeof(float));
            uint32_t parent = Field<uint32_t>(unit, REACH_OBJECT_PARENT);
            if (parent != 0xFFFFFFFF && parent != myParent)
                if (char* vehicle = ObjectFromDatum(objects, parent)) {
                    g_frozenObjects.insert(parent);
                    memset(&Field<float>(vehicle, REACH_OBJECT_VELOCITY), 0, 3 * sizeof(float));
                }
        }
    }

    // Despawn all custom AI: every AI the mod spawned (single spawns, squads,
    // rain, respawns), alive or dead, is deleted, and spawns still waiting
    // for a designer zone are cancelled.
    void DespawnCustomAi(void** state, const Array& objects) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        int removed = 0;
        for (auto& ai : g_spawnedAi) {
            if (ai.unit == 0xFFFFFFFF) continue;
            if (ObjectFromDatum(objects, ai.unit)) {
                if (actors.Valid() && actors.Get(ai.actor)) Engine<ActorEraseFn>(OFFSET_REACH_ACTOR_ERASE)(ai.actor, false);
                else Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(ai.unit);
                ++removed;
            }
            ai = {};
        }
        g_squadPending.clear();
        g_spawnWait = {};
        char text[96];
        sprintf_s(text, removed ? "Despawned %d custom AI" : "No custom AI to despawn", removed);
        SpawnResult(text);
    }

    // Retries a spawn or give each tick once its designer zone is loaded
    // (its resources may take a few more frames), until it works or times out.
    void ContinueZoneWaits(void** state, const Players& p, const Array& objects) {
        ULONGLONG now = GetTickCount64();
        if (g_spawnWait.active && (ActiveZones() & g_spawnWait.zone)) {
            ZoneWait& w = g_spawnWait;
            uint32_t object = Create(state, w.tag, w.team, w.position, w.yaw);
            char text[96];
            if (object != 0xFFFFFFFF) {
                LOG("Reach: spawn %08X: made after its zone loaded (%llu ms left) -> %08X", w.tag, w.deadline - now,
                    object);
                sprintf_s(text, "Spawned %s", SelectionName(w.tag).c_str());
                SpawnResult(text);
                w = {};
            }
        }
        if (g_spawnWait.active && now > g_spawnWait.deadline) {
            LOG("Reach: spawn %08X: still couldn't make it after loading its zone", g_spawnWait.tag);
            char text[96];
            sprintf_s(text, "Couldn't spawn %s here", SelectionName(g_spawnWait.tag).c_str());
            SpawnResult(text);
            g_spawnWait = {};
        }

        if (g_giveWait.active && (ActiveZones() & g_giveWait.zone) && GiveWeapon(p, objects, g_giveWait.tag, true)) {
            LOG("Reach: give weapon %08X: made after its zone loaded", g_giveWait.tag);
            g_giveWait = {};
        }
        if (g_giveWait.active && now > g_giveWait.deadline) {
            LOG("Reach: give weapon %08X: still couldn't make it after loading its zone", g_giveWait.tag);
            g_giveWait = {};
        }
    }

    // Upgrade vehicle: like the New Alexandria easter egg, the Falcon or
    // Banshee player 1 is in becomes a Pelican or Phantom where it is, with
    // them as its pilot. Only if this level has one (its vehicle palette);
    // its designer zone is loaded first if needed.
    // Seating a unit that's in a vehicle only queues its exit (RVA
    // 0x4F9859), so an upgrade is steps a tick or more apart: the zone (if
    // needed), out of the old vehicle, then the new one made (once the old
    // one is gone, so they don't collide) and the player seated.
    enum class UpgradeStep { Zone, Exit, Seat };
    struct UpgradeWait {
        bool active = false;
        UpgradeStep step = UpgradeStep::Zone;
        uint32_t target = 0xFFFFFFFF;  // vehicle tag
        uint32_t zone = 0;
        ULONGLONG deadline = 0;
        uint32_t old = 0xFFFFFFFF;
        uint32_t made = 0xFFFFFFFF;
        float position[3] = {};
        float velocity[3] = {};
        float yaw = 0.0f;
    };
    constexpr ULONGLONG kSeatMs = 1000;    // per step
    constexpr float kUpgradeLift = 3.0f;   // the new one is made this far above the old one
    UpgradeWait g_upgrade;
    uint32_t g_upgradeSerial = 0;

    // The tag of a vehicle in this level's palette with this short name, or -1.
    uint32_t PaletteVehicle(const char* name) {
        char* scnr = ReachTags::Scenario();
        int count = ReachTags::BlockCount(scnr, REACH_SCNR_VEHICLE_PALETTE);
        for (int i = 0; i < count; ++i) {
            char* entry = ReachTags::BlockElement(scnr, REACH_SCNR_VEHICLE_PALETTE, REACH_PALETTE_ENTRY_SIZE, i);
            uint32_t tag = entry ? Field<uint32_t>(entry, REACH_PALETTE_ENTRY_DATUM) : 0xFFFFFFFF;
            if (tag != 0xFFFFFFFF && ShortName(tag) == name) return tag;
        }
        return 0xFFFFFFFF;
    }

    // A vehicle tag's driver seat name (and index), or 0.
    uint32_t DriverSeat(uint32_t vehicle, int* index = nullptr) {
        char* tag = ReachTags::Get(vehicle);
        int seats = ReachTags::BlockCount(tag, REACH_VEHI_SEATS);
        for (int i = 0; i < seats; ++i) {
            char* seat = ReachTags::BlockElement(tag, REACH_VEHI_SEATS, REACH_VEHI_SEAT_SIZE, i);
            if (seat && (Field<uint32_t>(seat, REACH_SEAT_FLAGS) & REACH_SEAT_FLAG_DRIVER)) {
                if (index) *index = i;
                return Field<uint32_t>(seat, REACH_SEAT_NAME);
            }
        }
        return 0;
    }

    // The top vehicle player 1 is in (a Falcon's gun seats are on turrets), or -1.
    uint32_t PlayerVehicle(const Players& p, const Array& objects) {
        if (p.count == 0) return 0xFFFFFFFF;
        uint32_t vehicle = 0xFFFFFFFF;
        uint32_t at = Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT);
        for (int depth = 0; depth < 4 && at != 0xFFFFFFFF; ++depth) {
            int type = -1;
            char* object = ObjectFromDatum(objects, at, &type);
            if (!object || type != REACH_OBJECT_TYPE_VEHICLE) break;
            vehicle = at;
            at = Field<uint32_t>(object, REACH_OBJECT_PARENT);
        }
        return vehicle;
    }

    // Takes player 1 out of their vehicle, remembering where it was.
    void BeginUpgrade(const Players& p, const Array& objects, uint32_t target) {
        uint32_t old = PlayerVehicle(p, objects);
        char* vehicle = old != 0xFFFFFFFF ? ObjectFromDatum(objects, old) : nullptr;
        if (!vehicle) { g_upgrade = {}; return; }  // got out meanwhile
        UpgradeWait& u = g_upgrade;
        u.active = true;
        u.step = UpgradeStep::Exit;
        u.target = target;
        u.old = old;
        u.deadline = GetTickCount64() + kSeatMs;
        memcpy(u.position, &Field<float>(vehicle, REACH_OBJECT_POSITION), sizeof(u.position));
        memcpy(u.velocity, &Field<float>(vehicle, REACH_OBJECT_VELOCITY), sizeof(u.velocity));
        const float* forward = &Field<float>(vehicle, REACH_OBJECT_FORWARD);
        u.yaw = atan2f(forward[1], forward[0]);
        // Queues the exit (and does nothing else while they're seated). It's
        // an eject, which "Cannot be ejected" would block.
        AllowEject(p.datums[0]);
        Engine<UnitEnterVehicleFn>(OFFSET_REACH_UNIT_ENTER_VEHICLE)(p.datums[0], old, DriverSeat(target), true);
        LOG("Reach: upgrade to %s: taking player 1 out of %08X", ShortName(target).c_str(), old);
    }

    void UpgradeVehicle(const Players& p, const Array& objects) {
        g_upgrade = {};
        uint32_t vehicle = PlayerVehicle(p, objects);
        char* object = vehicle != 0xFFFFFFFF ? ObjectFromDatum(objects, vehicle) : nullptr;
        if (!object) { SpawnResult("Get in a Falcon or Banshee first"); return; }
        std::string from = ShortName(Field<uint32_t>(object, REACH_OBJECT_TAG_DATUM));
        const char* to = from == "falcon" ? "pelican" : from == "banshee" ? "phantom" : nullptr;
        if (!to) { SpawnResult("Only a Falcon or Banshee can be upgraded"); return; }
        uint32_t target = PaletteVehicle(to);
        char text[96];
        if (target == 0xFFFFFFFF) {
            sprintf_s(text, "This level has no %s to upgrade to", to);
            SpawnResult(text);
            return;
        }
        uint32_t zone = ZoneToLoad(target);
        if (!zone) {
            BeginUpgrade(p, objects, target);
            return;
        }
        LoadZones(zone);
        g_upgrade.active = true;
        g_upgrade.step = UpgradeStep::Zone;
        g_upgrade.target = target;
        g_upgrade.zone = zone;
        g_upgrade.deadline = GetTickCount64() + kZoneLoadMs;
        sprintf_s(text, "Loading %s...", to);
        SpawnResult(text);
        LOG("Reach: upgrade to %08X: loading designer zone %08X first", target, zone);
    }

    void ContinueUpgrade(const Players& p, const Array& objects) {
        if (!g_upgrade.active) return;
        UpgradeWait& u = g_upgrade;
        ULONGLONG now = GetTickCount64();
        char text[96];
        switch (u.step) {
            case UpgradeStep::Zone:
                if (ActiveZones() & u.zone) BeginUpgrade(p, objects, u.target);
                else if (now > u.deadline) {
                    sprintf_s(text, "Couldn't make a %s here", ShortName(u.target).c_str());
                    SpawnResult(text);
                    u = {};
                }
                return;
            case UpgradeStep::Exit: {
                if (PlayerVehicle(p, objects) != 0xFFFFFFFF) {
                    if (now > u.deadline) {
                        AllowEject(0xFFFFFFFF);
                        LOG("Reach: upgrade: player 1 never left %08X", u.old);
                        SpawnResult("Couldn't get you out of it");
                        u = {};
                    }
                    return;
                }
                AllowEject(0xFFFFFFFF);
                Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(u.old);
                float position[3] = {u.position[0], u.position[1], u.position[2] + kUpgradeLift};
                u.made = NewObject(u.target, 0, position, u.yaw);
                if (u.made == 0xFFFFFFFF || p.count == 0) {
                    sprintf_s(text, "Couldn't make a %s here", ShortName(u.target).c_str());
                    SpawnResult(text);
                    u = {};
                    return;
                }
                uint32_t seat = DriverSeat(u.target);
                Engine<UnitEnterVehicleFn>(OFFSET_REACH_UNIT_ENTER_VEHICLE)(p.datums[0], u.made, seat, true);
                LOG("Reach: upgrade: removed %08X, made %08X, seating player 1 ('%s')", u.old, u.made,
                    ReachTags::StringName(seat).c_str());
                u.step = UpgradeStep::Seat;
                u.deadline = now + kSeatMs;
                return;
            }
            case UpgradeStep::Seat:
                if (PlayerVehicle(p, objects) == u.made) {
                    if (char* made = ObjectFromDatum(objects, u.made))
                        memcpy(&Field<float>(made, REACH_OBJECT_VELOCITY), u.velocity, sizeof(u.velocity));
                    sprintf_s(text, "Upgraded to %s", ShortName(u.target).c_str());
                    SpawnResult(text);
                    LOG("Reach: upgrade: player 1 is piloting %08X", u.made);
                    u = {};
                } else if (now > u.deadline) {
                    LOG("Reach: upgrade: player 1 never got into %08X (left it there)", u.made);
                    SpawnResult("Couldn't get you into it");
                    u = {};
                }
                return;
        }
    }

    // Take pilot seat ("Pilot AI-only vehicles" on): Pelicans and Phantoms
    // have no boarding seats, so the game can't hijack them. The key picks
    // the AI-only aircraft nearest the crosshair within reach, deletes its
    // pilot, and once the seat is free (a tick later) seats player 1.
    constexpr float kTakeSeatReach = 8.0f;   // world units past the aircraft's bounding radius
    constexpr float kTakeSeatCone = 35.0f;   // degrees from the crosshair, on top of its size
    struct TakeSeat {
        bool active = false;
        uint32_t vehicle = 0xFFFFFFFF;
        uint32_t seat = 0;  // name
        int seatIndex = -1;
        bool seating = false;  // the enter is queued
        ULONGLONG deadline = 0;
    };
    TakeSeat g_takeSeat;
    uint32_t g_takeSeatSerial = 0;
    // The last vehicle the take-pilot-seat key put player 1 in. A mission's
    // scripted vehicle (Exodus's Falcons) may not let a player out, so the
    // hull exit's hold-to-leave works there too (see ExitHull).
    uint32_t g_tookVehicle = 0xFFFFFFFF;

    // Someone other than a player sits in the vehicle's pilot seat (an AI, or
    // a mission's invisible scripted pilot, like Exodus's Falcons).
    bool HasNonPlayerPilot(const Players& p, uint32_t vehicle, uint32_t tag) {
        int index = -1;
        DriverSeat(tag, &index);
        if (index < 0) return false;
        uint32_t pilot = Engine<SeatOccupantFn>(OFFSET_REACH_SEAT_OCCUPANT)(vehicle, (uint32_t)index);
        if (pilot == 0xFFFFFFFF) return false;
        for (int i = 0; i < p.count; ++i)
            if (p.datums[i] == pilot) return false;
        return true;
    }

    // The vehicle nearest the crosshair whose pilot seat can be taken: an
    // AI-only one (Pelican, Phantom), or any with a non-player pilot.
    uint32_t AimedAiOnlyAircraft(void** state, const Players& p, const Array& objects) {
        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        float forward[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        Engine<HeadPositionFn>(OFFSET_REACH_UNIT_HEAD_POSITION)(p.datums[0], eye);
        uint32_t best = 0xFFFFFFFF;
        float bestAngle = 1e9f;
        for (int i = 0; i < objects.HighWater(); ++i) {
            char* entry = objects.Entry(i);
            uint16_t salt = *(uint16_t*)entry;
            char* object = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            if (!salt || !object || Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE) != REACH_OBJECT_TYPE_VEHICLE) continue;
            uint32_t tag = Field<uint32_t>(object, REACH_OBJECT_TAG_DATUM);
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (!IsAiOnlyVehicle(tag) && !HasNonPlayerPilot(p, datum, tag)) continue;
            float radius = Field<float>(ReachTags::Get(tag), REACH_OBJE_BOUNDING_RADIUS);
            const float* at = &Field<float>(object, REACH_OBJECT_POSITION);
            float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > radius + kTakeSeatReach) continue;
            float angle = len < 0.1f ? 0.0f
                                     : acosf(fmaxf(-1.0f, fminf(1.0f, (d[0] * forward[0] + d[1] * forward[1] +
                                                                      d[2] * forward[2]) / len))) * 180.0f / kPi;
            float size = len > radius ? atanf(radius / len) * 180.0f / kPi : 90.0f;  // how big it looks
            if (angle > size + kTakeSeatCone || angle >= bestAngle) continue;
            bestAngle = angle;
            best = datum;
        }
        return best;
    }

    void TakePilotSeat(void** state, const Players& p, const Array& objects) {
        g_takeSeat = {};
        if (p.count == 0) return;
        if (PlayerVehicle(p, objects) != 0xFFFFFFFF) { SpawnResult("Get out of your vehicle first"); return; }
        uint32_t vehicle = AimedAiOnlyAircraft(state, p, objects);
        char* object = vehicle != 0xFFFFFFFF ? ObjectFromDatum(objects, vehicle) : nullptr;
        if (!object) { SpawnResult("Aim at a piloted or AI-only vehicle up close"); return; }
        uint32_t tag = Field<uint32_t>(object, REACH_OBJECT_TAG_DATUM);
        TakeSeat& t = g_takeSeat;
        t.seat = DriverSeat(tag, &t.seatIndex);
        if (t.seatIndex < 0) return;
        t.active = true;
        t.vehicle = vehicle;
        t.deadline = GetTickCount64() + kSeatMs;
        uint32_t pilot = Engine<SeatOccupantFn>(OFFSET_REACH_SEAT_OCCUPANT)(vehicle, (uint32_t)t.seatIndex);
        if (pilot != 0xFFFFFFFF) Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(pilot);
        LOG("Reach: take pilot seat of %08X (%s): pilot %08X removed", vehicle, ShortName(tag).c_str(), pilot);
    }

    void ContinueTakeSeat(const Players& p, const Array& objects) {
        TakeSeat& t = g_takeSeat;
        if (!t.active) return;
        ULONGLONG now = GetTickCount64();
        if (p.count == 0 || !ObjectFromDatum(objects, t.vehicle)) { t = {}; return; }
        if (!t.seating) {
            if (Engine<SeatOccupantFn>(OFFSET_REACH_SEAT_OCCUPANT)(t.vehicle, (uint32_t)t.seatIndex) == 0xFFFFFFFF) {
                Engine<UnitEnterVehicleFn>(OFFSET_REACH_UNIT_ENTER_VEHICLE)(p.datums[0], t.vehicle, t.seat, true);
                t.seating = true;
                t.deadline = now + kSeatMs;
            } else if (now > t.deadline) {
                SpawnResult("Couldn't clear the pilot seat");
                t = {};
            }
            return;
        }
        if (PlayerVehicle(p, objects) == t.vehicle) {
            SpawnResult("You're the pilot now");
            g_tookVehicle = t.vehicle;
            LOG("Reach: take pilot seat: player 1 is piloting %08X", t.vehicle);
            t = {};
        } else if (now > t.deadline) {
            LOG("Reach: take pilot seat: player 1 never got into %08X", t.vehicle);
            SpawnResult("Couldn't get you into it");
            t = {};
        }
    }

    // Getting out of a Phantom ("Pilot AI-only vehicles" on): its pilot
    // seat is inside the hull and no exit works for a player (exit
    // requests do nothing; the flip eviction's eject crashed MCC; deleting
    // the Phantom deletes them too). Holding the exit button there detaches
    // player 1 (objects_detach) and, once they're free, deletes the Phantom.
    constexpr float kExitHoldSeconds = 0.4f;
    // Any other seat: long enough that a seat whose own exit works has let
    // them out first.
    constexpr float kStuckExitHoldSeconds = 1.0f;
    struct HullExit {
        float held = 0.0f;
        uint32_t vehicle = 0xFFFFFFFF;  // detaching from it
        bool remove = false;            // delete it once out (a Phantom: they'd be inside its hull)
        ULONGLONG deadline = 0;
        // Stuck-seat fallback: only a press made after sitting down counts
        // (boarding is also a held X / E), so a normal vehicle's own exit
        // always gets first go.
        uint32_t seatedIn = 0xFFFFFFFF;  // player 1's parent when last seen
        bool released = false;           // exit let go since sitting there
    };
    HullExit g_hullExit;

    // The AI-only vehicle player 1 pilots from an enclosed seat, or -1.
    uint32_t EnclosedPilotSeat(const Players& p, const Array& objects, uint32_t* seatName) {
        uint32_t vehicle = PlayerVehicle(p, objects);
        char* object = vehicle != 0xFFFFFFFF ? ObjectFromDatum(objects, vehicle) : nullptr;
        uint32_t tag = object ? Field<uint32_t>(object, REACH_OBJECT_TAG_DATUM) : 0xFFFFFFFF;
        if (!object || !IsAiOnlyVehicle(tag)) return 0xFFFFFFFF;
        int index = -1;
        *seatName = DriverSeat(tag, &index);
        char* seat = ReachTags::BlockElement(ReachTags::Get(tag), REACH_VEHI_SEATS, REACH_VEHI_SEAT_SIZE, index);
        if (!seat || !(Field<uint32_t>(seat, REACH_SEAT_FLAGS) & REACH_SEAT_FLAG_INVISIBLE)) return 0xFFFFFFFF;
        uint32_t pilot = Engine<SeatOccupantFn>(OFFSET_REACH_SEAT_OCCUPANT)(vehicle, (uint32_t)index);
        return pilot == p.datums[0] ? vehicle : 0xFFFFFFFF;
    }

    void ExitHull(const Players& p, const Array& objects, bool on, bool exitHeld, float dt) {
        HullExit& h = g_hullExit;
        if (h.vehicle != 0xFFFFFFFF) {
            if (p.count > 0 && Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT) == 0xFFFFFFFF) {
                char* vehicle = ObjectFromDatum(objects, h.vehicle);
                if (h.remove || !vehicle) {
                    Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(h.vehicle);
                    LOG("Reach: player 1 detached from %08X; removed it", h.vehicle);
                } else {
                    // Kept: put them beside it (to its left), clear of the hull.
                    const float* at = &Field<float>(vehicle, REACH_OBJECT_POSITION);
                    const float* forward = &Field<float>(vehicle, REACH_OBJECT_FORWARD);
                    float radius = Field<float>(ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM)),
                                                REACH_OBJE_BOUNDING_RADIUS);
                    if (!(radius > 0.0f && radius < 20.0f)) radius = 3.0f;
                    float side = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]);
                    float left[2] = {side > 0.01f ? -forward[1] / side : 1.0f, side > 0.01f ? forward[0] / side : 0.0f};
                    float position[3] = {at[0] + left[0] * (radius + 0.5f), at[1] + left[1] * (radius + 0.5f),
                                         at[2] + 0.5f};
                    Engine<SetPositionFn>(OFFSET_REACH_OBJECT_SET_POSITION)(p.datums[0], position, nullptr, nullptr, 0,
                                                                          false);
                    LOG("Reach: player 1 detached from %08X; placed beside it", h.vehicle);
                }
                g_tookVehicle = 0xFFFFFFFF;
            } else if (GetTickCount64() < h.deadline) {
                return;
            } else {
                LOG("Reach: player 1 still attached to %08X; left it", h.vehicle);
                SpawnResult("Couldn't get you out of it");
            }
            h = {};
            AllowEject(0xFFFFFFFF);
            return;
        }
        uint32_t seat = 0;
        uint32_t vehicle = on && p.count > 0 ? EnclosedPilotSeat(p, objects, &seat) : 0xFFFFFFFF;
        bool remove = vehicle != 0xFFFFFFFF;
        uint32_t parent = p.count > 0 ? Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT) : 0xFFFFFFFF;
        if (parent != h.seatedIn) {
            h.seatedIn = parent;
            h.released = false;
        }
        if (!exitHeld) h.released = true;
        if (!h.released) { h.held = 0.0f; return; }
        float hold = kExitHoldSeconds;
        const char* why = "enclosed pilot seat";
        if (!remove && on && p.count > 0 && Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT) != 0xFFFFFFFF) {
            // Any other seat (pilot, gunner, passenger) whose own exit does
            // nothing: a taken vehicle's quickly, the rest after a second.
            // Placed beside the whole vehicle (a Falcon's side gun is a
            // turret attached to it).
            vehicle = PlayerVehicle(p, objects);
            bool taken = vehicle != 0xFFFFFFFF && vehicle == g_tookVehicle;
            // Quick only for AI-only aircraft, which have no exit of their own. A Banshee
            // you took has a normal exit already running; detaching during it crashed MCC.
            char* taker = taken ? ObjectFromDatum(objects, vehicle) : nullptr;
            bool aiOnly = taker && IsAiOnlyVehicle(Field<uint32_t>(taker, REACH_OBJECT_TAG_DATUM));
            hold = aiOnly ? kExitHoldSeconds : kStuckExitHoldSeconds;
            why = taken ? "taken pilot seat" : "stuck in a seat";
        }
        if (vehicle == 0xFFFFFFFF || !exitHeld) { h.held = 0.0f; return; }
        h.held += dt;
        if (h.held < hold) return;
        LOG("Reach: detaching player 1 from %08X (%s)", vehicle, why);
        // "Cannot be ejected" drops the detach's own eject action and crashed MCC here:
        // let it through for player 1 until the exit finishes.
        AllowEject(p.datums[0]);
        Engine<ObjectDetachFn>(OFFSET_REACH_OBJECT_DETACH)(p.datums[0]);
        h = {};
        h.seatedIn = parent;
        h.vehicle = vehicle;
        h.remove = remove;
        h.deadline = GetTickCount64() + kSeatMs;
    }

    // Spawned AI after death. Some characters (Spartans) are revived by the
    // game: a new actor on the same unit a few seconds later. A revived one
    // can be broken (stuck in an odd pose), so the game's revive is never
    // kept. When one is revived, or has been dead kRespawnMs, "Enable
    // respawn" decides, once: on, the unit is replaced by a fresh actor of
    // the same character and team where it lay; off, the body is removed.
    void FollowSpawnedAi(void** state, const Players& p, const Array& objects, bool respawn) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        for (auto& ai : g_spawnedAi) {
            if (ai.unit == 0xFFFFFFFF) continue;
            char* unit = ObjectFromDatum(objects, ai.unit);
            if (!unit) { ai = {}; continue; }  // gone for good
            if (actors.Get(ai.actor)) continue;  // still has its AI
            // Its actor is gone: look for a new one on the same unit.
            bool revived = false;
            for (int i = 0; i < actors.HighWater() && !revived; ++i) {
                char* entry = actors.Entry(i);
                revived = *(uint16_t*)entry && Field<uint32_t>(entry, REACH_ACTOR_UNIT) == ai.unit;
            }
            ULONGLONG now = GetTickCount64();
            if (!ai.goneAt) {
                ai.goneAt = now;
                float* health = Health(unit);
                LOG("Reach: spawned AI %08X died (unit %08X, health %.2f)", ai.actor, ai.unit,
                    health ? *health : -1.0f);
            }
            if (!revived && now - ai.goneAt < kRespawnMs) continue;
            float deadFor = (now - ai.goneAt) / 1000.0f;
            const char* why = revived ? "the game revived it" : "dead long enough";

            SpawnedAi old = ai;
            ai = {};
            uint32_t actor = 0xFFFFFFFF;
            if (respawn && ReachTags::Get(old.character)) {
                float position[3];
                memcpy(position, &Field<float>(unit, REACH_OBJECT_POSITION), sizeof(position));
                position[2] += kSpawnLift;
                float yaw = 0.0f;  // facing player 1
                if (p.count > 0) {
                    const float* at = &Field<float>(p.units[0], REACH_OBJECT_POSITION);
                    yaw = atan2f(at[1] - position[1], at[0] - position[0]);
                }
                actor = SpawnActor(old.character, position, yaw, old.team);
                Follow(ai, actors, objects, actor, old.character, old.team);
            }
            Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(old.unit);
            if (ai.unit != 0xFFFFFFFF)
                LOG("Reach: spawned AI respawned after %.1f s (%s): old unit %08X -> actor %08X, unit %08X", deadFor,
                    why, old.unit, ai.actor, ai.unit);
            else
                LOG("Reach: spawned AI unit %08X removed after %.1f s (%s, %s)", old.unit, deadFor, why,
                    respawn ? "couldn't respawn it" : "respawn off");
        }
    }

    // The engine's ray test from `from` along `vec` (its length is the
    // range), ignoring up to two objects. Writes the hit point.
    bool CastRay(const float* from, const float* vec, uint32_t ignore1, uint32_t ignore2, float* hit) {
        alignas(16) static uint8_t result[REACH_COLLISION_RESULT_SIZE];
        memset(result, 0, sizeof(result));
        bool ok = Engine<CollisionTestFn>(OFFSET_REACH_COLLISION_TEST_VECTOR)(
            REACH_COLLISION_FLAGS_CAMERA, false, from, vec, (int32_t)ignore1, (int32_t)ignore2, -1, result);
        if (ok) memcpy(hit, result + REACH_COLLISION_RESULT_POINT, 3 * sizeof(float));
        return ok;
    }

    // The top vehicle a unit is seated in (up through turrets), or -1.
    uint32_t TopVehicle(const Array& objects, uint32_t unit) {
        uint32_t top = 0xFFFFFFFF;
        uint32_t at = unit;
        for (int depth = 0; depth < 5; ++depth) {
            char* object = ObjectFromDatum(objects, at);
            uint32_t parent = object ? Field<uint32_t>(object, REACH_OBJECT_PARENT) : 0xFFFFFFFF;
            if (!ObjectOfType(objects, parent, REACH_OBJECT_TYPE_VEHICLE)) break;
            top = at = parent;
        }
        return top;
    }

    uint32_t g_random = 0x2545F491u;
    float Random01() {  // xorshift, [0, 1)
        g_random ^= g_random << 13;
        g_random ^= g_random >> 17;
        g_random ^= g_random << 5;
        return (g_random >> 8) / 16777216.0f;
    }

    // Skulls page: skulls the user ticked are kept on. Ones the mod turned
    // on are turned off again when unticked; ones MCC set are left alone.
    uint64_t g_skullsForced = 0;
    char* g_skullGlobals = nullptr;

    void Skulls(void** state, uint64_t wanted) {
        char* globals = (char*)state[REACH_TLS_SLOT_GAME_GLOBALS];
        if (!globals) return;
        if (globals != g_skullGlobals) {
            g_skullGlobals = globals;
            g_skullsForced = 0;
        }
        wanted &= (1ull << REACH_SKULL_COUNT) - 1;
        auto& mask = Field<uint64_t>(globals, REACH_GAME_GLOBALS_SKULLS);
        uint64_t before = mask;
        uint64_t add = wanted & ~mask;
        uint64_t drop = g_skullsForced & ~wanted;
        g_skullsForced = (g_skullsForced | add) & ~drop;
        mask = (mask | add) & ~drop;
        if (mask == before) return;
        LOG("Reach: skulls %016llX -> %016llX", (unsigned long long)before, (unsigned long long)mask);
        Engine<void(__fastcall*)(uint64_t*)>(OFFSET_REACH_SKULLS_CHANGED)(&mask);  // as skull_enable does
    }

    // Teleport: player 1 (or the vehicle they're in) to where they aim.
    uint32_t g_teleportSerial = 0;

    void Teleport(void** state, const Players& p, const Array& objects) {
        if (p.count == 0) return;
        char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
        float dir[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        Engine<HeadPositionFn>(OFFSET_REACH_UNIT_HEAD_POSITION)(p.datums[0], eye);
        float ray[3] = {dir[0] * kSpawnRange, dir[1] * kSpawnRange, dir[2] * kSpawnRange};
        uint32_t top = TopVehicle(objects, p.datums[0]);
        float at[3];
        if (!CastRay(eye, ray, p.datums[0], top, at)) { SpawnResult("Nothing in range to teleport to"); return; }

        // Backed off from the surface by the thing's size, and a little up.
        uint32_t target = p.datums[0];
        float backOff = 0.6f, lift = 0.3f;
        if (top != 0xFFFFFFFF) {
            target = top;
            char* vehicle = ObjectFromDatum(objects, top);
            float radius = Field<float>(ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM)),
                                        REACH_OBJE_BOUNDING_RADIUS);
            backOff = radius > 0.0f && radius < 100.0f ? radius : 2.0f;
            lift = kVehicleLift;
        }
        float position[3];
        for (int k = 0; k < 3; ++k) position[k] = at[k] - dir[k] * backOff;
        position[2] = (position[2] > at[2] ? position[2] : at[2]) + lift;
        Engine<SetPositionFn>(OFFSET_REACH_OBJECT_SET_POSITION)(target, position, nullptr, nullptr, 0, false);
        const float still[3] = {};
        Engine<SetVelocityFn>(OFFSET_REACH_OBJECT_SET_VELOCITY)(target, still, still);
        if (g_noclipActive && target == p.datums[0]) memcpy(g_anchor, position, sizeof(g_anchor));
        LOG("Reach: teleported %08X to %.2f %.2f %.2f", target, position[0], position[1], position[2]);
    }

    // Giant / tiny AI: every AI unit is kept at the size set for its side
    // (enemy or friendly, by team against player 1's). object_set_scale is
    // called when a unit's scale differs from the wanted one; only units this
    // changed are put back when the slider returns to 1, so scripted sizes
    // elsewhere are left alone.
    std::unordered_map<uint32_t, float> g_scaledAi;  // unit datum -> the scale this set

    void ScaleAi(void** state, const Players& p, const Array& objects, float enemy, float friendly) {
        auto setScale = [&](uint32_t datum, float scale) {
            Engine<SetScaleFn>(OFFSET_REACH_OBJECT_SET_SCALE)(datum, scale, 0);
        };
        bool wanted = enemy != 1.0f || friendly != 1.0f;
        if (!wanted && g_scaledAi.empty()) return;
        int myTeam = PlayerTeam(p);
        for (auto it = g_scaledAi.begin(); it != g_scaledAi.end();) {
            char* unit = ObjectFromDatum(objects, it->first);
            if (!unit) {
                it = g_scaledAi.erase(it);
                continue;
            }
            if (!wanted) {
                setScale(it->first, 1.0f);
                it = g_scaledAi.erase(it);
                continue;
            }
            ++it;
        }
        if (!wanted) return;
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            if (!*(uint16_t*)actor) continue;
            uint32_t datum = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
            char* unit = ObjectFromDatum(objects, datum);
            if (!unit || p.Contains(unit)) continue;
            float scale = Friends(Field<int8_t>(unit, REACH_UNIT_TEAM), myTeam) ? friendly : enemy;
            auto known = g_scaledAi.find(datum);
            float actual = Field<float>(unit, REACH_OBJECT_SCALE_TARGET);
            if (known == g_scaledAi.end()) {
                if (fabsf(scale - 1.0f) < 0.001f) continue;  // untouched and wanted at normal size
            } else if (fabsf(actual - scale) < 0.001f) {
                known->second = scale;
                continue;
            }
            setScale(datum, scale);
            g_scaledAi[datum] = scale;
        }
    }

    // Double / infinite jump: a jump press while in the air (no ground within
    // kAirGap below the feet) sets the unit's upward speed to the biped tag's
    // jump velocity, which the Jump height setting has already scaled. The
    // press is the game's default jump button (A / Space, read by the menu);
    // the ground test is the engine's ray. A trip off the ground allows
    // `airJumps` of them (or any number).
    constexpr float kAirGap = 0.25f;
    int g_airJumpsUsed = 0;
    uint32_t g_jumpPressesSeen = 0;
    bool g_airJumpLogged = false;

    void AirJump(const Players& p, const Array& objects, const Games::Settings& s, const Games::FrameInput& in) {
        bool pressed = in.jumpPresses != g_jumpPressesSeen;
        g_jumpPressesSeen = in.jumpPresses;
        if (!s.doubleJump || p.count == 0) { g_airJumpsUsed = 0; return; }
        char* unit = p.units[0];
        if (Field<uint32_t>(unit, REACH_OBJECT_PARENT) != 0xFFFFFFFF || g_noclipActive) { g_airJumpsUsed = 0; return; }
        const float* pos = &Field<float>(unit, REACH_OBJECT_POSITION);
        const float from[3] = {pos[0], pos[1], pos[2] + 0.1f};
        const float down[3] = {0.0f, 0.0f, -kAirGap};
        float at[3];
        bool grounded = CastRay(from, down, p.datums[0], 0xFFFFFFFF, at);
        if (pressed) {  // debug: why a press did or didn't become an air jump
            static int s_logged = 0;
            if (s_logged++ < 12)
                LOG("Reach: jump press: grounded %d, used %d, air jumps %.0f, infinite %d, feet z %.2f ground z %.2f",
                    grounded ? 1 : 0, g_airJumpsUsed, s.airJumps, s.infiniteJump ? 1 : 0, pos[2], grounded ? at[2] : 0.0f);
        }
        if (grounded) { g_airJumpsUsed = 0; return; }
        if (!pressed) return;
        int limit = s.infiniteJump ? 0x7FFFFFFF : (int)(s.airJumps + 0.5f);
        if (g_airJumpsUsed >= limit) return;
        char* tag = ReachTags::Get(Field<uint32_t>(unit, REACH_OBJECT_TAG_DATUM));
        float jump = tag ? Field<float>(tag, REACH_BIPD_JUMP_VELOCITY) : 0.0f;
        if (!(jump > 0.0f)) return;
        ++g_airJumpsUsed;
        const float* now = &Field<float>(unit, REACH_OBJECT_VELOCITY);
        float velocity[3] = {now[0], now[1], jump};
        Engine<SetVelocityFn>(OFFSET_REACH_OBJECT_SET_VELOCITY)(p.datums[0], velocity, nullptr);
        if (!g_airJumpLogged) {
            g_airJumpLogged = true;
            LOG("Reach: air jump at %.2f %.2f %.2f, upward speed %.2f (jump %d of %s)", pos[0], pos[1], pos[2], jump,
                g_airJumpsUsed, s.infiniteJump ? "unlimited" : "limit");
        }
    }

    // Rain: every few seconds something from the spawn lists is made a
    // little under the sky (or the ceiling) somewhere around player 1. The
    // oldest drop is removed once there are too many, unless a player is
    // using it (holding the weapon, in the vehicle).
    constexpr int kMaxRain = 64;
    constexpr float kMetresPerUnit = 3.048f;  // a world unit is 10 feet
    constexpr float kRainMinFraction = 0.3f;  // of the radius: nothing lands right on the player
    constexpr float kRainHeight = 15.0f;
    uint32_t g_rain[kMaxRain];  // object datums, oldest first
    int g_rainCount = 0;
    float g_rainTimer = 0.0f;

    bool InUseByPlayer(const Players& p, uint32_t object) {
        for (int i = 0; i < p.count; ++i) {
            if (Field<uint32_t>(p.units[i], REACH_OBJECT_PARENT) == object) return true;
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w)
                if (Field<uint32_t>(p.units[i], REACH_UNIT_WEAPONS + w * 4) == object) return true;
        }
        return false;
    }

    void Rain(void** state, const Players& p, const Array& objects, const Games::Settings& s, float dt) {
        int max = (int)(s.rainMax + 0.5f);
        max = max < 1 ? 1 : (max > kMaxRain ? kMaxRain : max);
        while (g_rainCount > 0 && (g_rainCount > max || !ObjectFromDatum(objects, g_rain[0]))) {
            uint32_t oldest = g_rain[0];
            if (ObjectFromDatum(objects, oldest) && !InUseByPlayer(p, oldest))
                Engine<ObjectDeleteFn>(OFFSET_REACH_OBJECT_DELETE)(oldest);
            memmove(g_rain, g_rain + 1, (--g_rainCount) * sizeof(uint32_t));
        }
        if (!s.rain || p.count == 0) { g_rainTimer = 0.0f; return; }
        g_rainTimer += dt;
        if (g_rainTimer < s.rainInterval) return;
        g_rainTimer = 0.0f;

        uint32_t pick = RainPick(s.rainKind, s.spawnTag);
        uint32_t tag = pick != 0xFFFFFFFF ? ChoiceFor(pick).vehicle : 0xFFFFFFFF;
        if (tag == 0xFFFFFFFF || !ReachTags::Get(tag)) return;

        // A random spot around the player, short of any wall in between.
        uint32_t top = TopVehicle(objects, p.datums[0]);
        char* base = top != 0xFFFFFFFF ? ObjectFromDatum(objects, top) : p.units[0];
        const float* at = &Field<float>(base, REACH_OBJECT_POSITION);
        float angle = Random01() * 2.0f * kPi;
        float radius = s.rainRadius / kMetresPerUnit;
        float distance = radius * (kRainMinFraction + Random01() * (1.0f - kRainMinFraction));
        float from[3] = {at[0], at[1], at[2] + 1.5f};
        float out[3] = {cosf(angle) * distance, sinf(angle) * distance, 0.0f};
        float spot[3];
        if (CastRay(from, out, p.datums[0], top, spot)) {
            for (int k = 0; k < 2; ++k) spot[k] -= out[k] / distance * 1.5f;
        } else {
            for (int k = 0; k < 3; ++k) spot[k] = from[k] + out[k];
        }
        // Then up to the sky, or just under a ceiling.
        float up[3] = {0.0f, 0.0f, kRainHeight};
        float ceiling[3];
        float drop[3] = {spot[0], spot[1], spot[2] + kRainHeight};
        if (CastRay(spot, up, 0xFFFFFFFF, 0xFFFFFFFF, ceiling))
            drop[2] = ceiling[2] - 1.5f > spot[2] ? ceiling[2] - 1.5f : spot[2];

        uint32_t made = Create(state, pick, s.spawnTeam, drop, Random01() * 2.0f * kPi);
        if (made == 0xFFFFFFFF) {
            // Load its zone for next time; this drop is skipped.
            if (uint32_t zone = SpawnZone(tag)) LoadZones(zone);
            return;
        }
        if (ReachTags::GroupOf(tag) != REACH_TAG_CHAR) {
            // New objects can start with their physics asleep and hang in the
            // air; a small push through the engine wakes them so they fall.
            const float down[3] = {0.0f, 0.0f, -1.0f};
            Engine<SetVelocityFn>(OFFSET_REACH_OBJECT_SET_VELOCITY)(made, down, nullptr);
        } else {  // Create gave an actor: keep its unit
            Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
            char* actor = actors.Valid() ? actors.Get(made) : nullptr;
            made = actor ? Field<uint32_t>(actor, REACH_ACTOR_UNIT) : 0xFFFFFFFF;
        }
        if (made != 0xFFFFFFFF && g_rainCount < kMaxRain) g_rain[g_rainCount++] = made;
        static ULONGLONG s_lastLog = 0;
        if (GetTickCount64() - s_lastLog > 5000) {
            s_lastLog = GetTickCount64();
            LOG("Reach: rain: %s at %.1f %.1f %.1f (%d dropped)", SelectionName(pick).c_str(), drop[0], drop[1],
                drop[2], g_rainCount);
        }
    }

    // Recoil push: the shoves queued by last tick's shots, through the
    // engine's set-velocity so the physics (a standing biped's too) takes them.
    void ApplyPushes(const Array& objects) {
        PendingPush pushes[8];
        int count = TakePushes(pushes, _countof(pushes));
        for (int i = 0; i < count; ++i) {
            char* object = ObjectFromDatum(objects, pushes[i].object);
            if (!object) continue;
            const float* now = &Field<float>(object, REACH_OBJECT_VELOCITY);
            float velocity[3] = {now[0] + pushes[i].velocity[0], now[1] + pushes[i].velocity[1],
                                 now[2] + pushes[i].velocity[2]};
            Engine<SetVelocityFn>(OFFSET_REACH_OBJECT_SET_VELOCITY)(pushes[i].object, velocity, nullptr);
        }
    }

    // --- Upgrade AI and random AI teams -----------------------------------
    // Both work on the actors (living AI) the mission has placed. The menu
    // buttons go over every AI in range; the auto toggles handle each AI
    // once, as it shows up (or comes in range).

    // An object's world position: a seated unit's own is relative to its
    // seat, so it's the position of what it sits in.
    const float* WorldPosition(const Array& objects, char* object) {
        for (int depth = 0; depth < 5; ++depth) {
            char* parent = ObjectFromDatum(objects, Field<uint32_t>(object, REACH_OBJECT_PARENT));
            if (!parent) break;
            object = parent;
        }
        return &Field<float>(object, REACH_OBJECT_POSITION);
    }

    bool InRange(const Players& p, const Array& objects, char* unit, float metres) {
        if (metres <= 0.0f) return true;
        if (p.count == 0) return false;
        const float* a = WorldPosition(objects, p.units[0]);
        const float* b = WorldPosition(objects, unit);
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        float range = metres / kMetresPerUnit;
        return dx * dx + dy * dy + dz * dz <= range * range;
    }

    // Rank ladders by character name (Reach's campaign characters, read
    // from every mission's tag names 2026-10-06). A character upgrades to
    // the lowest higher rank this map has, so a missing rank is skipped.
    // Rows that aren't targets share a rank with a plain one (an elite
    // pilot is a minor, a ranger an officer). Characters not listed
    // (Spartans, hunters, engineers, civilians, invisible pilots) are never
    // upgraded.
    struct Rank {
        const char* name;
        int family;
        int rank;
        bool target;  // something can be upgraded into this
    };
    constexpr int kAllyFamily = 6;
    const Rank kRanks[] = {
        {"grunt", 0, 0, true},
        {"grunt_major", 0, 1, true},
        {"grunt_heavy", 0, 2, true},
        {"grunt_ultra", 0, 3, true},
        {"grunt_specops", 0, 4, true},
        {"jackal", 1, 0, true},
        {"jackal_major", 1, 1, true},
        {"jackal_sniper", 1, 2, true},
        {"skirmisher", 2, 0, true},
        {"skirmisher_major", 2, 1, true},
        {"skirmisher_murmillone", 2, 2, true},
        {"skirmisher_commando", 2, 3, true},
        {"skirmisher_champion", 2, 4, true},
        {"elite", 3, 0, true},
        {"elite_pilot", 3, 0, false},
        {"elite_officer", 3, 1, true},
        {"elite_jetpack", 3, 1, false},
        {"elite_specops", 3, 2, true},
        {"elite_ultra", 3, 3, true},
        {"elite_general", 3, 4, true},
        {"brute", 4, 0, true},
        {"brute_captain", 4, 1, true},
        {"brute_chieftain_weapon", 4, 2, true},
        {"brute_chieftain_armor", 4, 3, true},
        {"bugger", 5, 0, true},
        {"bugger_major", 5, 1, true},
        {"bugger_ultra", 5, 2, true},
        {"bugger_captain", 5, 3, true},
        {"bugger_captain_major", 5, 4, true},
        {"trooper", kAllyFamily, 0, true},
        {"trooper_female", kAllyFamily, 0, false},
        {"trooper_sgt", kAllyFamily, 1, true},
        {"trooper_odst", kAllyFamily, 2, true},
    };

    struct UpgradeTo {
        uint32_t character;
        bool ally;
    };
    std::vector<uint32_t> g_ladderCharacters;  // the character pool g_upgradeTo was built from
    std::unordered_map<uint32_t, UpgradeTo> g_upgradeTo;

    // Rebuilt whenever the map's character list changes.
    void BuildUpgrades() {
        std::vector<uint32_t> characters = SpawnableCharacters();
        if (characters == g_ladderCharacters) return;
        g_ladderCharacters = characters;
        g_upgradeTo.clear();
        std::unordered_map<std::string, uint32_t> byName;
        for (uint32_t c : characters) byName[ShortName(c)] = c;
        for (const Rank& from : kRanks) {
            auto have = byName.find(from.name);
            if (have == byName.end()) continue;
            const Rank* best = nullptr;
            for (const Rank& to : kRanks) {
                if (to.family != from.family || to.rank <= from.rank || !to.target || !byName.count(to.name)) continue;
                if (!best || to.rank < best->rank) best = &to;
            }
            if (!best) continue;
            g_upgradeTo[have->second] = {byName[best->name], from.family == kAllyFamily};
            LOG("Reach: upgrade AI: %s -> %s", from.name, best->name);
        }
    }

    // Replaces one AI with a fresh actor of `character` where it stands,
    // facing the same way, in its squad (so it keeps the mission's orders)
    // and on its team. The new one is made before the old one is erased, so
    // the squad is never empty. Returns the new actor, or -1.
    uint32_t UpgradeActor(const Array& actors, const Array& objects, uint32_t actorDatum, uint32_t character) {
        char* actor = actors.Get(actorDatum);
        uint32_t unitDatum = actor ? Field<uint32_t>(actor, REACH_ACTOR_UNIT) : 0xFFFFFFFF;
        char* unit = ObjectFromDatum(objects, unitDatum);
        if (!unit) return 0xFFFFFFFF;
        float position[3];
        memcpy(position, &Field<float>(unit, REACH_OBJECT_POSITION), sizeof(position));
        position[2] += kSpawnLift;
        const float* forward = &Field<float>(unit, REACH_OBJECT_FORWARD);
        float yaw = atan2f(forward[1], forward[0]);
        int team = Field<int8_t>(actor, REACH_ACTOR_TEAM);
        int32_t squad = Field<int32_t>(actor, REACH_ACTOR_SQUAD);

        uint32_t fresh = SpawnActor(character, position, yaw, 0);
        if (fresh == 0xFFFFFFFF) return fresh;
        if (squad != -1) Engine<ActorMigrateFn>(OFFSET_REACH_ACTOR_MIGRATE)(fresh, (int16_t)squad);
        Engine<ActorSetTeamFn>(OFFSET_REACH_ACTOR_SET_TEAM)(fresh, team);
        Engine<ActorEraseFn>(OFFSET_REACH_ACTOR_ERASE)(actorDatum, false);
        // An AI the mod spawned stays followed (respawn), as its new rank.
        for (auto& ai : g_spawnedAi)
            if (ai.unit == unitDatum) Follow(ai, actors, objects, fresh, character, ai.team);
        return fresh;
    }

    std::unordered_set<uint32_t> g_upgradeSeen;  // actors auto upgrade has handled
    std::unordered_set<uint32_t> g_teamsSeen;    // actors auto teams has handled
    uint32_t g_upgradeAiSerial = 0;
    uint32_t g_nukeSerial = 0;
    uint32_t g_despawnSerial = 0;
    uint32_t g_randomTeamsSerial = 0;

    // An upgrade whose new rank is in a designer zone that isn't loaded: the
    // zone is loaded and the upgrade retried until it works or times out.
    struct UpgradeRetry {
        uint32_t actor;
        uint32_t character;
        uint32_t zone;
    };
    std::vector<UpgradeRetry> g_upgradeRetries;
    ULONGLONG g_upgradeRetryUntil = 0;

    // `all`: the button (every AI in range, again); otherwise auto (each AI
    // once). Seated AI (drivers, gunners, dropship troops) are skipped: auto
    // gets them once they're out.
    void UpgradeAi(void** state, const Players& p, const Array& objects, const Games::Settings& s, bool all) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        BuildUpgrades();
        if (g_upgradeTo.empty()) {
            if (all) SpawnResult("No AI to upgrade in this map");
            return;
        }
        struct Pick {
            uint32_t actor;
            uint32_t character;
        };
        std::vector<Pick> picks;  // collected first: upgrading adds actors
        int seated = 0;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            uint16_t salt = *(uint16_t*)actor;
            if (!salt) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (!all && g_upgradeSeen.count(datum)) continue;
            char* unit = ObjectFromDatum(objects, Field<uint32_t>(actor, REACH_ACTOR_UNIT));
            if (!unit || (Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS) & REACH_DAMAGE_FLAG_DEAD)) continue;
            if (!InRange(p, objects, unit, s.upgradeAiRange)) continue;
            auto to = g_upgradeTo.find(Field<uint32_t>(actor, REACH_ACTOR_CHARACTER));
            if (to == g_upgradeTo.end() || (to->second.ally && !s.upgradeAiAllies)) {
                g_upgradeSeen.insert(datum);
                continue;
            }
            if (Field<uint32_t>(unit, REACH_OBJECT_PARENT) != 0xFFFFFFFF) {
                ++seated;
                continue;
            }
            picks.push_back({datum, to->second.character});
        }

        int done = 0, loading = 0, failed = 0;
        for (const Pick& pick : picks) {
            uint32_t fresh = UpgradeActor(actors, objects, pick.actor, pick.character);
            if (fresh != 0xFFFFFFFF) {
                ++done;
                g_upgradeSeen.insert(fresh);  // auto doesn't upgrade it again
                continue;
            }
            uint32_t zone = SpawnZone(pick.character);
            if (zone) {
                LoadZones(zone);
                g_upgradeRetries.push_back({pick.actor, pick.character, zone});
                g_upgradeRetryUntil = GetTickCount64() + kZoneLoadMs;
                ++loading;
            } else {
                ++failed;
                g_upgradeSeen.insert(pick.actor);
            }
        }
        if (all || done || loading || failed)
            LOG("Reach: upgrade AI (%s): %d upgraded, %d waiting for a zone, %d failed, %d seated skipped",
                all ? "button" : "auto", done, loading, failed, seated);
        if (!all) return;
        char text[96];
        if (picks.empty()) sprintf_s(text, seated ? "No AI to upgrade (%d in vehicles)" : "No AI to upgrade", seated);
        else if (loading) sprintf_s(text, "Upgraded %d AI, loading the rest...", done);
        else sprintf_s(text, "Upgraded %d AI", done);
        SpawnResult(text);
    }

    void ContinueUpgradeRetries(void** state, const Array& objects) {
        if (g_upgradeRetries.empty()) return;
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        bool expired = GetTickCount64() > g_upgradeRetryUntil;
        int done = 0;
        for (size_t i = 0; i < g_upgradeRetries.size();) {
            UpgradeRetry& r = g_upgradeRetries[i];
            uint32_t fresh = 0xFFFFFFFF;
            bool gone = !actors.Valid() || !actors.Get(r.actor);
            if (!gone && (ActiveZones() & r.zone)) fresh = UpgradeActor(actors, objects, r.actor, r.character);
            if (fresh != 0xFFFFFFFF) {
                ++done;
                g_upgradeSeen.insert(fresh);
            } else if (!gone && !expired) {
                ++i;
                continue;
            } else if (!gone) {
                g_upgradeSeen.insert(r.actor);
                LOG("Reach: upgrade AI: actor %08X still couldn't become %s after loading its zone", r.actor,
                    ShortName(r.character).c_str());
            }
            g_upgradeRetries.erase(g_upgradeRetries.begin() + i);
        }
        if (done) LOG("Reach: upgrade AI: %d more upgraded after their zone loaded", done);
    }

    // Random swap: each AI becomes a fresh one of a random character, where it
    // stands, in its squad and on its team (UpgradeActor). The pool is every
    // character this map can make right now (no designer zone to load), minus
    // invisible pilots and turrets. Seated AI wait until they're out. `all` is
    // the button (every AI, again); otherwise each AI once, as it shows up.
    std::unordered_set<uint32_t> g_swapSeen;
    uint32_t g_randomSwapSerial = 0;

    bool SwapPoolName(const std::string& name) {
        return name.rfind("null", 0) != 0 && name.rfind("generic", 0) != 0 && name.find("turret") == std::string::npos;
    }

    void RandomSwap(void** state, const Players& p, const Array& objects, const Games::Settings& s, bool all) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        std::vector<uint32_t> pool;
        bool poolBuilt = false;
        int myTeam = PlayerTeam(p);
        struct Pick {
            uint32_t actor;
            uint32_t character;
        };
        std::vector<Pick> picks;  // collected first: swapping adds actors
        int seated = 0;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            uint16_t salt = *(uint16_t*)actor;
            if (!salt) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (!all && g_swapSeen.count(datum)) continue;
            char* unit = ObjectFromDatum(objects, Field<uint32_t>(actor, REACH_ACTOR_UNIT));
            if (!unit || p.Contains(unit) || (Field<uint32_t>(unit, REACH_OBJECT_DAMAGE_FLAGS) & REACH_DAMAGE_FLAG_DEAD))
                continue;
            uint32_t current = Field<uint32_t>(actor, REACH_ACTOR_CHARACTER);
            if (!SwapPoolName(ShortName(current)) ||
                (!s.randomSwapAllies && Friends(Field<int8_t>(unit, REACH_UNIT_TEAM), myTeam))) {
                g_swapSeen.insert(datum);
                continue;
            }
            if (Field<uint32_t>(unit, REACH_OBJECT_PARENT) != 0xFFFFFFFF) {
                ++seated;
                continue;
            }
            if (!poolBuilt) {
                poolBuilt = true;
                for (uint32_t c : SpawnableCharacters())
                    if (SwapPoolName(ShortName(c)) && SpawnZone(c) == 0) pool.push_back(c);
            }
            if (pool.size() < 2) {
                g_swapSeen.insert(datum);
                continue;
            }
            uint32_t pick = current;
            for (int tries = 0; tries < 8 && pick == current; ++tries)
                pick = pool[(size_t)(Random01() * pool.size()) % pool.size()];
            if (pick == current) {
                g_swapSeen.insert(datum);
                continue;
            }
            picks.push_back({datum, pick});
        }

        int done = 0, failed = 0;
        for (const Pick& pick : picks) {
            uint32_t fresh = UpgradeActor(actors, objects, pick.actor, pick.character);
            for (int again = 0; fresh == 0xFFFFFFFF && again < 3; ++again)
                fresh = UpgradeActor(actors, objects, pick.actor, pool[(size_t)(Random01() * pool.size()) % pool.size()]);
            if (fresh != 0xFFFFFFFF) {
                ++done;
                g_swapSeen.insert(fresh);  // auto doesn't swap it again
            } else {
                ++failed;
                g_swapSeen.insert(pick.actor);
            }
        }
        if (all || done || failed)
            LOG("Reach: random swap (%s): %d swapped, %d failed, %d seated skipped, pool %zu",
                all ? "button" : "auto", done, failed, seated, pool.size());
        if (!all) return;
        char text[96];
        if (picks.empty()) sprintf_s(text, seated ? "No AI to swap (%d in vehicles)" : "No AI to swap", seated);
        else sprintf_s(text, "Swapped %d AI", done);
        SpawnResult(text);
    }

    // Random teams: kFactionTeams (defined with the spawn team resolver); with
    // more factions it's a brawl.
    // "We all feel down sometimes": a grenade where each AI stands. The projectile
    // is the first usable one in the globals tag's grenade list (frag first).
    // `all` = the button; otherwise only AI not seen yet are handled, and the
    // first call after the checkbox is ticked just remembers the ones already there.
    std::unordered_set<uint32_t> g_grenadeSeen;
    bool g_grenadeAutoWas = false;
    uint32_t g_grenadeSerial = 0;

    uint32_t GrenadeProjectile() {
        char* matg = ReachTags::Globals();
        for (int i = 0; matg && i < 2 && i < ReachTags::BlockCount(matg, REACH_MATG_GRENADES); ++i) {
            char* el = ReachTags::BlockElement(matg, REACH_MATG_GRENADES, REACH_MATG_GRENADE_DEF_SIZE, i);
            uint32_t tag = el ? Field<uint32_t>(el, REACH_MATG_GRENADE_PROJECTILE + REACH_TAGREF_DATUM) : 0xFFFFFFFF;
            if (tag != 0xFFFFFFFF && ReachTags::Get(tag)) return tag;
        }
        return 0xFFFFFFFF;
    }

    void DropGrenades(void** state, const Players& p, const Array& objects, const Games::Settings& s, bool all) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        bool firstAuto = !all && !g_grenadeAutoWas;
        if (!all) g_grenadeAutoWas = true;
        uint32_t tag = firstAuto ? 0 : GrenadeProjectile();
        if (!firstAuto && tag == 0xFFFFFFFF) {
            if (all) SpawnResult("No grenade in this map");
            return;
        }
        int myTeam = PlayerTeam(p);
        int dropped = 0;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            uint16_t salt = *(uint16_t*)actor;
            if (!salt) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (!all && g_grenadeSeen.count(datum)) continue;
            char* unit = ObjectFromDatum(objects, Field<uint32_t>(actor, REACH_ACTOR_UNIT));
            if (!unit || p.Contains(unit)) continue;
            float* health = Health(unit);
            if (!health || *health <= 0.0f) continue;
            g_grenadeSeen.insert(datum);
            if (firstAuto) continue;
            if (s.grenadeEnemiesOnly && Friends(Field<int8_t>(unit, REACH_UNIT_TEAM), myTeam)) continue;
            float at[3];
            memcpy(at, unit + REACH_OBJECT_POSITION, sizeof(at));
            at[2] += 0.3f;
            if (NewObject(tag, 0, at, 0.0f) != 0xFFFFFFFF) ++dropped;
        }
        if (all || dropped) LOG("Reach: grenades (%s): %d dropped", all ? "button" : "auto", dropped);
        if (all) {
            char text[96];
            sprintf_s(text, dropped ? "Dropped %d grenades" : "No AI to drop grenades under", dropped);
            SpawnResult(text);
        }
    }

    void RandomTeams(void** state, const Players& p, const Array& objects, const Games::Settings& s, bool all) {
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        int factions = (int)(s.randomTeamsFactions + 0.5f);
        int most = (int)_countof(kFactionTeams);
        factions = factions < 2 ? 2 : (factions > most ? most : factions);
        int changed = 0;
        for (int i = 0; i < actors.HighWater(); ++i) {
            char* actor = actors.Entry(i);
            uint16_t salt = *(uint16_t*)actor;
            if (!salt) continue;
            uint32_t datum = ((uint32_t)salt << 16) | (uint32_t)i;
            if (!all && g_teamsSeen.count(datum)) continue;
            char* unit = ObjectFromDatum(objects, Field<uint32_t>(actor, REACH_ACTOR_UNIT));
            if (!unit || !InRange(p, objects, unit, s.randomTeamsRange)) continue;
            g_teamsSeen.insert(datum);
            // Invisible scripted pilots (dropships, Falcons) keep theirs, and
            // Spartans unless asked.
            std::string name = ShortName(Field<uint32_t>(actor, REACH_ACTOR_CHARACTER));
            if (name.rfind("null", 0) == 0) continue;
            if (!s.randomTeamsSpartans && name.rfind("spartan", 0) == 0) continue;
            int team = kFactionTeams[(int)(Random01() * factions) % factions];
            Engine<ActorSetTeamFn>(OFFSET_REACH_ACTOR_SET_TEAM)(datum, team);
            ++changed;
        }
        if (all || changed)
            LOG("Reach: random AI teams (%s): %d AI over %d teams", all ? "button" : "auto", changed, factions);
        if (all) {
            char text[96];
            sprintf_s(text, changed ? "Shuffled %d AI over %d teams" : "No AI in range", changed, factions);
            SpawnResult(text);
        }
    }

    // Your team: the player record's team, which the game copies to the
    // player's unit every tick (offsets.h). Switching back to normal puts
    // the player team back once; a new map starts on it anyway.
    const int kPlayerTeams[] = {REACH_TEAM_PLAYER, REACH_TEAM_COVENANT, REACH_TEAM_SPARE};
    bool g_teamForced = false;

    void SetPlayerTeam(void** state, int choice) {
        if (choice <= 0 && !g_teamForced) return;
        int count = (int)_countof(kPlayerTeams);
        int team = kPlayerTeams[choice > 0 && choice < count ? choice : 0];
        Array players{(char*)state[REACH_TLS_SLOT_PLAYERS]};
        if (!players.Valid()) return;
        int limit = players.HighWater() < 16 ? players.HighWater() : 16;
        for (int i = 0; i < limit; ++i) {
            char* player = players.Entry(i);
            if (*(uint16_t*)player == 0) continue;
            int8_t& current = Field<int8_t>(player, REACH_PLAYER_TEAM);
            if (current == team) continue;
            LOG("Reach: player %d team %d -> %d", i, current, team);
            current = (int8_t)team;
        }
        g_teamForced = choice > 0;
    }

    // Friendly fire diagnostics: logs each time an allied AI's health drops, with what the
    // damage hook saw since the last drop (calls on allies, blocked, let through).
    void WatchAllyHealth(void** state, const Players& p, const Array& objects, bool on) {
        static float s_health[64];
        static uint32_t s_datum[64];
        static int s_logged = 0;
        if (!on) return;
        Array actors{(char*)state[REACH_TLS_SLOT_ACTORS]};
        if (!actors.Valid()) return;
        int me = PlayerTeam(p);
        for (int i = 0; i < actors.HighWater() && i < 64; ++i) {
            char* actor = actors.Entry(i);
            uint32_t datum = Field<uint32_t>(actor, REACH_ACTOR_UNIT);
            char* unit = *(uint16_t*)actor ? ObjectFromDatum(objects, datum) : nullptr;
            float* health = unit ? Health(unit) : nullptr;
            if (!health || p.Contains(unit) || !Friends(Field<int8_t>(unit, REACH_UNIT_TEAM), me)) {
                s_datum[i] = 0xFFFFFFFF;
                continue;
            }
            if (s_datum[i] == datum && *health < s_health[i] - 0.0005f && s_logged < 60) {
                ++s_logged;
                FriendlyFireStats st = TakeFriendlyFireStats();
                LOG("Reach: ally %08X health %.3f -> %.3f; hook saw %u hits on allies (%u blocked, %u let through, %u no owner), last effect %08X flags %08X owner %08X",
                    datum, s_health[i], *health, st.onAllies, st.blocked, st.throughOwned, st.noOwner, st.lastEffect,
                    st.lastFlags, st.lastOwner);
            }
            s_datum[i] = datum;
            s_health[i] = *health;
        }
    }

    // Each cheat gets its own guard so one fault can't stop the others.
    template <typename Fn>
    void Guard(const char* what, Fn fn) {
        __try {
            fn();
        } __except (Log::Exception(GetExceptionInformation(), what)) {
        }
    }

    bool ReadPlayers(void** state, Array& objects, Players& p) {
        __try {
            Array players{(char*)state[REACH_TLS_SLOT_PLAYERS]};
            objects.header = (char*)state[REACH_TLS_SLOT_OBJECTS];
            if (!players.Valid() || !objects.Valid()) return false;
            p = FindPlayers(players, objects);
            return true;
        } __except (Log::Exception(GetExceptionInformation(), "Reach main-thread players")) {
            return false;
        }
    }

    void RunAll(void** state, const Games::Settings& s, const Games::FrameInput& in) {
        Array objects;
        Players p;
        if (!ReadPlayers(state, objects, p)) return;

        g_randomGuns = s.spawnRandomGuns;
        g_state = state;
        Guard("Reach friendly fire watch", [&] { WatchAllyHealth(state, p, objects, s.noFriendlyFire); });
        Guard("Reach your team", [&] { SetPlayerTeam(state, s.playerTeam); });
        g_myTeam = PlayerTeam(p);
        float dt = TickSeconds(g_lastTick);
        Guard("Reach noclip", [&] { Noclip(state, p, s.noclip, in, dt); });
        Guard("Reach damage multiplier", [&] { Damage(p, objects, s.damage); });
        Guard("Reach launch on hit", [&] { LaunchOnHit(p, objects, s.launchStrength, s.launchOnHit); });
        Guard("Reach rapid fire", [&] { RapidFire(p, objects, s.rapidFire, s.vehicleRapidFire); });
        Guard("Reach airstrikes", [&] { Airstrike(state, s.infiniteAmmo, s.rapidFire); });
        Guard("Reach aimbot strength", [&] {
            if (s.aimbot && in.firing) AimPull(state, p, objects, s.aimbotStrength);
        });
        if (s.saveCheckpointSerial != g_saveSerial) {
            g_saveSerial = s.saveCheckpointSerial;
            LOG("Reach: saving a checkpoint");
            Guard("Reach save checkpoint", [&] {
                g_ownSave = true;  // the lock lets this one through
                Engine<void(__fastcall*)(int32_t)>(OFFSET_REACH_GAME_SAVE)(REACH_GAME_SAVE_IMMEDIATE);
                g_ownSave = false;
            });
            g_ownSave = false;
        }
        if (s.loadCheckpointSerial != g_loadSerial) {
            g_loadSerial = s.loadCheckpointSerial;
            LOG("Reach: loading the last checkpoint");
            Guard("Reach load checkpoint", [&] {
                *(uint16_t*)(g_module + OFFSET_REACH_GAME_REVERT_FLAG) = REACH_GAME_REVERT_REQUEST;
            });
        }
        g_checkpointsLocked = s.lockCheckpoints;
        Guard("Reach checkpoint history", [&] { PollCheckpoints(); });
        if (s.restoreCheckpointSerial != g_restoreSerial) {
            g_restoreSerial = s.restoreCheckpointSerial;
            Guard("Reach restore checkpoint", [&] { RestoreCheckpoint(s.restoreCheckpoint); });
        }
        Guard("Reach designer zones", [&] { KeepForcedZones(); });
        if (s.upgradeVehicleSerial != g_upgradeSerial) {
            g_upgradeSerial = s.upgradeVehicleSerial;
            Guard("Reach upgrade vehicle", [&] { UpgradeVehicle(p, objects); });
        }
        Guard("Reach upgrade vehicle wait", [&] { ContinueUpgrade(p, objects); });
        if (s.takeSeatSerial != g_takeSeatSerial) {
            g_takeSeatSerial = s.takeSeatSerial;
            if (s.pilotAiVehicles) Guard("Reach take pilot seat", [&] { TakePilotSeat(state, p, objects); });
        }
        Guard("Reach take pilot seat wait", [&] { ContinueTakeSeat(p, objects); });
        Guard("Reach hull exit", [&] { ExitHull(p, objects, s.pilotAiVehicles, in.exitHeld, dt); });
        if (s.spawnSerial != g_spawnSerial) {
            g_spawnSerial = s.spawnSerial;
            if (s.spawnEnabled && s.spawnSquad >= 0)
                Guard("Reach spawn squad", [&] { SpawnSquad(state, p, s.spawnSquad, s.spawnTeam); });
            else if (s.spawnEnabled && s.spawnTag != 0xFFFFFFFF)
                Guard("Reach spawn", [&] { Spawn(state, p, s.spawnTag, s.spawnTeam); });
        }
        Guard("Reach squad wait", [&] { ContinueSquadWait(state); });
        Guard("Reach spawned AI", [&] { FollowSpawnedAi(state, p, objects, s.spawnRespawn); });
        Guard("Reach skulls", [&] { Skulls(state, s.skulls); });
        if (s.teleportSerial != g_teleportSerial) {
            g_teleportSerial = s.teleportSerial;
            if (s.teleportEnabled) Guard("Reach teleport", [&] { Teleport(state, p, objects); });
        }
        Guard("Reach boundary occlusion", [&] {
            if (p.count > 0)
                BoundaryOcclusion(state, s, p.datums[0], Field<uint32_t>(p.units[0], REACH_OBJECT_PARENT));
        });
        Guard("Reach air jump", [&] { AirJump(p, objects, s, in); });
        Guard("Reach AI size", [&] { ScaleAi(state, p, objects, s.aiScaleEnemy, s.aiScaleFriendly); });
        Guard("Reach rain", [&] { Rain(state, p, objects, s, dt); });
        Guard("Reach recoil push", [&] { ApplyPushes(objects); });
        Guard("Reach AI stay dead", [&] { AiStayDead(state, objects, s.aiStayDead); });
        if (s.upgradeAiSerial != g_upgradeAiSerial) {
            g_upgradeAiSerial = s.upgradeAiSerial;
            Guard("Reach upgrade AI", [&] { UpgradeAi(state, p, objects, s, true); });
        }
        Guard("Reach drop shield", [&] { DropShieldBlocks(p, objects); });
        Guard("Reach time stop", [&] { TimeStop(state, p, objects, s.timeStop); });
        if (s.poisonTouch) Guard("Reach poison touch", [&] { PoisonTouch(state, p, objects); });
        if (s.nukeSerial != g_nukeSerial) {
            g_nukeSerial = s.nukeSerial;
            Guard("Reach nuke", [&] { Nuke(state, p, objects, s.nukeSpareAllies); });
        }
        if (s.despawnSerial != g_despawnSerial) {
            g_despawnSerial = s.despawnSerial;
            Guard("Reach despawn custom AI", [&] { DespawnCustomAi(state, objects); });
        }
        if (s.upgradeAiAuto) Guard("Reach upgrade AI auto", [&] { UpgradeAi(state, p, objects, s, false); });
        Guard("Reach upgrade AI retries", [&] { ContinueUpgradeRetries(state, objects); });
        if (s.randomSwapSerial != g_randomSwapSerial) {
            g_randomSwapSerial = s.randomSwapSerial;
            Guard("Reach random swap", [&] { RandomSwap(state, p, objects, s, true); });
        }
        if (s.randomSwapAuto) Guard("Reach random swap auto", [&] { RandomSwap(state, p, objects, s, false); });
        if (s.randomTeamsSerial != g_randomTeamsSerial) {
            g_randomTeamsSerial = s.randomTeamsSerial;
            Guard("Reach random AI teams", [&] { RandomTeams(state, p, objects, s, true); });
        }
        if (s.randomTeamsAuto) Guard("Reach random AI teams auto", [&] { RandomTeams(state, p, objects, s, false); });
        if (s.grenadeSerial != g_grenadeSerial) {
            g_grenadeSerial = s.grenadeSerial;
            Guard("Reach drop grenades", [&] { DropGrenades(state, p, objects, s, true); });
        } else if (s.grenadeAuto) {
            Guard("Reach drop grenades auto", [&] { DropGrenades(state, p, objects, s, false); });
        } else if (g_grenadeAutoWas) {
            g_grenadeAutoWas = false;
            g_grenadeSeen.clear();
        }
        bool request = s.giveWeaponSerial != g_giveSerial;
        if (request) {
            g_giveSerial = s.giveWeaponSerial;
            LOG("Reach: give weapon request %08X (%d player units)", s.giveWeapon, p.count);
        }
        __try {
            if (request) {
                g_giveWait = {};
                bool made = GiveWeapon(p, objects, s.giveWeapon);
                uint32_t zone = made ? 0 : ZoneToLoad(s.giveWeapon);
                if (!made && !zone) NotLoaded(s.giveWeapon);
                if (zone) {
                    LoadZones(zone);
                    g_giveWait.active = true;
                    g_giveWait.tag = s.giveWeapon;
                    g_giveWait.zone = zone;
                    g_giveWait.deadline = GetTickCount64() + kZoneLoadMs;
                    LOG("Reach: give weapon %08X: loading designer zone %08X first", s.giveWeapon, zone);
                }
            }
            ContinueGive(p);
        } __except (Log::Exception(GetExceptionInformation(), "Reach give weapon")) {
            g_give = {};
            g_giveWait = {};
        }
        if (s.giveEquipmentSerial != g_giveEquipmentSerial) {
            g_giveEquipmentSerial = s.giveEquipmentSerial;
            LOG("Reach: give equipment request %08X (%d player units)", s.giveEquipment, p.count);
            Guard("Reach give equipment", [&] {
                if (!GiveEquipment(p, objects, s.giveEquipment)) NotLoaded(s.giveEquipment);
            });
        }
        Guard("Reach designer zone waits", [&] { ContinueZoneWaits(state, p, objects); });
    }
}

namespace ReachCheats {
    void SetMainThreadModule(HMODULE reach) { g_module = (char*)reach; }

    bool CheckpointsLocked() { return g_checkpointsLocked && !g_ownSave; }

    void CheckpointHistory(std::vector<Games::CheckpointEntry>& out) {
        std::lock_guard<std::mutex> lock(g_checkpointLock);
        for (const StoredCheckpoint& saved : g_checkpoints) {
            Games::CheckpointEntry entry;
            entry.id = saved.id;
            entry.when = saved.when;
            time_t t = (time_t)saved.when;
            tm local{};
            localtime_s(&local, &t);
            char text[16];
            strftime(text, sizeof(text), "%H:%M:%S", &local);
            entry.label = text;
            out.push_back(std::move(entry));
        }
    }

    void SpawnStatus(Games::Status& status) {
        std::lock_guard<std::mutex> lock(g_spawnLock);
        status.spawnDone = g_spawnDone;
        sprintf_s(status.spawnMessage, "%s", g_spawnMessage);
    }

    void ResetMainThread() {
        g_swapSeen.clear();
        g_scaledAi.clear();  // those units went with the map
        g_frozenObjects.clear();
        g_timeStopOn = false;
        g_frozenActors.clear();  // the actors went with the map
        g_noclipActive = false;
        g_noclipUnit = 0xFFFFFFFF;
        g_lastTick = {};
        g_give = {};  // its objects went with the map
        memset(g_tracked, 0, sizeof(g_tracked));
        memset(g_launchTracked, 0, sizeof(g_launchTracked));
        for (auto& ai : g_spawnedAi) ai = {};
        g_unprotectedCount = g_removedReviveCount = g_deadCharacterCount = 0;  // AI stay dead: those units went with the map
        g_stayDeadApplied = false;
        g_parkedCount = 0;
        g_airstrikeSlot = nullptr;
        g_spawnWait = {};
        g_squadPending.clear();
        g_giveWait = {};
        g_upgrade = {};
        g_takeSeat = {};
        g_hullExit = {};
        g_tookVehicle = 0xFFFFFFFF;
        AllowEject(0xFFFFFFFF);
        g_forcedZones = 0;  // the tag edits went with the map
        g_forcedScenario = nullptr;
        g_skullsForced = 0;
        g_skullGlobals = nullptr;
        g_rainCount = 0;
        g_rainTimer = 0.0f;
        g_upgradeSeen.clear();  // actor datums are per map
        g_teamsSeen.clear();
        g_grenadeSeen.clear();
        g_grenadeAutoWas = false;
        g_teamForced = false;  // a new map's players start on the player team
        g_upgradeRetries.clear();
        g_ladderCharacters.clear();
        g_upgradeTo.clear();
    }
}

bool ReachCheats::ObjectFrozen(uint32_t datum) {
    return g_timeStopOn && g_frozenObjects.count(datum) != 0;
}

void ReachCheats::MainThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input) {
    if (!g_module || !gameState) return;
    // Don't run any "Give" that was pressed before this session.
    static bool s_first = true;
    if (s_first) {
        g_giveSerial = settings.giveWeaponSerial;
        g_giveEquipmentSerial = settings.giveEquipmentSerial;
        g_saveSerial = settings.saveCheckpointSerial;
        g_loadSerial = settings.loadCheckpointSerial;
        g_restoreSerial = settings.restoreCheckpointSerial;
        g_spawnSerial = settings.spawnSerial;
        g_upgradeSerial = settings.upgradeVehicleSerial;
        g_takeSeatSerial = settings.takeSeatSerial;
        g_teleportSerial = settings.teleportSerial;
        g_upgradeAiSerial = settings.upgradeAiSerial;
        g_randomSwapSerial = settings.randomSwapSerial;
        g_nukeSerial = settings.nukeSerial;
        g_grenadeSerial = settings.grenadeSerial;
        g_despawnSerial = settings.despawnSerial;
        g_randomTeamsSerial = settings.randomTeamsSerial;
        s_first = false;
    }
    RunAll(gameState, settings, input);
}
