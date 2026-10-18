#include "cheats.h"
#include "games/squads.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "cheat_util.h"
#include "core/log.h"
#include "core/patch.h"
#include "tags.h"

namespace ReachCheats {
    // main_thread.cpp
    void SetMainThreadModule(HMODULE reach);
    void ResetMainThread();
    void SpawnStatus(Games::Status& status);
}

using namespace ReachCheats;

namespace {
    constexpr int kMaxSoftCeilings = 128;
    constexpr int kMaxAimbotTags = 16;
    constexpr int kMaxAmmoBarrels = 16;
    constexpr int kMaxRecoilEffects = 64;
    constexpr int kMaxDeafActors = 512;
    constexpr float kPi = 3.14159265f;

    // --- State we've changed ---------------------------------------------
    // A flag we set on player units. `units` remembers which, so a unit that
    // stops being a player's (co-op turned off: the second player's body stays
    // in the world) gets it cleared instead of keeping it for good.
    struct FlagState {
        bool applied = false;
        uint32_t units[8] = {};
        int next = 0;
    };
    FlagState g_godApplied;
    FlagState g_blindApplied;
    Restorable<float> g_speed[10];                 // matg player information speeds
    Restorable<float> g_jump[kMaxLocalPlayers];    // per player biped tag
    constexpr Games::BlockField kVehicleSpeedFields[] = {
        {REACH_VEHI_TANK, 0x70, 0x3C}, {REACH_VEHI_JEEP, 0x58, 0x18}, {REACH_VEHI_WOLVERINE, 0x60, 0x18},
        {REACH_VEHI_PLANE, 0x4C, 0x0}, {REACH_VEHI_PLANE, 0x4C, 0x4}, {REACH_VEHI_PLANE, 0x4C, 0x8},
        {REACH_VEHI_SCOUT, 0x74, 0x8}, {REACH_VEHI_SCOUT, 0x74, 0xC}, {REACH_VEHI_SCOUT, 0x74, 0x10},
        {REACH_VEHI_SCOUT, 0x74, 0x4C}, {REACH_VEHI_SCOUT, 0x74, 0x60},
        {REACH_VEHI_FIGHTER, 0x64, 0x14}, {REACH_VEHI_FIGHTER, 0x64, 0x18}, {REACH_VEHI_FIGHTER, 0x64, 0x1C},
        {REACH_VEHI_MANTIS, 0xAC, 0x14}, {REACH_VEHI_MANTIS, 0xAC, 0x18}, {REACH_VEHI_MANTIS, 0xAC, 0x1C},
        {REACH_VEHI_CHOPPER, 0x7C, 0x18},
        {REACH_VEHI_GUARDIAN, 0x30, 0x8}, {REACH_VEHI_GUARDIAN, 0x30, 0xC}, {REACH_VEHI_GUARDIAN, 0x30, 0x10},
        {REACH_VEHI_BOAT, 0x60, 0x24},
        {REACH_VEHI_SPACE_FIGHTER, 0xC4, 0x14}, {REACH_VEHI_SPACE_FIGHTER, 0xC4, 0x18},
        {REACH_VEHI_SPACE_FIGHTER, 0xC4, 0x1C}, {REACH_VEHI_SPACE_FIGHTER, 0xC4, 0x20},
        {REACH_VEHI_REVENANT, 0xA8, 0x3C}, {REACH_VEHI_REVENANT, 0xA8, 0x70}, {REACH_VEHI_REVENANT, 0xA8, 0x74},
        {REACH_VEHI_REVENANT, 0xA8, 0x78},
    };
    Games::VehicleScaler<_countof(kVehicleSpeedFields)> g_vehicleSpeed;
    Games::VehicleGodState g_vehicleGod;
    Games::VehicleGodState g_vehicleNoEmp;
    Games::VehicleGodState g_vehicleReserved;  // cannot be hijacked: AI reservation
    // Infinite boost: boost burns no power.
    constexpr Games::TagValue kInfiniteBoost[] = {{REACH_VEHI_BOOST_POWER_PER_SECOND, 0.0f}};
    Games::VehicleSetter<_countof(kInfiniteBoost)> g_infiniteBoost;
    // Cannot be hijacked: the seats of the vehicle player 1 is in (and of
    // what it's mounted on) are locked while they're in it.
    constexpr int kMaxLockedSeats = 32;
    Restorable<uint32_t> g_lockedSeats[kMaxLockedSeats];
    Restorable<float> g_gravity;
    Restorable<float> g_gameSpeed;
    Restorable<int32_t> g_killTriggerCount;
    Restorable<uint16_t> g_softCeilingFlags[kMaxSoftCeilings][2];
    Restorable<uint8_t> g_aiGrenades;
    Restorable<uint8_t> g_hud[REACH_CHUD_SHOW_COUNT];
    Restorable<uint8_t> g_deaf[kMaxDeafActors];

    struct AimbotTag {
        char* tag = nullptr;
        Restorable<float> fields[4];  // angle, range, falloff, deviation
    };
    AimbotTag g_aimbot[kMaxAimbotTags];

    Restorable<float> g_selfDestruct[16];  // equipment tags' self destruct times (infinite equipment)
    Restorable<uint32_t> g_ammo[kMaxAmmoBarrels];          // barrel projectile datums
    Restorable<uint32_t> g_recoil[kMaxRecoilEffects];      // firing effect damage datums
    Restorable<uint32_t> g_vehicleRecoil[kMaxRecoilEffects];  // the same, and rider responses, on vehicle guns
    Restorable<float> g_vehiclePush[kMaxRecoilEffects];       // firing effect accelerations on vehicle guns

    ::Patch::Nop g_aiInvisible1;
    ::Patch::Nop g_aiInvisible2;
    ::Patch::Nop g_noHeat;
    ::Patch::Nop g_thirdPerson;
    ::Patch::Nop g_explosionPush;       // the push factor's load, pointed at g_explosionCell
    float* g_explosionCell = nullptr;   // (in memory near the module) 5.0 * the slider
    std::atomic<float> g_explosionScale{1.0f};
    char* g_moduleBase = nullptr;
    Restorable<float> g_fovFirst, g_fovThird;

    // Read by the barrel-fire hook on the game thread.
    volatile bool g_aiCantShoot = false;
    volatile bool g_timeStop = false;  // time stop: nothing but the players' weapons fires
    ReachCheats::FriendlyFireStats g_ffStats;
    volatile bool g_noFriendlyFire = false;  // read by the cause-damage hook (game thread)
    volatile bool g_noEject = false;  // read by the seat-exit hook (game thread)

    // Pickable tags, rebuilt when the map changes.
    char* g_listsScenario = nullptr;
    std::vector<Games::TagChoice> g_weapons;
    std::vector<Games::TagChoice> g_equipment;
    std::vector<Games::CheckpointEntry> g_checkpointStatus;  // what the menu shows, refreshed each frame
    std::vector<Games::TagChoice> g_projectiles;
    std::vector<Games::TagChoice> g_vehicles;
    std::vector<Games::TagChoice> g_objects;  // equipment, crates, machines, scenery
    std::mutex g_vehicleChoicesLock;  // read by the game thread
    std::vector<VehicleChoice> g_vehicleChoices;

    // Seats only AI may use (a Pelican's or Phantom's pilot, gunner and
    // passenger seats the game keeps from players), on every vehicle tag in
    // the map, found with the lists; "Pilot AI-only vehicles" clears their flag.
    struct AiOnlySeat {
        uint32_t vehicle;
        uint32_t seatName;
        uint32_t* flags;
        bool driver;  // the pilot's seat: take pilot seat and the hull exit use these
        Restorable<uint32_t> edit;
    };
    std::vector<AiOnlySeat> g_aiOnlySeats;  // guarded by g_vehicleChoicesLock
    std::vector<Games::TagChoice> g_characters;  // AI that can be spawned

    // The same lists as datums, for the game thread (chaos gun, rain).
    struct Pools {
        std::vector<uint32_t> projectiles, vehicles, objects, ai, weapons;
    };
    std::mutex g_poolsLock;
    Pools g_pools;
    uint32_t g_random = 0x9E3779B9u;  // xorshift state, under g_poolsLock

    // Read by the barrel-fire hook (game thread).
    std::atomic<bool> g_chaosGun{false};
    std::atomic<float> g_recoilPush{0.0f};  // world units / s per shot, 0 = off

    // Skulls page, by bit (the script "skull" enum). Reach names two
    // internally only (Assassin, Superman); one is likely Cowbell.
    const char* const kSkullNames[REACH_SKULL_COUNT] = {
        "Iron", "Black Eye", "Tough Luck", "Catch", "Cloud", "Famine", "Thunderstorm", "Tilt", "Mythic",
        "Assassin (internal name)", "Blind", "Superman (internal name)", "Grunt Birthday Party", "IWHBYD",
        "Red", "Yellow", "Blue"};

    struct Frame {
        Array players;
        Array objects;
        Array actors;
        char* gameGlobals;
        char* gameTime;
        char* physics;
        char* aiGlobals;
        char* chud;
        char* matg;
        char* scnr;
        char* allegiance;
    };

    // --- Cheats -------------------------------------------------------------

