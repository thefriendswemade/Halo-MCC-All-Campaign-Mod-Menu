// Halo 3 cheats that call engine functions. Runs once per game tick on the
// game's main thread (from MCC's input polling).

#include <windows.h>

#include <cmath>
#include <cstring>

#include "cheat_util.h"
#include "cheats.h"
#include "core/log.h"
#include "internal.h"
#include "spawn.h"
#include "tags.h"

using namespace Halo3Cheats;

namespace {
    constexpr float kNoclipSpeed = 6.0f;      // world units per second
    constexpr float kNoclipFastScale = 4.0f;
    constexpr int kMaxTracked = 4096;         // object table slots we track for damage
    constexpr float kPi = 3.14159265f;
    constexpr float kAimMinCone = 3.0f;       // degrees from the crosshair, at 1% strength
    constexpr float kAimMaxCone = 55.0f;      // at 100%: about the whole screen
    constexpr float kAimRange = 200.0f;       // world units
    constexpr int kMaxOwnEquipment = 16;

    using PlacementNewFn = void(__fastcall*)(void* placement, uint32_t tag, uint32_t owner, int64_t);
    using ObjectNewFn = uint32_t(__fastcall*)(void* placement);
    using ObjectDeleteFn = void(__fastcall*)(uint32_t object);
    using UnitAddWeaponFn = bool(__fastcall*)(uint32_t unit, uint32_t weapon, int mode);
    using UnitKillFn = void(__fastcall*)(uint32_t unit);
    using HeadPositionFn = void(__fastcall*)(uint32_t unit, float* out);

    char* g_module = nullptr;  // set by Halo3Cheats::Load via SetModule below

    template <typename Fn>
    Fn Engine(int rva) { return (Fn)(g_module + rva); }

    // Noclip
    bool g_noclipActive = false;
    // Noclip's per-tick teleport faults before it finishes (e.g. before the
    // unit is reconnected to the map), so when noclip ends we run one full
    // teleport from the frame hook, where it doesn't fault.
    bool g_noclipFinishPending = false;
    // The TLS thread index as the frame sets it; borrowed for noclip's teleport.
    uint32_t g_noclipUnit = 0xFFFFFFFF;
    float g_anchor[3] = {};
    LARGE_INTEGER g_lastFunTick{};  // Fun page timers (rain)
    LARGE_INTEGER g_lastTick{};       // frame path
    LARGE_INTEGER g_lastInputTick{};  // input-polling path (noclip)

    // Damage multiplier: last seen vitality per object slot.
    struct Tracked {
        uint32_t datum;
        float health;
        float shield;
    };
    Tracked g_tracked[kMaxTracked];

    uint32_t g_giveSerial = 0;
    uint32_t g_saveSerial = 0;
    uint32_t g_loadSerial = 0;