    // Allies never turn on the player: hits on them add to the player/human
    // betrayal record's score and at its threshold the allegiance breaks. Keep
    // the score at 0 and the threshold out of reach, and put the friendship
    // back if it already broke. Everything is restored when switched off.
    void NoBetrayal(char* table, bool on) {
        static bool applied = false;
        static int16_t saved[16];
        if (!table || (!on && !applied)) return;
        int count = *(uint16_t*)table;
        if (count > 16) count = 16;
        for (int i = 0; i < count; ++i) {
            char* record = table + i * REACH_BETRAYAL_RECORD_SIZE;
            int a = Field<int8_t>(record, REACH_BETRAYAL_TEAM_A);
            int b = Field<int8_t>(record, REACH_BETRAYAL_TEAM_B);
            if (!((a == REACH_TEAM_PLAYER && b == REACH_TEAM_HUMAN) || (a == REACH_TEAM_HUMAN && b == REACH_TEAM_PLAYER)))
                continue;
            int16_t& threshold = Field<int16_t>(record, REACH_BETRAYAL_THRESHOLD);
            if (on) {
                if (threshold != 0x7FFF) saved[i] = threshold;
                threshold = 0x7FFF;
                Field<uint16_t>(record, REACH_BETRAYAL_SCORE) = 0;
            } else if (threshold == 0x7FFF) {
                threshold = saved[i];
            }
        }
        if (on) {
            for (int team : {REACH_TEAM_PLAYER, REACH_TEAM_HUMAN})
                Field<uint32_t>(table, REACH_ALLEGIANCE_MASKS + 4 * team) |= (1u << REACH_TEAM_PLAYER) | (1u << REACH_TEAM_HUMAN);
        }
        applied = on;
    }

    // Per-unit flag: set, or cleared once when switched off. Units we flagged
    // that no longer belong to a player are cleared too.
    void UnitFlag(const Players& p, const Array& objects, int offset, uint32_t mask, bool on, FlagState& state) {
        for (uint32_t& d : state.units) {
            if (!d) continue;
            bool current = false;
            for (int i = 0; i < p.count; ++i) current |= p.datums[i] == d;
            if (current) continue;
            if (char* unit = ObjectFromDatum(objects, d)) Field<uint32_t>(unit, offset) &= ~mask;
            d = 0;
        }
        if (!on && !state.applied) return;
        for (int i = 0; i < p.count; ++i) {
            auto& flags = Field<uint32_t>(p.units[i], offset);
            if (on) flags |= mask;
            else flags &= ~mask;
            bool known = false;
            for (uint32_t d : state.units) known |= d == p.datums[i];
            if (on && !known) {
                state.units[state.next] = p.datums[i];
                state.next = (state.next + 1) % 8;
            }
        }
        if (!on)
            for (uint32_t& d : state.units) d = 0;
        state.applied = on;
    }

    // Tops up the player's weapons from their weapon tag's magazine sizes.
    void Ammo(const Players& p, const Array& objects, bool infinite, bool bottomless) {
        if (!infinite && !bottomless) return;

        for (int i = 0; i < p.count; ++i) {
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(p.units[i], objects, w);
                if (!weapon) continue;
                char* tag = ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM));
                int magazines = ReachTags::BlockCount(tag, REACH_WEAP_MAGAZINES);

                // Energy weapons' battery is the weapon's age.
                Field<float>(weapon, REACH_WEAPON_AGE) = 0.0f;