    float TickSeconds(LARGE_INTEGER& last) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
        last = now;
        return dt > 0.1f ? 0.1f : dt;
    }

    // Moves an object without touching its facing (or a unit's aim), and
    // reconnects it to the map; velocity is cleared so it doesn't drift. The
    // fifth argument is an optional location pointer handed to the map
    // reconnect: it must be a full 64-bit null (passed as a 32-bit 0, the
    // stack slot's high half was garbage and the reconnect faulted on it).
    void SetPosition(char* objectData, uint32_t object, const float* pos) {
        __try {
            Engine<void(__fastcall*)(uint32_t, const float*, const float*, const float*, const void*, bool)>(
                OFFSET_HALO3_OBJECT_SET_POSITION)(object, pos, nullptr, nullptr, nullptr, false);
            memset(&Field<float>(objectData, HALO3_OBJECT_VELOCITY), 0, 3 * sizeof(float));
        } __except (Log::Exception(GetExceptionInformation(), "noclip set position")) {
        }
    }

    // Holds player 1 in place, and flies them where they're looking while
    // the noclip button is held. The engine's teleport ignores collision.
    void Noclip(void** state, const Players& p, bool on, const Games::FrameInput& in, float dt) {
        // Paused while seated (vehicle, turret): re-anchor where they get out.
        bool seated = p.count > 0 && Field<uint32_t>(p.units[0], HALO3_OBJECT_PARENT) != 0xFFFFFFFF;
        bool sameUnit = p.count > 0 && p.datums[0] == g_noclipUnit && !seated;
        if (g_noclipActive && (!on || !sameUnit)) {
            g_noclipActive = false;
            g_noclipFinishPending = p.count > 0 && p.datums[0] == g_noclipUnit && !seated;
            LOG("Halo 3: noclip off");
        }
        if (!on || p.count == 0 || seated) return;

        char* unit = p.units[0];
        float* pos = &Field<float>(unit, HALO3_OBJECT_POSITION);
        if (!g_noclipActive) {
            memcpy(g_anchor, pos, sizeof(g_anchor));
            g_noclipUnit = p.datums[0];
            g_noclipActive = true;
            LOG("Halo 3: noclip on at %.2f %.2f %.2f", pos[0], pos[1], pos[2]);
        }

        if (in.noclipForward) {
            char* control = (char*)state[HALO3_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
            float yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
            float pitch = Field<float>(control, HALO3_PLAYER_CONTROL_PITCH);
            float step = kNoclipSpeed * (in.fast ? kNoclipFastScale : 1.0f) * dt;
            g_anchor[0] += cosf(yaw) * cosf(pitch) * step;
            g_anchor[1] += sinf(yaw) * cosf(pitch) * step;
            g_anchor[2] += sinf(pitch) * step;
        }

        SetPosition(unit, p.datums[0], g_anchor);
    }

    // Scales the health and shield enemies lost since last tick. Kills them
    // properly if the scaled damage is fatal.
    void Damage(const Players& p, Blam::DataArray* objects, float m) {
        if (m == 1.0f) {
            memset(g_tracked, 0, sizeof(g_tracked));
            return;
        }

        for (int i = 0; i < objects->highWater && i < kMaxTracked; ++i) {
            auto entry = (Blam::ObjectEntry*)objects->Entry(i);
            char* object = entry->object;
            Tracked& t = g_tracked[i];
            if (entry->salt == 0 || !object || !IsKillableEnemy(object, p)) {
                t.datum = 0;
                continue;
            }

            uint32_t datum = ((uint32_t)entry->salt << 16) | (uint32_t)i;
            auto& health = Field<float>(object, HALO3_OBJECT_HEALTH);
            auto& shield = Field<float>(object, HALO3_OBJECT_SHIELD);
            if (t.datum != datum) {
                t = {datum, health, shield};
                continue;
            }
            if (health <= 0.0f) {  // already dead
                t.datum = 0;
                continue;
            }

            if (shield < t.shield) {
                float s = t.shield - (t.shield - shield) * m;
                shield = s > 0.0f ? s : 0.0f;
            }
            if (health < t.health) {
                float h = t.health - (t.health - health) * m;
                if (h <= 0.0f) {
                    health = 0.0001f;
                    Engine<UnitKillFn>(OFFSET_HALO3_UNIT_KILL)(datum);
                    t.datum = 0;
                    continue;
                }
                health = h;
            }
            t.health = health;
            t.shield = shield;
        }
    }

    void LogInventory(const char* when, char* unit) {
        LOG("Halo 3:   inventory %s: current %d, slots %08X %08X %08X %08X", when,
            Field<int8_t>(unit, HALO3_UNIT_CURRENT_WEAPON), Field<uint32_t>(unit, HALO3_UNIT_WEAPONS),
            Field<uint32_t>(unit, HALO3_UNIT_WEAPONS + 4), Field<uint32_t>(unit, HALO3_UNIT_WEAPONS + 8),
            Field<uint32_t>(unit, HALO3_UNIT_WEAPONS + 12));
    }

    // A give waiting for the game to free the held weapon's slot. Deleting
    // an object takes effect a tick or so later, so the new weapon can't be
    // added in the same tick.
    struct PendingGive {
        bool active = false;
        uint32_t weapon = 0xFFFFFFFF;  // new weapon, already created
        uint32_t held = 0xFFFFFFFF;    // weapon being replaced (deleted)
        int frames = 0;
    };
    PendingGive g_give;

    int SlotOf(char* unit, uint32_t weapon) {
        for (int w = 0; w < HALO3_UNIT_WEAPON_COUNT; ++w)
            if (Field<uint32_t>(unit, HALO3_UNIT_WEAPONS + w * 4) == weapon) return w;
        return -1;
    }

    // Adds the weapon into a free slot and switches to it. Deletes it if the
    // game won't take it.
    void AddAndSelect(char* unit, uint32_t unitDatum, uint32_t weapon) {
        bool added = Engine<UnitAddWeaponFn>(OFFSET_HALO3_UNIT_ADD_WEAPON)(unitDatum, weapon,
                                                                           HALO3_ADD_WEAPON_FREE_SLOT);
        int slot = added ? SlotOf(unit, weapon) : -1;
        if (slot >= 0) Field<int8_t>(unit, HALO3_UNIT_DESIRED_WEAPON) = (int8_t)slot;
        else Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(weapon);
        LogInventory("after", unit);
        LOG("Halo 3: give weapon: %s (object %08X, slot %d)", slot >= 0 ? "added" : "add FAILED", weapon, slot);
    }

    // Replaces player 1's held weapon with a new one of the chosen type (or
    // fills a free slot). The new weapon is created first, so a type the
    // game can't create costs nothing.
    void GiveWeapon(const Players& p, Blam::DataArray* objects, uint32_t weaponTag) {
        if (p.count == 0) { LOG("Halo 3: give weapon: no player unit"); return; }
        if (!Halo3Tags::Get(weaponTag)) { LOG("Halo 3: give weapon %08X: tag not loaded", weaponTag); return; }
        char* unit = p.units[0];
        uint32_t unitDatum = p.datums[0];

        if (g_give.active) {  // replaced by this newer request
            Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(g_give.weapon);
            g_give = {};
        }

        static uint8_t placement[HALO3_PLACEMENT_DATA_SIZE];
        memset(placement, 0, sizeof(placement));
        Engine<PlacementNewFn>(OFFSET_HALO3_OBJECT_PLACEMENT_NEW)(placement, weaponTag, unitDatum, 0);
        uint32_t weapon = Engine<ObjectNewFn>(OFFSET_HALO3_OBJECT_NEW)(placement);
        if (weapon == 0xFFFFFFFF) {
            LOG("Halo 3: give weapon %08X: the game couldn't create it here (vehicle-only, or not loaded in this "
                "part of the level)", weaponTag);
            return;
        }
        LogInventory("before", unit);

        bool freeSlot = false;
        for (int w = 0; w < HALO3_PLAYER_WEAPON_SLOTS; ++w)
            freeSlot |= Field<uint32_t>(unit, HALO3_UNIT_WEAPONS + w * 4) == 0xFFFFFFFF;

        uint32_t held = 0xFFFFFFFF;
        if (freeSlot || !HeldWeapon(unit, objects, &held)) {
            AddAndSelect(unit, unitDatum, weapon);
            return;
        }

        Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(held);
        g_give = {true, weapon, held, 0};
        LOG("Halo 3: give weapon %08X: removing held %08X, adding once its slot frees", weaponTag, held);
    }

    // Finishes a pending give once the held weapon has left the inventory.
    void ContinueGive(const Players& p) {
        if (!g_give.active) return;
        if (p.count == 0) {  // died: drop the request
            Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(g_give.weapon);
            g_give = {};
            return;
        }
        char* unit = p.units[0];
        if (SlotOf(unit, g_give.held) < 0) {
            AddAndSelect(unit, p.datums[0], g_give.weapon);
            g_give = {};
        } else if (++g_give.frames > 120) {
            Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(g_give.weapon);
            LOG("Halo 3: give weapon: held weapon never left the inventory; gave up");
            g_give = {};
        }
    }

    // Edits the players' weapon state before the game updates it this tick,
    // so only their weapons are affected (the Cheat Engine table patches the
    // shared weapon code, which buffs the AI too).
    // Vehicle rapid fire gives the weapons of the vehicle each player is in,
    // and of its turrets (the Wraith's mortar is one), full-auto and rapid fire.
    void WeaponMods(const Players& p, Blam::DataArray* objects, const Games::Settings& s) {
        if (!s.fullAuto && !s.rapidFire && !s.noSpread && !s.noOverheat && !s.vehicleRapidFire) return;
        // No overheat also covers the vehicle (and its turrets) each player is in.

        constexpr int kMaxParts = 8;
        struct Shooter { char* unit; bool fullAuto, rapidFire, noSpread, noOverheat; };
        Shooter units[kMaxLocalPlayers * (1 + kMaxParts)];
        int count = 0;
        for (int i = 0; i < p.count; ++i) {
            units[count++] = {p.units[i], s.fullAuto, s.rapidFire, s.noSpread, s.noOverheat};
            if (!s.vehicleRapidFire && !s.noOverheat) continue;
            char* parts[kMaxParts];
            int partCount = VehicleParts(p.units[i], objects, parts, kMaxParts);
            for (int k = 0; k < partCount; ++k)
                units[count++] = {parts[k], s.vehicleRapidFire, s.vehicleRapidFire, false, s.noOverheat};
        }
        for (int i = 0; i < count; ++i) {
            const Shooter& u = units[i];
            for (int w = 0; w < HALO3_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = Blam::ObjectFromDatum(objects, Field<uint32_t>(u.unit, HALO3_UNIT_WEAPONS + w * 4));
                if (!weapon || Field<uint8_t>(weapon, HALO3_OBJECT_TYPE) != HALO3_OBJECT_TYPE_WEAPON) continue;

                // Full-auto: the trigger always counts as released since the last shot.
                if (u.fullAuto)
                    for (int t = 0; t < HALO3_WEAPON_TRIGGER_COUNT; ++t)
                        Field<uint16_t>(weapon, HALO3_WEAPON_TRIGGERS + t * HALO3_WEAPON_TRIGGER_SIZE +
                                                    HALO3_TRIGGER_FLAGS) |= HALO3_TRIGGER_FLAG_READY;

                for (int b = 0; b < HALO3_WEAPON_BARREL_COUNT; ++b) {
                    char* barrel = weapon + HALO3_WEAPON_BARRELS + b * HALO3_WEAPON_BARREL_SIZE;
                    // Rapid fire: ready to shoot every tick, ignoring the weapon's rate of fire.
                    if (u.rapidFire) {
                        Field<int8_t>(barrel, HALO3_BARREL_TICKS_SINCE_SHOT) = HALO3_BARREL_TICKS_MAX;
                        Field<int16_t>(barrel, HALO3_BARREL_FIRE_DELAY) = 0;
                    }
                    if (u.noSpread) Field<float>(barrel, HALO3_BARREL_ERROR) = 0.0f;
                }

                if (u.noOverheat) {
                    Field<float>(weapon, HALO3_WEAPON_HEAT) = 0.0f;
                    Field<uint16_t>(weapon, HALO3_WEAPON_FLAGS) &= (uint16_t)~HALO3_WEAPON_FLAG_OVERHEATED;
                }
            }
        }
    }

    // Equipment the players have carried, so no-despawn can keep it alive
    // after it's used and dropped (and leave the AI's equipment alone).
    uint32_t g_ownEquipment[kMaxOwnEquipment];  // 0 or -1 = empty (datums always have a salt)
    bool EmptySlot(uint32_t datum) { return datum == 0xFFFFFFFF || (datum >> 16) == 0; }

    void Equipment(void** state, const Players& p, Blam::DataArray* objects, const Games::Settings& s) {
        if (!s.infiniteEquipment && !s.noEquipmentDespawn) {
            memset(g_ownEquipment, 0xFF, sizeof(g_ownEquipment));
            return;
        }

        for (int i = 0; i < p.count; ++i) {
            uint32_t datum = Field<uint32_t>(p.units[i], HALO3_UNIT_EQUIPMENT);
            char* equipment = Blam::ObjectFromDatum(objects, datum);
            if (!equipment || Field<uint8_t>(equipment, HALO3_OBJECT_TYPE) != HALO3_OBJECT_TYPE_EQUIPMENT) continue;

            // Infinite: spent one past the limit, so uses left is never exactly 0.
            char* tag = Halo3Tags::Get(Field<uint32_t>(equipment, HALO3_OBJECT_TAG_DATUM));
            int16_t uses = tag ? Field<int16_t>(tag, HALO3_EQIP_USES) : -1;
            if (s.infiniteEquipment && uses > 0 && uses < 255)
                Field<uint8_t>(equipment, HALO3_EQUIPMENT_USES_SPENT) = (uint8_t)(uses + 1);

            bool known = false;
            for (uint32_t own : g_ownEquipment) known |= own == datum;
            for (auto& own : g_ownEquipment)
                if (!known && EmptySlot(own)) { own = datum; known = true; }
        }

        if (!s.noEquipmentDespawn) return;
        char* gameTime = (char*)state[HALO3_TLS_SLOT_GAME_TIME];
        if (!gameTime) return;
        int32_t now = Field<int32_t>(gameTime, HALO3_GAME_TIME_TICKS);
        for (auto& own : g_ownEquipment) {
            if (EmptySlot(own)) continue;
            // Not while it's still carried: using it again (and picking up
            // other equipment) waits for the carried one's effect to end.
            bool carried = false;
            for (int i = 0; i < p.count; ++i) carried |= Field<uint32_t>(p.units[i], HALO3_UNIT_EQUIPMENT) == own;
            if (carried) continue;
            char* equipment = Blam::ObjectFromDatum(objects, own);
            if (!equipment || Field<uint8_t>(equipment, HALO3_OBJECT_TYPE) != HALO3_OBJECT_TYPE_EQUIPMENT) {
                own = 0xFFFFFFFF;  // gone
                continue;
            }
            // Active equipment expires a set time after activation: keep it just activated.
            auto& activated = Field<int32_t>(equipment, HALO3_EQUIPMENT_ACTIVATED);
            if (activated != -1) activated = now;
        }
    }

    // Aimbot strength: while firing, turns player 1's view toward the enemy
    // head nearest the crosshair. Strength widens the cone searched (up to about the whole
    // screen) and speeds the turn (100% snaps). No line-of-sight check, so an
    // enemy behind cover can pull too.
    void AimPull(void** state, const Players& p, Blam::DataArray* objects, float strength) {
        if (strength <= 0.0f || p.count == 0) return;
        if (IsMeleeOnly(HeldWeapon(p.units[0], objects))) return;  // energy sword, hammer: no pull
        float s = strength / 100.0f;
        auto head = Engine<HeadPositionFn>(OFFSET_HALO3_UNIT_HEAD_POSITION);

        char* control = (char*)state[HALO3_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        float& yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
        float& pitch = Field<float>(control, HALO3_PLAYER_CONTROL_PITCH);
        float forward[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        head(p.datums[0], eye);

        float minCos = cosf((kAimMinCone + (kAimMaxCone - kAimMinCone) * s) * kPi / 180.0f);
        float best[3] = {};
        float bestCos = minCos;
        bool found = false;
        for (int i = 0; i < objects->highWater; ++i) {
            auto entry = (Blam::ObjectEntry*)objects->Entry(i);
            char* object = entry->object;
            if (entry->salt == 0 || !object || !IsKillableEnemy(object, p)) continue;
            if (Field<float>(object, HALO3_OBJECT_HEALTH) <= 0.0f) continue;

            float pos[3];
            head(((uint32_t)entry->salt << 16) | (uint32_t)i, pos);
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

    // Each cheat gets its own guard so one fault can't stop the others.
    void GuardedNoclip(void** state, const Players& p, const Games::Settings& s, const Games::FrameInput& in,
                       float dt) {
        __try {
            Noclip(state, p, s.noclip, in, dt);
        } __except (Log::Exception(GetExceptionInformation(), "noclip")) {
        }
    }

    void GuardedDamage(const Players& p, Blam::DataArray* objects, float m) {
        __try {
            Damage(p, objects, m);
        } __except (Log::Exception(GetExceptionInformation(), "damage multiplier")) {
        }
    }

    // Save and load checkpoints when their buttons were pressed.
    void GuardedCheckpoints(const Games::Settings& s) {
        __try {
            if (s.saveCheckpointSerial != g_saveSerial) {
                g_saveSerial = s.saveCheckpointSerial;
                LOG("Halo 3: saving a checkpoint");
                Halo3Checkpoints::BeginOwnSave();
                Engine<void(__fastcall*)(int32_t)>(OFFSET_HALO3_GAME_SAVE)(HALO3_GAME_SAVE_IMMEDIATE);
                Halo3Checkpoints::EndOwnSave();
            }
            if (s.loadCheckpointSerial != g_loadSerial) {
                g_loadSerial = s.loadCheckpointSerial;
                LOG("Halo 3: loading the last checkpoint");
                *(uint16_t*)(g_module + OFFSET_HALO3_GAME_REVERT_FLAG) = HALO3_GAME_REVERT_REQUEST;
            }
        } __except (Log::Exception(GetExceptionInformation(), "Halo 3 checkpoints")) {
        }
    }

    void GuardedGive(const Players& p, Blam::DataArray* objects, uint32_t tag, bool request) {
        __try {
            if (request) GiveWeapon(p, objects, tag);
            ContinueGive(p);
        } __except (Log::Exception(GetExceptionInformation(), "give weapon")) {
            g_give = {};
        }
    }

    void GuardedAimPull(void** state, const Players& p, Blam::DataArray* objects, const Games::Settings& s,
                        bool firing) {
        __try {
            if (s.aimbot && firing) AimPull(state, p, objects, s.aimbotStrength);
        } __except (Log::Exception(GetExceptionInformation(), "aimbot strength")) {
        }
    }

    void GuardedEquipment(void** state, const Players& p, Blam::DataArray* objects, const Games::Settings& s) {
        __try {
            Equipment(state, p, objects, s);
        } __except (Log::Exception(GetExceptionInformation(), "equipment")) {
        }
    }

    void GuardedWeaponMods(const Players& p, Blam::DataArray* objects, const Games::Settings& s) {
        __try {
            WeaponMods(p, objects, s);
        } __except (Log::Exception(GetExceptionInformation(), "weapon mods")) {
        }
    }

    bool ReadPlayers(void** state, Blam::DataArray*& objects, Players& p) {
        __try {
            auto players = (Blam::DataArray*)state[HALO3_TLS_SLOT_PLAYERS];
            objects = (Blam::DataArray*)state[HALO3_TLS_SLOT_OBJECTS];
            if (!players || !objects || !players->data || !objects->data) return false;
            p = FindPlayers(players, objects);
            ReadAllies(p, (char*)state[HALO3_TLS_SLOT_GAME_GLOBALS], (char*)state[HALO3_TLS_SLOT_ALLEGIANCE]);
            return true;
        } __except (Log::Exception(GetExceptionInformation(), "main-thread players")) {
            return false;
        }
    }

    void RunAll(void** state, const Games::Settings& s, const Games::FrameInput& in) {
        Blam::DataArray* objects = nullptr;
        Players p;
        if (!ReadPlayers(state, objects, p)) return;

        // Noclip runs here in the frame now (set position doesn't touch aim,
        // so the frame timing that broke aiming with the teleport is fine).
        GuardedNoclip(state, p, s, in, TickSeconds(g_lastInputTick));
        if (g_noclipFinishPending) {
            g_noclipFinishPending = false;
            if (p.count > 0) {
                char* unit = p.units[0];
                float pos[3];
                memcpy(pos, &Field<float>(unit, HALO3_OBJECT_POSITION), sizeof(pos));
                SetPosition(unit, p.datums[0], pos);
                LOG("Halo 3: noclip finished at %.2f %.2f %.2f", pos[0], pos[1], pos[2]);
            }
        }
        GuardedDamage(p, objects, s.damage);
        GuardedWeaponMods(p, objects, s);
        GuardedAimPull(state, p, objects, s, in.firing);
        GuardedEquipment(state, p, objects, s);
        GuardedCheckpoints(s);
        bool request = s.giveWeaponSerial != g_giveSerial;
        if (request) {
            g_giveSerial = s.giveWeaponSerial;
            LOG("Halo 3: give weapon request %08X (%d player units)", s.giveWeapon, p.count);
        }
        GuardedGive(p, objects, s.giveWeapon, request);
        Halo3Spawn::Tick(state, p, objects, s);
        Halo3Spawn::FunTick(state, p, objects, s, TickSeconds(g_lastFunTick));
        Halo3Teams::Tick(state, s, p, objects);
        Halo3Extras::Tick(state, s, p, objects);
        Halo3Checkpoints::Tick(state, s, p, objects);
        Halo3Fun::Tick(state, s, in, p, objects);
        Halo3Bombing::Tick(state, s, p, objects);
        Halo3Equipment::Tick(state, s, p, objects);
        Halo3Upgrade::Tick(state, s, p, objects);
        Halo3Vehicles::Tick(state, s, in, p, objects);
    }
}

namespace Halo3Cheats {
    void SetMainThreadModule(HMODULE halo3) { g_module = (char*)halo3; }

    void MoveNoclipAnchor(const float* position) {
        if (g_noclipActive) memcpy(g_anchor, position, sizeof(g_anchor));
    }

    void ResetMainThread() {
        g_noclipActive = false;
        g_noclipUnit = 0xFFFFFFFF;
        g_lastTick = {};
        g_lastInputTick = {};
        g_noclipFinishPending = false;
        g_give = {};  // its objects went with the map
        memset(g_tracked, 0, sizeof(g_tracked));
        memset(g_ownEquipment, 0xFF, sizeof(g_ownEquipment));
    }
}

void Halo3Cheats::MainThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input) {
    if (!g_module || !gameState) return;
    // Don't run any "Give" that was pressed before this map loaded.
    static bool s_first = true;
    if (s_first) {
        g_giveSerial = settings.giveWeaponSerial;
        g_saveSerial = settings.saveCheckpointSerial;
        g_loadSerial = settings.loadCheckpointSerial;
        s_first = false;
    }
    RunAll(gameState, settings, input);
}

void Halo3Cheats::InputThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input) {
    // Nothing runs from MCC's input polling any more (noclip moved to the frame).
    (void)gameState;
    (void)settings;
    (void)input;
}