                for (int m = 0; m < magazines && m < 2; ++m) {
                    char* def = ReachTags::BlockElement(tag, REACH_WEAP_MAGAZINES, REACH_WEAP_MAGAZINE_DEF_SIZE, m);
                    if (!def) continue;
                    char* mag = weapon + REACH_WEAPON_MAGAZINES + m * REACH_WEAPON_MAGAZINE_SIZE;

                    int16_t loadedMax = Field<int16_t>(def, REACH_MAGDEF_ROUNDS_LOADED_MAX);
                    int16_t totalMax = Field<int16_t>(def, REACH_MAGDEF_ROUNDS_TOTAL_MAX);
                    auto& loaded = Field<int16_t>(mag, REACH_MAGAZINE_LOADED);
                    auto& reserve = Field<int16_t>(mag, REACH_MAGAZINE_RESERVE);

                    if (bottomless && loaded < loadedMax) loaded = loadedMax;
                    int16_t reserveMax = totalMax > loadedMax ? totalMax - loadedMax : totalMax;
                    if (infinite && reserve < reserveMax) reserve = reserveMax;
                }
            }
        }
    }

    void Grenades(const Players& p, char* matg, bool on) {
        if (!on || !matg) return;
        int types = ReachTags::BlockCount(matg, REACH_MATG_GRENADES);
        for (int t = 0; t < types && t < REACH_UNIT_GRENADE_TYPES; ++t) {
            char* def = ReachTags::BlockElement(matg, REACH_MATG_GRENADES, REACH_MATG_GRENADE_DEF_SIZE, t);
            if (!def) continue;
            int16_t max = Field<int16_t>(def, 0);
            for (int i = 0; i < p.count; ++i) {
                auto& count = Field<int8_t>(p.units[i], REACH_UNIT_GRENADE_COUNTS + t);
                if (max > 0 && count < max) count = (int8_t)max;
            }
        }
    }

    // Leaves enemy AI with no shield and a sliver of health, so any hit kills.
    void OneHitKill(const Players& p, const Array& objects, bool on) {
        if (!on) return;
        for (int i = 0; i < objects.HighWater(); ++i) {
            char* entry = objects.Entry(i);
            int type = Field<uint8_t>(entry, REACH_OBJECT_ENTRY_TYPE);
            char* object = Field<char*>(entry, REACH_OBJECT_ENTRY_OBJECT);
            if (*(uint16_t*)entry == 0 || !object || !IsKillableEnemy(object, type, p)) continue;

            float* health = Health(object);
            if (!health || *health <= 0.0f) continue;  // dying
            if (float* shield = Shield(object)) *shield = 0.0f;
            if (*health > 0.01f) *health = 0.01f;
        }
    }

    void Speed(char* matg, float m) {
        char* info = ReachTags::BlockElement(matg, REACH_MATG_PLAYER_INFORMATION, REACH_MATG_PLAYER_INFO_SIZE, 0);
        if (!info) return;
        // Walk, run forward/back/side, run accel, sneak forward/back/side, sneak accel, air accel.
        for (int f = 0; f < 10; ++f)
            Scale(g_speed[f], (float*)(info + f * 4), m);
    }

    // Vehicle god mode: the damage flag (all god mode uses in Reach).
    void VehicleGod(const Players& p, const Array& objects, bool on) {
        uint32_t seats[kMaxLocalPlayers];
        for (int i = 0; i < p.count; ++i) seats[i] = Field<uint32_t>(p.units[i], REACH_OBJECT_PARENT);
        g_vehicleGod.Update(
            seats, p.count, on, [&](uint32_t d) { return ObjectOfType(objects, d, REACH_OBJECT_TYPE_VEHICLE); },
            [](char* o) { return Field<uint32_t>(o, REACH_OBJECT_PARENT); },
            [](char* o) { return (Field<uint32_t>(o, REACH_OBJECT_DAMAGE_FLAGS) & REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE) != 0; },
            [](char* o, bool god) {
                auto& flags = Field<uint32_t>(o, REACH_OBJECT_DAMAGE_FLAGS);
                flags = god ? (uint32_t)(flags | REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE)
                            : (uint32_t)(flags & ~REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE);
            });
    }

    // Cannot be shut down: the "ignores EMP" damage flag on the vehicles the
    // players are in, and any stun already running is cleared.
    void VehicleNoEmp(const Players& p, const Array& objects, bool on) {
        uint32_t seats[kMaxLocalPlayers];
        for (int i = 0; i < p.count; ++i) seats[i] = Field<uint32_t>(p.units[i], REACH_OBJECT_PARENT);
        g_vehicleNoEmp.Update(
            seats, p.count, on, [&](uint32_t d) { return ObjectOfType(objects, d, REACH_OBJECT_TYPE_VEHICLE); },
            [](char* o) { return Field<uint32_t>(o, REACH_OBJECT_PARENT); },
            [](char* o) { return (Field<uint32_t>(o, REACH_OBJECT_DAMAGE_FLAGS) & REACH_DAMAGE_FLAG_IGNORES_EMP) != 0; },
            [](char* o, bool on) {
                auto& flags = Field<uint32_t>(o, REACH_OBJECT_DAMAGE_FLAGS);
                flags = on ? (flags | REACH_DAMAGE_FLAG_IGNORES_EMP) : (flags & ~REACH_DAMAGE_FLAG_IGNORES_EMP);
                if (on && Field<int16_t>(o, REACH_UNIT_EMP_STUN) > 0) Field<int16_t>(o, REACH_UNIT_EMP_STUN) = 0;
            });
    }

    // Cannot be hijacked: every seat of the vehicle player 1 is in (and of
    // what it's mounted on) is set "Locked" and loses "Boarding Seat" (a tag
    // edit, so same-type vehicles too, only while you're in one), and the
    // vehicles are reserved so AI won't pick them. Locked alone didn't stop
    // an Elite boarding (tested 2026-10-02). Put back when they get out.
    // True while the unit sits in a boarding seat, i.e. is hijacking. Reads
    // the seat's own flags, or what they were before we cleared them.
    bool Boarding(char* unit, const Array& objects) {
        char* vehicle = VehicleOf(unit, objects);
        char* tag = vehicle ? ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM)) : nullptr;
        char* seat = ReachTags::BlockElement(tag, REACH_VEHI_SEATS, REACH_VEHI_SEAT_SIZE,
                                             Field<int16_t>(unit, REACH_UNIT_SEAT));
        if (!seat) return false;
        uint32_t* at = &Field<uint32_t>(seat, 0);
        uint32_t flags = *at;
        for (auto& r : g_lockedSeats)
            if (r.Ours(at)) flags = r.original;
        return (flags & REACH_SEAT_FLAG_BOARDING_SEAT) != 0;
    }

    // Not while a player is boarding a vehicle themselves: locking the seat
    // they board through left them stuck mid-hijack (tested 2026-10-02).
    void VehicleNoHijack(const Players& p, const Array& objects, bool on) {
        uint32_t seatsOf[kMaxLocalPlayers];
        for (int i = 0; i < p.count; ++i)
            seatsOf[i] = Boarding(p.units[i], objects) ? 0xFFFFFFFF : Field<uint32_t>(p.units[i], REACH_OBJECT_PARENT);
        g_vehicleReserved.Update(
            seatsOf, p.count, on, [&](uint32_t d) { return ObjectOfType(objects, d, REACH_OBJECT_TYPE_VEHICLE); },
            [](char* o) { return Field<uint32_t>(o, REACH_OBJECT_PARENT); },
            [](char* o) { return Field<uint64_t>(o, REACH_VEHICLE_AI_RESERVED) == ~0ull; },
            [](char* o, bool reserve) { Field<uint64_t>(o, REACH_VEHICLE_AI_RESERVED) = reserve ? ~0ull : 0; });

        uint32_t* wanted[kMaxLockedSeats] = {};
        int count = 0;
        char* vehicle = on && p.count > 0 && !Boarding(p.units[0], objects) ? VehicleOf(p.units[0], objects) : nullptr;
        for (int depth = 0; vehicle && depth < 4; ++depth) {
            char* tag = ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM));
            int seats = ReachTags::BlockCount(tag, REACH_VEHI_SEATS);
            for (int i = 0; i < seats && count < kMaxLockedSeats; ++i)
                if (char* seat = ReachTags::BlockElement(tag, REACH_VEHI_SEATS, REACH_VEHI_SEAT_SIZE, i))
                    wanted[count++] = &Field<uint32_t>(seat, 0);
            vehicle = VehicleOf(vehicle, objects);
        }
        for (auto& r : g_lockedSeats) {
            if (!r.addr) continue;
            bool keep = false;
            for (int i = 0; i < count; ++i) keep |= wanted[i] == r.addr;
            if (!keep) r.Restore();
        }
        for (int i = 0; i < count; ++i) {
            Restorable<uint32_t>* slot = nullptr;
            for (auto& r : g_lockedSeats)
                if (r.addr == wanted[i]) { slot = &r; break; }
            for (auto& r : g_lockedSeats)
                if (!slot && !r.addr) slot = &r;
            if (!slot) continue;
            uint32_t base = slot->Ours(wanted[i]) ? slot->original : *wanted[i];
            slot->Set(wanted[i], (base | REACH_SEAT_FLAG_LOCKED) & ~REACH_SEAT_FLAG_BOARDING_SEAT);
            slot->original = base;
        }
    }

    // Vehicle speed: the tag of the vehicle player 1 is in (same-type vehicles too).
    void VehicleSpeed(const Players& p, const Array& objects, float m) {
        char* vehicle = p.count > 0 ? VehicleOf(p.units[0], objects) : nullptr;
        char* tag = vehicle ? ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM)) : nullptr;
        g_vehicleSpeed.Update(tag, nullptr, 0, kVehicleSpeedFields, _countof(kVehicleSpeedFields), m,
                              [](char* t, int block, int size) { return ReachTags::BlockElement(t, block, size, 0); });
    }

    void Jump(const Players& p, float m) {
        for (int i = 0; i < kMaxLocalPlayers; ++i) {
            char* tag = i < p.count ? ReachTags::Get(Field<uint32_t>(p.units[i], REACH_OBJECT_TAG_DATUM)) : nullptr;
            if (tag) Scale(g_jump[i], &Field<float>(tag, REACH_BIPD_JUMP_VELOCITY), m);
        }
    }

    void NoBoundaries(char* scnr, bool on) {
        if (!scnr) return;

        auto* killCount = &Field<int32_t>(scnr, REACH_SCNR_KILL_TRIGGERS);
        if (on) g_killTriggerCount.Set(killCount, 0);
        else g_killTriggerCount.Restore();

        int ceilings = ReachTags::BlockCount(scnr, REACH_SCNR_SOFT_CEILINGS);
        for (int i = 0; i < ceilings && i < kMaxSoftCeilings; ++i) {
            char* ceiling = ReachTags::BlockElement(scnr, REACH_SCNR_SOFT_CEILINGS, REACH_SCNR_SOFT_CEILING_SIZE, i);
            if (!ceiling) continue;
            for (int f = 0; f < 2; ++f) {  // flags, runtime flags
                auto* flags = &Field<uint16_t>(ceiling, f * 2);
                Restorable<uint16_t>& patch = g_softCeilingFlags[i][f];
                if (on) {
                    uint16_t base = patch.Ours(flags) ? patch.original : *flags;
                    patch.Set(flags, base | REACH_SOFT_CEILING_IGNORE_ALL);
                    patch.original = base;
                } else if (patch.addr) {
                    patch.Restore();
                }
            }
        }
    }

    // Maxes out the built-in autoaim on the player's weapons, so shots bend
    // onto any enemy in view.
    void Aimbot(const Players& p, const Array& objects, bool on) {
        if (!on) {
            for (auto& a : g_aimbot) {
                for (auto& f : a.fields) f.Restore();
                a.tag = nullptr;
            }
            return;
        }

        for (int i = 0; i < p.count; ++i) {
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(p.units[i], objects, w);
                char* tag = weapon ? ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM)) : nullptr;
                if (!tag || IsMeleeOnly(weapon)) continue;  // melee weapons keep their own aim

                AimbotTag* slot = nullptr;
                for (auto& a : g_aimbot)
                    if (a.tag == tag) { slot = &a; break; }
                for (auto& a : g_aimbot)
                    if (!slot && !a.tag) { slot = &a; slot->tag = tag; }
                if (!slot) continue;

                slot->fields[0].Set(&Field<float>(tag, REACH_WEAP_AUTOAIM_ANGLE), kPi);
                slot->fields[1].Set(&Field<float>(tag, REACH_WEAP_AUTOAIM_RANGE), 1000.0f);
                slot->fields[2].Set(&Field<float>(tag, REACH_WEAP_AUTOAIM_FALLOFF), 1000.0f);
                slot->fields[3].Set(&Field<float>(tag, REACH_WEAP_DEVIATION_ANGLE), kPi);
            }
        }
    }

    bool InList(const std::vector<Games::TagChoice>& list, uint32_t datum) {
        for (const auto& c : list)
            if (c.datum == datum) return true;
        return false;
    }

    // Sets each wanted field (tag datums, floats), and restores the ones no longer wanted.
    template <typename T, size_t N>
    void SetValues(Restorable<T> (&patches)[N], T* const* wanted, int wantedCount, T value) {
        for (auto& patch : patches) {
            if (!patch.addr) continue;
            bool keep = false;
            for (int i = 0; i < wantedCount; ++i) keep |= wanted[i] == patch.addr;
            if (!keep) patch.Restore();
        }
        for (int i = 0; i < wantedCount; ++i) {
            Restorable<T>* slot = nullptr;
            for (auto& patch : patches)
                if (patch.addr == wanted[i]) { slot = &patch; break; }
            for (auto& patch : patches)
                if (!slot && !patch.addr) slot = &patch;
            if (slot) slot->Set(wanted[i], value);
        }
    }

    // Points player 1's held weapon's barrels at the chosen projectile. Other
    // weapons go back to normal.
    void AmmoType(const Players& p, const Array& objects, uint32_t projectile) {
        uint32_t* wanted[kMaxAmmoBarrels] = {};
        int wantedCount = 0;

        char* weapon = p.count > 0 && projectile != 0xFFFFFFFF && InList(g_projectiles, projectile)
                           ? HeldWeapon(p.units[0], objects) : nullptr;
        char* tag = weapon ? ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM)) : nullptr;
        int barrels = ReachTags::BlockCount(tag, REACH_WEAP_BARRELS);
        for (int b = 0; b < barrels && wantedCount < kMaxAmmoBarrels; ++b) {
            char* barrel = ReachTags::BlockElement(tag, REACH_WEAP_BARRELS, REACH_WEAP_BARREL_SIZE, b);
            if (barrel) wanted[wantedCount++] = &Field<uint32_t>(barrel, REACH_BARREL_PROJECTILE + REACH_TAGREF_DATUM);
        }
        SetValues(g_ammo, wanted, wantedCount, projectile);
    }

    // No recoil: Reach's view kick comes from each barrel's firing damage
    // effect. Clears it on the players' weapons; other weapons go back.
    void NoRecoil(const Players& p, const Array& objects, bool on) {
        uint32_t* wanted[kMaxRecoilEffects] = {};
        int wantedCount = 0;
        for (int i = 0; on && i < p.count; ++i) {
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(p.units[i], objects, w);
                char* tag = weapon ? ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM)) : nullptr;
                int barrels = ReachTags::BlockCount(tag, REACH_WEAP_BARRELS);
                for (int b = 0; b < barrels; ++b) {
                    char* barrel = ReachTags::BlockElement(tag, REACH_WEAP_BARRELS, REACH_WEAP_BARREL_SIZE, b);
                    int effects = ReachTags::BlockCount(barrel, REACH_BARREL_FIRING_EFFECTS);
                    for (int e = 0; e < effects && wantedCount < kMaxRecoilEffects; ++e) {
                        char* effect = ReachTags::BlockElement(barrel, REACH_BARREL_FIRING_EFFECTS,
                                                               REACH_FIRING_EFFECT_SIZE, e);
                        if (effect)
                            wanted[wantedCount++] = &Field<uint32_t>(effect, REACH_FIRING_EFFECT_DAMAGE + REACH_TAGREF_DATUM);
                    }
                }
            }
        }
        SetValues(g_recoil, wanted, wantedCount, 0xFFFFFFFF);
    }

    // The acceleration fields of an effect tag's events, appended to `out`.
    void EffectAccelerations(uint32_t effect, float** out, int& count) {
        char* tag = ReachTags::Get(effect);
        int events = ReachTags::BlockCount(tag, REACH_EFFE_EVENTS);
        for (int e = 0; e < events; ++e) {
            char* event = ReachTags::BlockElement(tag, REACH_EFFE_EVENTS, REACH_EFFE_EVENT_SIZE, e);
            int accelerations = ReachTags::BlockCount(event, REACH_EFFE_EVENT_ACCELERATIONS);
            for (int a = 0; a < accelerations && count < kMaxRecoilEffects; ++a) {
                char* acceleration = ReachTags::BlockElement(event, REACH_EFFE_EVENT_ACCELERATIONS,
                                                             REACH_EFFE_ACCELERATION_SIZE, a);
                if (acceleration) out[count++] = &Field<float>(acceleration, REACH_EFFE_ACCELERATION);
            }
        }
    }

    // No vehicle recoil: the same for the guns of the vehicle (or turret)
    // each player is in, plus the rider responses on those firing effects
    // (the camera jolt) and the firing effects' accelerations (the push back).
    void VehicleNoRecoil(const Players& p, const Array& objects, bool on) {
        constexpr int kFields[] = {REACH_FIRING_EFFECT_DAMAGE, REACH_FIRING_EFFECT_SECONDARY_DAMAGE,
                                   REACH_FIRING_EFFECT_RIDER_RESPONSE, REACH_FIRING_EFFECT_SECONDARY_RIDER_DAMAGE};
        uint32_t* wanted[kMaxRecoilEffects] = {};
        int wantedCount = 0;
        float* pushes[kMaxRecoilEffects] = {};
        int pushCount = 0;
        for (int i = 0; on && i < p.count; ++i) {
            char* parts[16];
            int partCount = VehicleParts(p.units[i], objects, parts, _countof(parts));
            for (int k = 0; k < partCount; ++k) {
                for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                    char* weapon = WeaponInSlot(parts[k], objects, w);
                    char* tag = weapon ? ReachTags::Get(Field<uint32_t>(weapon, REACH_OBJECT_TAG_DATUM)) : nullptr;
                    int barrels = ReachTags::BlockCount(tag, REACH_WEAP_BARRELS);
                    for (int b = 0; b < barrels; ++b) {
                        char* barrel = ReachTags::BlockElement(tag, REACH_WEAP_BARRELS, REACH_WEAP_BARREL_SIZE, b);
                        int effects = ReachTags::BlockCount(barrel, REACH_BARREL_FIRING_EFFECTS);
                        for (int e = 0; e < effects; ++e) {
                            char* effect = ReachTags::BlockElement(barrel, REACH_BARREL_FIRING_EFFECTS,
                                                                   REACH_FIRING_EFFECT_SIZE, e);
                            if (!effect) continue;
                            for (int field : kFields)
                                if (wantedCount < kMaxRecoilEffects)
                                    wanted[wantedCount++] = &Field<uint32_t>(effect, field + REACH_TAGREF_DATUM);
                            for (int field : {REACH_FIRING_EFFECT_EFFECT, REACH_FIRING_EFFECT_SECONDARY_EFFECT})
                                EffectAccelerations(Field<uint32_t>(effect, field + REACH_TAGREF_DATUM), pushes, pushCount);
                        }
                    }
                }
            }
        }
        SetValues(g_vehicleRecoil, wanted, wantedCount, 0xFFFFFFFF);
        SetValues(g_vehiclePush, pushes, pushCount, 0.0f);
    }

    // AI can't shoot: the barrel-fire hook (see WeaponMayFire) plus the
    // global "AI throw grenades" switch.
    void AiCantShoot(char* aiGlobals, bool on) {
        g_aiCantShoot = on;
        if (!aiGlobals) return;
        auto* grenades = &Field<uint8_t>(aiGlobals, REACH_AI_GLOBALS_GRENADES);
        if (on) g_aiGrenades.Set(grenades, 0);
        else g_aiGrenades.Restore();
    }

    // AI can't hear: the actors' own "deaf" flag (what ai_set_deaf sets).
    void AiDeaf(const Array& actors, bool on) {
        int count = on && actors.Valid() ? actors.HighWater() : 0;
        for (int i = 0; i < kMaxDeafActors; ++i) {
            char* actor = i < count ? actors.Entry(i) : nullptr;
            if (actor && *(uint16_t*)actor != 0) g_deaf[i].Set(&Field<uint8_t>(actor, REACH_ACTOR_DEAF), 1);
            else g_deaf[i].Restore();
        }
    }

    void NoHud(char* chud, bool on) {
        for (int i = 0; i < REACH_CHUD_SHOW_COUNT; ++i) {
            if (on && chud) g_hud[i].Set(&Field<uint8_t>(chud, REACH_CHUD_SHOW + i * REACH_CHUD_SHOW_STRIDE), 0);
            else g_hud[i].Restore();
        }
    }

    // Equipment that removes itself after a set time (the drop shield bubble,
    // 15 s) loses that time while infinite equipment is on; restored when off.
    void NoSelfDestruct(bool on) {
        float* wanted[_countof(g_selfDestruct)] = {};
        int count = 0;
        for (size_t i = 0; on && i < g_equipment.size() && count < (int)_countof(wanted); ++i) {
            char* tag = ReachTags::Get(g_equipment[i].datum);
            char* mine = ReachTags::BlockElement(tag, REACH_EQIP_PROXIMITY_MINE, REACH_EQIP_MINE_SIZE, 0);
            if (mine) wanted[count++] = &Field<float>(mine, REACH_EQIP_MINE_SELF_DESTRUCT);
        }
        SetValues(g_selfDestruct, wanted, count, 0.0f);
    }

    // Keeps the players' armor abilities fully charged.
    void InfiniteEquipment(const Players& p, const Array& objects, bool on) {
        NoSelfDestruct(on);
        if (!on) return;
        for (int i = 0; i < p.count; ++i) {
            char* equipment = ObjectOfType(objects, Field<uint32_t>(p.units[i], REACH_UNIT_EQUIPMENT),
                                           REACH_OBJECT_TYPE_EQUIPMENT);
            if (equipment) Field<float>(equipment, REACH_EQUIPMENT_ENERGY) = 1.0f;
        }
    }

    // Perfect camo: while the armor ability is active camo, the unit's camo
    // value (1.0 = fully effective) is held at 1.0 instead of falling as the
    // player moves. Only while the ability is running and the value is above 0,
    // so the camo still fades out when it's switched off.
    void PerfectCamo(const Players& p, const Array& objects, bool on) {
        if (!on) return;
        for (int i = 0; i < p.count; ++i) {
            char* equipment = ObjectOfType(objects, Field<uint32_t>(p.units[i], REACH_UNIT_EQUIPMENT),
                                           REACH_OBJECT_TYPE_EQUIPMENT);
            if (!equipment || Field<uint32_t>(equipment, REACH_EQUIPMENT_ACTIVE_HANDLE) == 0xFFFFFFFF) continue;
            auto& value = Field<float>(p.units[i], REACH_UNIT_CAMO_VALUE);
            if (!(value > 0.0f)) continue;
            value = 1.0f;
            Field<float>(p.units[i], REACH_UNIT_CAMO_PREVIOUS) = 1.0f;
        }
    }

    // Weapon state edits, player weapons only: the trigger always counts as
    // released (full-auto), no spread, no heat. Rapid fire is on the game
    // thread (main_thread.cpp).
    void WeaponMods(const Players& p, const Array& objects, const Games::Settings& s) {
        bool vehicleFullAuto = s.fullAuto || s.vehicleRapidFire;
        if (!s.fullAuto && !s.noSpread && !s.noOverheat && !vehicleFullAuto) return;

        // No overheat and full auto also cover the vehicle (or turret) each
        // player is in; full auto there is what lets the Sabre's missiles
        // fire while the trigger is held. Vehicle rapid fire turns it on too.
        for (int i = 0; (s.noOverheat || vehicleFullAuto) && i < p.count; ++i) {
            char* parts[16];
            int partCount = VehicleParts(p.units[i], objects, parts, _countof(parts));
            for (int k = 0; k < partCount; ++k)
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(parts[k], objects, w);
                if (!weapon) continue;
                if (s.noOverheat) Field<float>(weapon, REACH_WEAPON_HEAT) = 0.0f;
                if (vehicleFullAuto)
                    for (int t = 0; t < REACH_WEAPON_TRIGGER_COUNT; ++t)
                        Field<uint8_t>(weapon, REACH_WEAPON_TRIGGERS + t * REACH_WEAPON_TRIGGER_SIZE +
                                                   REACH_TRIGGER_FLAGS) |= REACH_TRIGGER_FLAG_READY;
            }
        }

        for (int i = 0; i < p.count; ++i) {
            for (int w = 0; w < REACH_UNIT_WEAPON_COUNT; ++w) {
                char* weapon = WeaponInSlot(p.units[i], objects, w);
                if (!weapon) continue;
                if (s.fullAuto)
                    for (int t = 0; t < REACH_WEAPON_TRIGGER_COUNT; ++t)
                        Field<uint8_t>(weapon, REACH_WEAPON_TRIGGERS + t * REACH_WEAPON_TRIGGER_SIZE +
                                                   REACH_TRIGGER_FLAGS) |= REACH_TRIGGER_FLAG_READY;
                if (s.noSpread)
                    for (int b = 0; b < REACH_WEAPON_BARREL_COUNT; ++b)
                        Field<float>(weapon, REACH_WEAPON_BARRELS + b * REACH_WEAPON_BARREL_SIZE + REACH_BARREL_ERROR) = 0.0f;
                if (s.noOverheat) Field<float>(weapon, REACH_WEAPON_HEAT) = 0.0f;
            }
        }
    }

    // Weapons flagged "Cannot Be Used By Player" (turrets, vehicle guns).
    bool PlayerCanUse(uint32_t weaponTag) {
        __try {
            char* tag = ReachTags::Get(weaponTag);
            return tag && !(Field<uint32_t>(tag, REACH_WEAP_FLAGS) & REACH_WEAP_FLAG_NOT_FOR_PLAYER);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Characters that have a unit (biped) of their own or from a parent.
    // Vehicles the mission places itself (its scenario's vehicle palette,
    // which squads' vehicles also come from). The map's other 'vehi' tags
    // are parts: turrets and guns other vehicles carry (warthog_chaingun,
    // wraith_mortar) or cutscene props' guns (falcon_side_gun_left).
    bool InVehiclePalette(char* scnr, uint32_t vehicle) {
        __try {
            int count = ReachTags::BlockCount(scnr, REACH_SCNR_VEHICLE_PALETTE);
            for (int i = 0; i < count; ++i) {
                char* entry = ReachTags::BlockElement(scnr, REACH_SCNR_VEHICLE_PALETTE, REACH_PALETTE_ENTRY_SIZE, i);
                if (entry && Field<uint32_t>(entry, REACH_PALETTE_ENTRY_DATUM) == vehicle) return true;
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct ModelVariant {
        uint32_t name = 0;
        int objects = 0;
        uint32_t parentMarker = 0;  // of its first object
        uint32_t childMarker = 0;
    };

    // A vehicle's model's variants. SEH: tag reads.
    int VariantsOf(uint32_t vehicle, ModelVariant* out, int max) {
        __try {
            char* tag = ReachTags::Get(vehicle);
            char* model = tag ? ReachTags::Get(Field<uint32_t>(tag, REACH_OBJE_MODEL + REACH_TAGREF_DATUM)) : nullptr;
            int count = model ? ReachTags::BlockCount(model, REACH_HLMT_VARIANTS) : 0;
            int n = 0;
            for (int v = 0; v < count && n < max; ++v) {
                char* variant = ReachTags::BlockElement(model, REACH_HLMT_VARIANTS, REACH_HLMT_VARIANT_SIZE, v);
                if (!variant) continue;
                ModelVariant& m = out[n++];
                m = {};
                m.name = Field<uint32_t>(variant, REACH_HLMT_VARIANT_NAME);
                m.objects = ReachTags::BlockCount(variant, REACH_HLMT_VARIANT_OBJECTS);
                if (char* object = ReachTags::BlockElement(variant, REACH_HLMT_VARIANT_OBJECTS, REACH_HLMT_OBJECT_SIZE, 0)) {
                    m.parentMarker = Field<uint32_t>(object, REACH_HLMT_OBJECT_PARENT_MARKER);
                    m.childMarker = Field<uint32_t>(object, REACH_HLMT_OBJECT_CHILD_MARKER);
                }
            }
            return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // Every object some model attaches to one of its variants.
    void AddAttached(uint32_t model, std::unordered_set<uint32_t>& attached) {
        __try {
            char* tag = ReachTags::Get(model);
            int count = tag ? ReachTags::BlockCount(tag, REACH_HLMT_VARIANTS) : 0;
            for (int v = 0; v < count && v < 256; ++v) {
                char* variant = ReachTags::BlockElement(tag, REACH_HLMT_VARIANTS, REACH_HLMT_VARIANT_SIZE, v);
                int objects = variant ? ReachTags::BlockCount(variant, REACH_HLMT_VARIANT_OBJECTS) : 0;
                for (int o = 0; o < objects && o < 256; ++o)
                    if (char* object = ReachTags::BlockElement(variant, REACH_HLMT_VARIANT_OBJECTS, REACH_HLMT_OBJECT_SIZE, o))
                        attached.insert(Field<uint32_t>(object, REACH_HLMT_OBJECT_CHILD));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    uint32_t DefaultVariant(uint32_t vehicle) {
        __try {
            char* tag = ReachTags::Get(vehicle);
            return tag ? Field<uint32_t>(tag, REACH_OBJE_DEFAULT_VARIANT) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // The driver seats of the mission's vehicles that players may not use.
    void FindAiOnlySeats(const std::vector<Games::TagChoice>& vehicles, std::vector<AiOnlySeat>& out) {
        __try {
            for (auto& vehicle : vehicles) {
                char* tag = ReachTags::Get(vehicle.datum);
                int seats = ReachTags::BlockCount(tag, REACH_VEHI_SEATS);
                for (int i = 0; i < seats && i < 64; ++i) {
                    char* seat = ReachTags::BlockElement(tag, REACH_VEHI_SEATS, REACH_VEHI_SEAT_SIZE, i);
                    uint32_t flags = seat ? Field<uint32_t>(seat, REACH_SEAT_FLAGS) : 0;
                    if (flags & REACH_SEAT_FLAG_INVALID_FOR_PLAYER)
                        out.push_back({vehicle.datum, Field<uint32_t>(seat, REACH_SEAT_NAME),
                                       &Field<uint32_t>(seat, REACH_SEAT_FLAGS),
                                       (flags & REACH_SEAT_FLAG_DRIVER) != 0, {}});
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void PilotAiVehicles(bool on) {
        std::lock_guard<std::mutex> lock(g_vehicleChoicesLock);
        for (auto& seat : g_aiOnlySeats) {
            if (!on) seat.edit.Restore();
            else if (!seat.edit.Ours(seat.flags)) seat.edit.Set(seat.flags, *seat.flags & ~REACH_SEAT_FLAG_INVALID_FOR_PLAYER);
        }
    }

    // The Vehicles list: each vehicle the mission places (its default
    // variant), then its model's other variants, then turrets the map has
    // for it that no variant carries, on its turretless variant.
    void BuildVehicleList(char* scnr) {
        std::vector<Games::TagChoice> all = ReachTags::ListGroup(REACH_TAG_VEHI);
        std::unordered_set<uint32_t> attached;
        for (auto& model : ReachTags::ListGroup(REACH_TAG_HLMT)) AddAttached(model.datum, attached);

        std::vector<Games::TagChoice> list;
        std::vector<VehicleChoice> choices;
        auto add = [&](VehicleChoice c) {
            list.push_back({kVehicleChoiceId | (uint32_t)choices.size(), c.name});
            choices.push_back(c);
        };
        for (auto& vehicle : all) {
            if (!InVehiclePalette(scnr, vehicle.datum)) continue;
            list.push_back(vehicle);

            ModelVariant variants[64];
            int count = VariantsOf(vehicle.datum, variants, _countof(variants));
            uint32_t defaultName = DefaultVariant(vehicle.datum);
            if (!defaultName && count > 0) defaultName = variants[0].name;
            const ModelVariant* bare = nullptr;   // carries nothing
            const ModelVariant* mount = nullptr;  // carries something: where turrets go
            for (int v = 0; v < count; ++v) {
                if (!bare && variants[v].objects == 0) bare = &variants[v];
                if (variants[v].objects > 0 && (!mount || variants[v].name == defaultName)) mount = &variants[v];
                if (variants[v].name == defaultName) continue;
                std::string label = ReachTags::StringName(variants[v].name);
                VehicleChoice c;
                c.vehicle = vehicle.datum;
                c.variant = variants[v].name;
                c.name = vehicle.name + " (" + (label.empty() ? "variant " + std::to_string(v) : label) + ")";
                add(c);
            }
            if (!bare || !mount) continue;

            std::string full = ReachTags::FullName(vehicle.datum);
            std::string folder = full.substr(0, full.find_last_of('\\') + 1);  // objects\vehicles\human\warthog\ //
            if (folder.empty()) continue;
            for (auto& turret : all) {
                if (turret.datum == vehicle.datum || attached.count(turret.datum) ||
                    InVehiclePalette(scnr, turret.datum) || ReachTags::FullName(turret.datum).rfind(folder, 0) != 0)
                    continue;
                std::string label = turret.name;
                if (label.rfind(vehicle.name + "_", 0) == 0) label = label.substr(vehicle.name.size() + 1);
                VehicleChoice c;
                c.vehicle = vehicle.datum;
                c.variant = bare->name;
                c.turret = turret.datum;
                c.parentMarker = mount->parentMarker;
                c.childMarker = mount->childMarker;
                c.name = vehicle.name + " (" + label + ")";
                add(c);
            }
        }

        std::vector<AiOnlySeat> seats;
        FindAiOnlySeats(all, seats);  // every vehicle tag in the map, not just the palette
        for (auto& seat : seats)
            LOG("Reach: %s: seat '%s'%s is AI-only", ReachTags::FullName(seat.vehicle).c_str(),
                ReachTags::StringName(seat.seatName).c_str(), seat.driver ? " (driver)" : "");

        g_vehicles = std::move(list);
        std::lock_guard<std::mutex> lock(g_vehicleChoicesLock);
        g_vehicleChoices = std::move(choices);
        g_aiOnlySeats = std::move(seats);  // the old map's edits went with it
    }

    // The Objects list: what the mission places of each kind (its
    // palettes), prefixed by kind so they group ("crate: pallet"). Scenery
    // the level uses as editing helpers is left out.
    void BuildObjectList(char* scnr) {
        struct Kind { int palette; const char* prefix; };
        constexpr Kind kKinds[] = {{REACH_SCNR_CRATE_PALETTE, "crate"},
                                   {REACH_SCNR_EQUIPMENT_PALETTE, "equipment"},
                                   {REACH_SCNR_MACHINE_PALETTE, "machine"},
                                   {REACH_SCNR_SCENERY_PALETTE, "scenery"}};
        constexpr const char* kHelpers[] = {"invisible", "pathfinding", "ui_object", "fx_", "standin", "kill_"};
        g_objects.clear();
        for (const Kind& kind : kKinds) {
            std::vector<Games::TagChoice> found;
            int count = ReachTags::BlockCount(scnr, kind.palette);
            for (int i = 0; i < count && i < 1024; ++i) {
                char* entry = ReachTags::BlockElement(scnr, kind.palette, REACH_PALETTE_ENTRY_SIZE, i);
                uint32_t tag = entry ? Field<uint32_t>(entry, REACH_PALETTE_ENTRY_DATUM) : 0xFFFFFFFF;
                if (tag == 0xFFFFFFFF || !ReachTags::Get(tag)) continue;
                std::string name = ReachTags::FullName(tag);
                name = name.substr(name.find_last_of('\\') + 1);
                bool helper = name.empty();
                for (const char* h : kHelpers) helper |= name.find(h) != std::string::npos;
                if (helper) continue;
                bool dup = false;
                for (auto& f : found) dup |= f.datum == tag;
                if (!dup) found.push_back({tag, std::string(kind.prefix) + ": " + name});
            }
            std::sort(found.begin(), found.end(),
                      [](const Games::TagChoice& a, const Games::TagChoice& b) { return a.name < b.name; });
            g_objects.insert(g_objects.end(), found.begin(), found.end());
        }
    }

    bool Spawnable(uint32_t character) {
        __try {
            for (int depth = 0; depth < 8 && character != 0xFFFFFFFF; ++depth) {
                char* tag = ReachTags::Get(character);
                if (!tag) return false;
                if (Field<uint32_t>(tag, REACH_CHAR_UNIT) != 0xFFFFFFFF) return true;
                character = Field<uint32_t>(tag, REACH_CHAR_PARENT);
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void RefreshLists(char* scnr) {
        if (scnr == g_listsScenario) return;
        g_listsScenario = scnr;
        ReachTags::LoadNames();
        g_weapons.clear();
        for (auto& weapon : ReachTags::ListGroup(REACH_TAG_WEAP))
            if (PlayerCanUse(weapon.datum)) g_weapons.push_back(weapon);
        // Ammo boxes and grenades are equipment tags too, but not abilities.
        g_equipment.clear();
        for (auto& equipment : ReachTags::ListGroup(REACH_TAG_EQIP)) {
            const std::string& name = equipment.name;
            if (name.find("ammo") == std::string::npos && name.find("grenade") == std::string::npos &&
                name.find("rocket") == std::string::npos)
                g_equipment.push_back(equipment);
        }
        g_projectiles = ReachTags::ListGroup(REACH_TAG_PROJ);
        BuildVehicleList(scnr);
        BuildObjectList(scnr);
        g_characters.clear();
        for (auto& character : ReachTags::ListGroup(REACH_TAG_CHAR))
            if (Spawnable(character.datum)) g_characters.push_back(character);
        {
            std::lock_guard<std::mutex> lock(g_poolsLock);
            auto datums = [](const std::vector<Games::TagChoice>& list) {
                std::vector<uint32_t> out;
                for (const auto& c : list) out.push_back(c.datum);
                return out;
            };
            g_pools.projectiles = datums(g_projectiles);
            g_pools.vehicles = datums(g_vehicles);
            // Scenery and machines have no physics: dropped, they'd hang in the air.
            g_pools.objects.clear();
            for (const auto& c : g_objects) {
                uint32_t group = ReachTags::GroupOf(c.datum);
                if (group != REACH_TAG_SCEN && group != REACH_TAG_MACH) g_pools.objects.push_back(c.datum);
            }
            g_pools.ai = datums(g_characters);
            g_pools.weapons = datums(g_weapons);
        }
        LOG("Reach: map has %zu weapons, %zu projectiles, %zu characters, %zu vehicles, %zu objects",
            g_weapons.size(), g_projectiles.size(), g_characters.size(), g_vehicles.size(), g_objects.size());
    }

    // Reads the pointers the cheats need. SEH: they can be mid-teardown.
    bool ReadFrame(void** state, Frame& f) {
        __try {
            f.players.header = (char*)state[REACH_TLS_SLOT_PLAYERS];
            f.objects.header = (char*)state[REACH_TLS_SLOT_OBJECTS];
            f.actors.header = (char*)state[REACH_TLS_SLOT_ACTORS];
            f.gameGlobals = (char*)state[REACH_TLS_SLOT_GAME_GLOBALS];
            f.gameTime = (char*)state[REACH_TLS_SLOT_GAME_TIME];
            f.physics = (char*)state[REACH_TLS_SLOT_PHYSICS];
            f.aiGlobals = (char*)state[REACH_TLS_SLOT_AI_GLOBALS];
            f.chud = (char*)state[REACH_TLS_SLOT_CHUD];
            f.allegiance = (char*)state[REACH_TLS_SLOT_ALLEGIANCE];
            f.matg = ReachTags::Globals();
            f.scnr = ReachTags::Scenario();
            return f.players.Valid() && f.objects.Valid() && f.gameGlobals;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Each group gets its own guard, so a bad offset in one can't stop the others.
    template <typename Fn>
    void Guard(const char* what, Fn fn) {
        __try {
            fn();
        } __except (Log::Exception(GetExceptionInformation(), what)) {
        }
    }

    bool ApplyAll(const Frame& f, const Games::Settings& s, Games::Status& status) {
        Players p;
        __try {
            p = FindPlayers(f.players, f.objects);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        Guard("god/blind", [&] {
            UnitFlag(p, f.objects, REACH_OBJECT_DAMAGE_FLAGS, REACH_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE, s.godMode, g_godApplied);
            UnitFlag(p, f.objects, REACH_UNIT_FLAGS, REACH_UNIT_FLAG_AI_DISREGARD, s.aiBlind, g_blindApplied);
        });
        g_noFriendlyFire = s.noFriendlyFire;
        Guard("no betrayal", [&] { NoBetrayal(f.allegiance, s.noBetrayal); });
        Guard("ammo", [&] { Ammo(p, f.objects, s.infiniteAmmo, s.bottomlessClip); });
        Guard("grenades", [&] { Grenades(p, f.matg, s.infiniteGrenades); });
        Guard("one-hit kill", [&] { OneHitKill(p, f.objects, s.oneHitKill); });
        Guard("speed", [&] { if (f.matg) Speed(f.matg, s.speed); });
        Guard("jump", [&] { Jump(p, s.jump); });
        Guard("vehicle speed", [&] { VehicleSpeed(p, f.objects, s.vehicleSpeed); });
        Guard("vehicle god mode", [&] { VehicleGod(p, f.objects, s.vehicleGodMode); });
        Guard("vehicle no EMP", [&] { VehicleNoEmp(p, f.objects, s.vehicleNoEmp); });
        Guard("pilot AI-only vehicles", [&] { PilotAiVehicles(s.pilotAiVehicles); });
        Guard("vehicle no hijack", [&] { VehicleNoHijack(p, f.objects, s.vehicleNoHijack); });
        Guard("vehicle infinite boost", [&] {
            char* vehicle = p.count > 0 ? VehicleOf(p.units[0], f.objects) : nullptr;
            char* tag = vehicle ? ReachTags::Get(Field<uint32_t>(vehicle, REACH_OBJECT_TAG_DATUM)) : nullptr;
            g_infiniteBoost.Update(tag, kInfiniteBoost, _countof(kInfiniteBoost), s.vehicleInfiniteBoost);
        });
        Guard("gravity", [&] {
            if (f.physics) Scale(g_gravity, &Field<float>(f.physics, REACH_PHYSICS_GRAVITY), s.gravity);
        });
        Guard("game speed", [&] {
            if (f.gameTime) Scale(g_gameSpeed, &Field<float>(f.gameTime, REACH_GAME_TIME_SPEED), s.gameSpeed);
        });
        Guard("boundaries", [&] { NoBoundaries(f.scnr, s.noBoundaries); });
        Guard("aimbot", [&] { Aimbot(p, f.objects, s.aimbot); });
        Guard("ammo type", [&] { AmmoType(p, f.objects, s.ammoType); });
        Guard("no recoil", [&] { NoRecoil(p, f.objects, s.noRecoil); });
        Guard("vehicle no recoil", [&] { VehicleNoRecoil(p, f.objects, s.vehicleNoRecoil); });
        Guard("ai can't shoot", [&] { AiCantShoot(f.aiGlobals, s.aiCantShoot); });
        Guard("ai deaf", [&] { AiDeaf(f.actors, s.aiDeaf); });
        Guard("no hud", [&] { NoHud(f.chud, s.noHud); });
        Guard("equipment", [&] { InfiniteEquipment(p, f.objects, s.infiniteEquipment); });
        Guard("weapon mods", [&] { WeaponMods(p, f.objects, s); });
        g_thirdPerson.Apply(s.thirdPerson);
        g_noHeat.Apply(s.noOverheat);  // covers vehicle guns the heat reset misses (and the AI's)

        status.playerCount = p.count;
        Guard("status", [&] {
            if (p.count == 0) return;
            if (float* health = Health(p.units[0])) status.health = *health;
            if (float* shield = Shield(p.units[0])) status.shield = *shield;
        });
        return true;
    }

    bool InitPatch(::Patch::Nop& patch, HMODULE reach, const char* pattern, size_t size, int offset = 0,
                   const uint8_t* bytes = nullptr) {
        uint8_t* at = ::Patch::FindUnique(reach, pattern);
        if (!at) return false;
        return bytes ? patch.Init(at + offset, bytes, size) : patch.Init(at + offset, size);
    }
}

void ReachCheats::Load(HMODULE reach) {
    SetMainThreadModule(reach);
    g_moduleBase = (char*)reach;
    if (uint8_t* load = ::Patch::FindUnique(reach, REACH_AOB_EXPLOSION_PUSH)) {
        // A float cell within rel32 reach of the module, then the load re-aimed at it.
        uintptr_t base = (uintptr_t)reach & ~(uintptr_t)0xFFFF;
        for (uintptr_t delta = 0x10000; !g_explosionCell && delta < 0x70000000; delta += 0x10000)
            for (int sign : {-1, 1}) {
                void* want = (void*)(sign < 0 ? base - delta : base + delta);
                if (void* got = VirtualAlloc(want, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                    g_explosionCell = (float*)got;
                    break;
                }
            }
        if (g_explosionCell) {
            *g_explosionCell = REACH_EXPLOSION_PUSH_FACTOR;
            uint8_t bytes[8] = {0xF3, 0x0F, 0x10, 0x15};
            *(int32_t*)(bytes + 4) = (int32_t)((uint8_t*)g_explosionCell - (load + 8));
            g_explosionPush.Init(load, bytes, sizeof(bytes));
        }
    }
    if (!g_explosionPush.Ready()) LOG("Reach: explosion push patch NOT FOUND");
    bool blind = InitPatch(g_aiInvisible1, reach, REACH_AOB_AI_INVISIBLE_1, 7) &&
                 InitPatch(g_aiInvisible2, reach, REACH_AOB_AI_INVISIBLE_2, 6);
    const uint8_t thirdBytes[] = REACH_THIRD_PERSON_PATCH;
    bool third = InitPatch(g_thirdPerson, reach, REACH_AOB_THIRD_PERSON, sizeof(thirdBytes),
                           REACH_THIRD_PERSON_OFFSET, thirdBytes);
    if (!InitPatch(g_noHeat, reach, REACH_AOB_NO_HEAT, 8)) LOG("Reach: no-heat patch NOT FOUND");
    LOG("Reach: AI-invisible patch %s, third-person patch %s", blind ? "found" : "NOT FOUND",
        third ? "found" : "NOT FOUND");
}

void ReachCheats::Unload() {
    ::Patch::Nop* patches[] = {&g_aiInvisible1, &g_aiInvisible2, &g_thirdPerson, &g_noHeat, &g_explosionPush};
    for (auto* patch : patches) {
        patch->Apply(false);
        patch->Forget();
    }
    if (g_explosionCell) {
        VirtualFree(g_explosionCell, 0, MEM_RELEASE);
        g_explosionCell = nullptr;
    }
    g_explosionScale = 1.0f;
    g_fovFirst = {};
    g_fovThird = {};
    g_moduleBase = nullptr;
    g_aiCantShoot = false;
    g_timeStop = false;
    g_noFriendlyFire = false;

    // The game's memory goes with the module: forget, don't restore.
    g_godApplied = FlagState();
    g_blindApplied = FlagState();
    for (auto& r : g_speed) r = {};
    for (auto& r : g_jump) r = {};
    g_vehicleSpeed.Forget();
    g_vehicleGod.Forget();
    g_vehicleNoEmp.Forget();
    g_vehicleReserved.Forget();
    g_infiniteBoost.Forget();
    for (auto& r : g_lockedSeats) r = {};
    g_gravity = {};
    g_gameSpeed = {};
    g_killTriggerCount = {};
    for (auto& row : g_softCeilingFlags)
        for (auto& r : row) r = {};
    g_aiGrenades = {};
    for (auto& r : g_hud) r = {};
    for (auto& r : g_deaf) r = {};
    for (auto& a : g_aimbot) a = {};
    for (auto& r : g_ammo) r = {};
    for (auto& r : g_selfDestruct) r = {};
    for (auto& r : g_recoil) r = {};
    for (auto& r : g_vehicleRecoil) r = {};
    for (auto& r : g_vehiclePush) r = {};
    g_listsScenario = nullptr;
    g_weapons.clear();
    g_equipment.clear();
    g_projectiles.clear();
    g_vehicles.clear();
    g_objects.clear();
    {
        std::lock_guard<std::mutex> lock(g_vehicleChoicesLock);
        g_vehicleChoices.clear();
        g_aiOnlySeats.clear();
    }
    g_characters.clear();
    {
        std::lock_guard<std::mutex> lock(g_poolsLock);
        g_pools = {};
    }
    g_chaosGun = false;
    g_recoilPush = 0.0f;
    ResetMainThread();
    SetMainThreadModule(nullptr);
}

// The field of view and explosion edits (no C++ objects here, so SEH is allowed).
static void ApplyViewAndExplosions(float fovSetting, float explosionScale) {
    __try {
        if (g_moduleBase) {
            float* fov = (float*)(g_moduleBase + REACH_FOV_SETTING);
            if (fovSetting > 0.0f) {
                g_fovFirst.Set(fov, fovSetting);
                g_fovThird.Set(fov + 1, fovSetting + 1.0f);
            } else {
                g_fovFirst.Restore();
                g_fovThird.Restore();
            }
        }
        g_explosionScale = explosionScale;
        bool push = g_explosionCell && g_explosionPush.Ready() && fabsf(explosionScale - 1.0f) > 0.001f;
        if (g_explosionCell) *g_explosionCell = REACH_EXPLOSION_PUSH_FACTOR * explosionScale;
        g_explosionPush.Apply(push);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ReachCheats::AfterTick(void** gameState, const Games::Settings& settings) {
    if (!settings.perfectCamo) return;
    Frame f{};
    if (!ReadFrame(gameState, f)) return;
    Players p;
    __try {
        p = FindPlayers(f.players, f.objects);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    Guard("perfect camo", [&] { PerfectCamo(p, f.objects, true); });
}

bool ReachCheats::Apply(void** gameState, const Games::Settings& settings, Games::Status& status) {
    g_noEject = settings.vehicleNoEject;
    g_chaosGun = settings.chaosGun;
    g_recoilPush = settings.recoilPush ? settings.recoilStrength : 0.0f;
    status.fovMin = REACH_FOV_MIN;
    status.fovMax = REACH_FOV_MAX;
    status.fovDefault = REACH_FOV_DEFAULT;
    ApplyViewAndExplosions(settings.fov, settings.explosionScale);
    status.skullNames = kSkullNames;
    status.skullCount = REACH_SKULL_COUNT;
    g_timeStop = settings.timeStop;
    Frame f{};
    if (!ReadFrame(gameState, f)) return false;
    Guard("skulls", [&] { status.skullsActive = Field<uint64_t>(f.gameGlobals, REACH_GAME_GLOBALS_SKULLS); });

    Guard("tag lists", [&] { RefreshLists(f.scnr); });
    Guard("boundary view", [&] {
        static Games::BoundaryView view;
        if (!settings.showBoundaries && !settings.showSoftCeilings) { status.boundaryView = nullptr; return; }
        int kills = 0;
        if (f.scnr) {
            auto* count = &Field<int32_t>(f.scnr, REACH_SCNR_KILL_TRIGGERS);
            kills = g_killTriggerCount.Ours(count) ? g_killTriggerCount.original : *count;
        }
        BoundaryFill(gameState, f.scnr, kills, settings, view);
        status.boundaryView = view.valid ? &view : nullptr;
    });
    status.weapons = &g_weapons;
    status.equipment = &g_equipment;
    g_checkpointStatus.clear();
    CheckpointHistory(g_checkpointStatus);
    status.checkpoints = &g_checkpointStatus;
    status.projectiles = &g_projectiles;
    status.spawnAi = &g_characters;
    status.squadsAvailable[0] = status.squadsAvailable[1] = 0;
    for (int i = 0; i < Games::kSquadCount; ++i)
        if (Games::SquadInTitle(Games::kSquads[i], "Halo: Reach") && !Games::SquadTags(Games::kSquads[i], g_characters).empty())
            Games::SetSquadAvailable(status, i);
    status.spawnVehicles = &g_vehicles;
    status.spawnObjects = &g_objects;
    SpawnStatus(status);

    return ApplyAll(f, settings, status);
}

// A weapon may fire unless AI can't shoot is on and it isn't held by a local
// player or by a vehicle a player is in (so turrets the player uses still work).
namespace {
    uint32_t g_allowEjectUnit = 0xFFFFFFFF;
}

void ReachCheats::AllowEject(uint32_t unit) { g_allowEjectUnit = unit; }

float ReachCheats::ExplosionScale() { return g_explosionScale; }

bool ReachCheats::FlipEjectAllowed(void** state, uint32_t unit, void* event) {
    if (!g_noEject || !state || unit == g_allowEjectUnit) return true;
    __try {
        Array objects{(char*)state[REACH_TLS_SLOT_OBJECTS]};
        if (!objects.Valid()) return true;
        char* object = ObjectFromDatum(objects, unit);
        if (!object || Field<int32_t>(object, REACH_UNIT_PLAYER_INDEX) == -1) return true;
        // The handler also runs your own exit request (unit action 0xC) while
        // the vehicle is flipped; only the flip eviction (action 0x15, with the
        // eject byte the handler tests) is skipped. Logged live 2026-10-02.
        using GetDataFn = uint8_t*(__fastcall*)(void* event, int32_t id);
        uint8_t* data = event ? (*(GetDataFn**)event)[0](event, REACH_FLIP_EJECT_EVENT) : nullptr;
        if (!data || !data[REACH_FLIP_EJECT_DATA_EJECT]) return true;
        static int s_logged = 0;
        if (s_logged++ < 3) LOG("Reach: kept player unit %08X in its flipped vehicle", unit);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// Friendly fire: a hit from the player on one of their allied AI, or from
// one of those AI on the player, is dropped (the hit's owner is in the
// damage data, see offsets.h). Logs the first few decisions.
bool ReachCheats::DamageAllowed(void** state, const void* data, uint32_t victimDatum) {
    if (!g_noFriendlyFire || !state || !data) return true;
    __try {
        Array objects{(char*)state[REACH_TLS_SLOT_OBJECTS]};
        Array players{(char*)state[REACH_TLS_SLOT_PLAYERS]};
        if (!objects.Valid() || !players.Valid()) return true;
        Players p = FindPlayers(players, objects);
        if (p.count == 0) return true;
        char* victim = ObjectFromDatum(objects, victimDatum);
        if (!victim) return true;

        const char* d = (const char*)data;
        uint32_t ownerPlayer = Field<uint32_t>((char*)d, REACH_DAMAGE_OWNER_PLAYER);
        uint32_t ownerDatum = Field<uint32_t>((char*)d, REACH_DAMAGE_OWNER_UNIT);
        char* owner = ObjectFromDatum(objects, ownerDatum);
        if (!owner && ownerPlayer != 0xFFFFFFFF) {
            char* entry = players.Get(ownerPlayer);
            if (entry) {
                ownerDatum = Field<uint32_t>(entry, REACH_PLAYER_UNIT_DATUM);
                owner = ObjectFromDatum(objects, ownerDatum);
            }
        }
        int mine = PlayerTeam(p);
        bool victimAlly = !p.Contains(victim) && ObjectOfType(objects, victimDatum, REACH_OBJECT_TYPE_BIPED) &&
                          Field<uint32_t>(victim, REACH_UNIT_ACTOR) != 0xFFFFFFFF &&
                          Friends(Field<int8_t>(victim, REACH_UNIT_TEAM), mine);
        // Diagnostics: each distinct kind of hit on an allied AI (effect, flags, who and whether it
        // was blocked) is logged once, so rare kinds aren't buried by the common ones.
        static uint64_t s_seen[160];
        static int s_seenCount = 0;
        auto note = [&](bool blocked, const char* who) {
            if (!victimAlly) return;
            ++g_ffStats.onAllies;
            if (blocked) ++g_ffStats.blocked;
            else if (who[0] == 'n') ++g_ffStats.noOwner;
            else ++g_ffStats.throughOwned;
            g_ffStats.lastEffect = Field<uint32_t>((char*)d, 0);
            g_ffStats.lastFlags = Field<uint32_t>((char*)d, 4);
            g_ffStats.lastOwner = ownerDatum;
            uint64_t sig = ((uint64_t)Field<uint32_t>((char*)d, 0) << 32) ^ ((uint64_t)Field<uint32_t>((char*)d, 4) << 1) ^
                           (uint64_t)(blocked ? 1 : 0) ^ ((uint64_t)who[0] << 8);
            for (int i = 0; i < s_seenCount; ++i)
                if (s_seen[i] == sig) return;
            if (s_seenCount >= 160) return;
            s_seen[s_seenCount++] = sig;
            LOG("Reach: hit on ally %08X (%s): effect %08X flags %08X owner player %08X unit %08X -> %s", victimDatum, who,
                Field<uint32_t>((char*)d, 0), Field<uint32_t>((char*)d, 4), ownerPlayer, ownerDatum,
                blocked ? "blocked" : "let through");
        };
        if (!owner) {
            note(false, "no owner");
            return true;
        }

        auto ally = [&](uint32_t datum, char* object) {
            if (p.Contains(object) || !ObjectOfType(objects, datum, REACH_OBJECT_TYPE_BIPED)) return false;
            if (Field<uint32_t>(object, REACH_UNIT_ACTOR) == 0xFFFFFFFF) return false;
            return Friends(Field<int8_t>(object, REACH_UNIT_TEAM), mine);
        };
        bool blocked = (p.Contains(owner) && ally(victimDatum, victim)) || (ally(ownerDatum, owner) && p.Contains(victim));
        note(blocked, p.Contains(owner) ? "player" : "other");
        static int s_logged = 0;
        if (blocked && s_logged++ < 4)
            LOG("Reach: friendly fire blocked: owner %08X (player %08X) -> victim %08X", ownerDatum, ownerPlayer,
                victimDatum);
        return !blocked;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

ReachCheats::FriendlyFireStats ReachCheats::TakeFriendlyFireStats() {
    FriendlyFireStats out = g_ffStats;
    g_ffStats = {};
    return out;
}

bool ReachCheats::WeaponMayFire(void** state, uint32_t weapon) {
    if (!(g_aiCantShoot || g_timeStop) || !state) return true;
    __try {
        Array objects{(char*)state[REACH_TLS_SLOT_OBJECTS]};
        Array players{(char*)state[REACH_TLS_SLOT_PLAYERS]};
        if (!objects.Valid() || !players.Valid()) return true;
        char* object = ObjectFromDatum(objects, weapon);
        if (!object) return true;
        uint32_t holder = Field<uint32_t>(object, REACH_OBJECT_PARENT);
        if (holder == 0xFFFFFFFF) return true;  // not held (e.g. a dropped weapon)

        Players p = FindPlayers(players, objects);
        for (int i = 0; i < p.count; ++i) {
            if (p.datums[i] == holder) return true;
            if (Field<uint32_t>(p.units[i], REACH_OBJECT_PARENT) == holder) return true;  // player's vehicle
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

bool ReachCheats::FindVehicleChoice(uint32_t id, VehicleChoice& out) {
    if ((id & 0xFFFF0000) != kVehicleChoiceId) return false;
    std::lock_guard<std::mutex> lock(g_vehicleChoicesLock);
    uint32_t index = id & 0xFFFF;
    if (index >= g_vehicleChoices.size()) return false;
    out = g_vehicleChoices[index];
    return true;
}

bool ReachCheats::IsAiOnlyVehicle(uint32_t vehicle) {
    std::lock_guard<std::mutex> lock(g_vehicleChoicesLock);
    for (auto& seat : g_aiOnlySeats)
        if (seat.driver && seat.vehicle == vehicle) return true;
    return false;
}

namespace {
    uint32_t PickFrom(const std::vector<uint32_t>& list) {
        if (list.empty()) return 0xFFFFFFFF;
        g_random ^= g_random << 13;
        g_random ^= g_random >> 17;
        g_random ^= g_random << 5;
        return list[g_random % list.size()];
    }

    uint32_t PickProjectile() {
        std::lock_guard<std::mutex> lock(g_poolsLock);
        return PickFrom(g_pools.projectiles);
    }

    // Recoil pushes waiting for the game tick (see TakePushes).
    std::mutex g_pushLock;
    ReachCheats::PendingPush g_pushes[8];
    int g_pushCount = 0;

    // Queues a shove of an object away from the direction it's firing in.
    void Push(uint32_t object, const float* dir, float strength) {
        std::lock_guard<std::mutex> lock(g_pushLock);
        ReachCheats::PendingPush* push = nullptr;
        for (int i = 0; i < g_pushCount; ++i)
            if (g_pushes[i].object == object) push = &g_pushes[i];
        if (!push && g_pushCount < _countof(g_pushes)) push = &g_pushes[g_pushCount++], *push = {object, {}};
        if (!push) return;
        for (int k = 0; k < 3; ++k) push->velocity[k] -= dir[k] * strength;
    }
}

uint32_t ReachCheats::RainPick(int kind, uint32_t spawnSelection) {
    if (kind == 5) return spawnSelection;
    std::lock_guard<std::mutex> lock(g_poolsLock);
    const Pools& p = g_pools;
    switch (kind) {
        case 1: return PickFrom(p.vehicles);
        case 2: return PickFrom(p.objects);
        case 3: return PickFrom(p.ai);
        case 4: return PickFrom(p.weapons);
        default: {
            // A kind first, so the long object lists don't drown out the rest.
            const std::vector<uint32_t>* kinds[] = {&p.vehicles, &p.objects, &p.ai, &p.weapons};
            std::vector<uint32_t> which;
            for (uint32_t k = 0; k < 4; ++k)
                if (!kinds[k]->empty()) which.push_back(k);
            if (which.empty()) return 0xFFFFFFFF;
            return PickFrom(*kinds[PickFrom(which)]);
        }
    }
}

std::vector<uint32_t> ReachCheats::WeaponTags() {
    std::lock_guard<std::mutex> lock(g_poolsLock);
    return g_pools.weapons;
}

std::vector<uint32_t> ReachCheats::SpawnableCharacters() {
    std::lock_guard<std::mutex> lock(g_poolsLock);
    return g_pools.ai;
}

void ReachCheats::BeforeShot(void** state, uint32_t weapon, int16_t barrel, Shot& shot) {
    bool chaos = g_chaosGun;
    float push = g_recoilPush;
    if ((!chaos && push <= 0.0f) || !state) return;
    __try {
        Array objects{(char*)state[REACH_TLS_SLOT_OBJECTS]};
        Array players{(char*)state[REACH_TLS_SLOT_PLAYERS]};
        if (!objects.Valid() || !players.Valid()) return;
        char* object = ObjectFromDatum(objects, weapon);
        if (!object) return;
        uint32_t holder = Field<uint32_t>(object, REACH_OBJECT_PARENT);
        if (holder == 0xFFFFFFFF) return;

        // Whose shot: a player's own weapon, or a gun of the vehicle (or
        // turret) a player is in.
        Players p = FindPlayers(players, objects);
        int shooter = -1;
        bool vehicleGun = false;
        for (int i = 0; i < p.count && shooter < 0; ++i) {
            if (p.datums[i] == holder) shooter = i;
            else {
                char* parts[16];
                int count = VehicleParts(p.units[i], objects, parts, _countof(parts));
                for (int k = 0; k < count; ++k)
                    if (parts[k] == ObjectFromDatum(objects, holder)) { shooter = i; vehicleGun = true; break; }
            }
        }
        if (shooter < 0) return;

        if (chaos) {
            char* tag = ReachTags::Get(Field<uint32_t>(object, REACH_OBJECT_TAG_DATUM));
            char* def = ReachTags::BlockElement(tag, REACH_WEAP_BARRELS, REACH_WEAP_BARREL_SIZE, barrel);
            uint32_t pick = PickProjectile();
            if (def && pick != 0xFFFFFFFF && ReachTags::Get(pick)) {
                shot.projectile = &Field<uint32_t>(def, REACH_BARREL_PROJECTILE + REACH_TAGREF_DATUM);
                shot.saved = *shot.projectile;
                *shot.projectile = pick;
            }
        }

        if (push > 0.0f) {
            char* control = (char*)state[REACH_TLS_SLOT_PLAYER_CONTROL] + shooter * REACH_PLAYER_CONTROL_SIZE;
            float yaw = Field<float>(control, REACH_PLAYER_CONTROL_YAW);
            float pitch = Field<float>(control, REACH_PLAYER_CONTROL_PITCH);
            float dir[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
            // Seated, the whole vehicle moves: the top vehicle above the shooter.
            uint32_t target = p.datums[shooter];
            for (int depth = 0; depth < 5; ++depth) {
                char* object = ObjectFromDatum(objects, target);
                uint32_t parent = object ? Field<uint32_t>(object, REACH_OBJECT_PARENT) : 0xFFFFFFFF;
                if (!ObjectOfType(objects, parent, REACH_OBJECT_TYPE_VEHICLE)) break;
                target = parent;
            }
            if (target != p.datums[shooter] || !vehicleGun) Push(target, dir, push);
        }
    } __except (Log::Exception(GetExceptionInformation(), "Reach shot effects")) {
        shot.projectile = nullptr;
    }
}

void ReachCheats::AfterShot(const Shot& shot) {
    if (shot.projectile) *shot.projectile = shot.saved;
}

int ReachCheats::TakePushes(PendingPush* out, int max) {
    std::lock_guard<std::mutex> lock(g_pushLock);
    int count = g_pushCount < max ? g_pushCount : max;
    memcpy(out, g_pushes, count * sizeof(PendingPush));
    g_pushCount = 0;
    return count;
}
