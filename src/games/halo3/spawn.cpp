// Halo 3 spawning: the Spawning page's lists and the spawn key, ported from
// Reach's (see NOTES.md). A ray from player 1's head along their view finds
// the spot; AI are squadless actors made by the engine's own actor_create;
// tags only loaded in some parts of a mission get their designer zone loaded
// first.

#include "spawn.h"

#include "cheats.h"
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "games/fun.h"
#include "tags.h"

using namespace Halo3Cheats;

namespace {
    constexpr float kPi = 3.14159265f;
    constexpr float kSpawnRange = 300.0f;  // world units the spawn ray reaches
    constexpr float kSpawnBackOff = 0.3f;  // back along the ray from the hit, so it isn't inside the surface
    constexpr float kSpawnLift = 0.25f;    // and up, so it doesn't clip into the ground
    constexpr float kVehicleLift = 0.5f;   // vehicles and scenery: above the higher of the hit and the backed-off point
    constexpr ULONGLONG kZoneLoadMs = 5000;  // how long a spawn waits for its designer zone
    constexpr int kMaxSpawnedAi = 512;
    constexpr ULONGLONG kRespawnMs = 5000;   // dead this long: respawned, or left as a body

    using PlacementNewFn = void(__fastcall*)(void* placement, uint32_t tag, uint32_t owner, int64_t);
    using ObjectNewFn = uint32_t(__fastcall*)(void* placement);
    using ObjectDeleteFn = void(__fastcall*)(uint32_t object);
    using HeadPositionFn = void(__fastcall*)(uint32_t unit, float* out);
    using CollisionTestFn = bool(__fastcall*)(uint64_t flags, bool, const float* point, const float* vector,
                                              uint32_t ignore, uint32_t ignore2, int32_t, void* result);
    using ActorCreateFn = uint32_t(__fastcall*)(uint32_t character, int32_t squad, int32_t cell, void* start,
                                                int16_t flags);
    using ActorSetActiveFn = void(__fastcall*)(uint32_t actor, bool active);
    using ActorSetTeamFn = void(__fastcall*)(uint32_t actor, int16_t team);
    using SetPositionFn = void(__fastcall*)(uint32_t object, const float* position, const float* forward,
                                            const float* up, const void* location, bool);
    using SetVelocityFn = void(__fastcall*)(uint32_t object, const float* linear, const float* angular);

    char* g_module = nullptr;

    template <typename Fn>
    Fn Engine(int rva) { return (Fn)(g_module + rva); }

    template <typename Fn>
    void Guard(const char* what, Fn fn) {
        __try {
            fn();
        } __except (Log::Exception(GetExceptionInformation(), what)) {
        }
    }

    // --- Lists (render thread builds, game thread reads the variant table) --

    // A Vehicles-list entry other than a plain tag: a model variant. Its id is
    // kVehicleChoiceId | index (no tag has salt 0xFFFF).
    constexpr uint32_t kVehicleChoiceId = 0xFFFF0000;
    struct VehicleChoice {
        uint32_t vehicle = 0xFFFFFFFF;
        uint32_t variant = 0;  // model variant name (string id), 0 = default
        std::string name;
    };

    std::vector<Games::TagChoice> g_characters;
    std::vector<Games::TagChoice> g_vehicles;
    std::vector<Games::TagChoice> g_objects;
    std::mutex g_listsLock;  // guards g_vehicleChoices
    std::vector<VehicleChoice> g_vehicleChoices;

    bool FindVehicleChoice(uint32_t id, VehicleChoice& out) {
        if ((id & 0xFFFF0000) != kVehicleChoiceId) return false;
        std::lock_guard<std::mutex> lock(g_listsLock);
        size_t index = id & 0xFFFF;
        if (index >= g_vehicleChoices.size()) return false;
        out = g_vehicleChoices[index];
        return true;
    }

    uint32_t PaletteDatum(char* scnr, int palette, int index) {
        char* entry = Halo3Tags::BlockElement(scnr, palette, HALO3_PALETTE_ENTRY_SIZE, index);
        return entry ? Field<uint32_t>(entry, HALO3_PALETTE_ENTRY_DATUM) : 0xFFFFFFFF;
    }

    int PaletteIndex(char* scnr, int palette, uint32_t tag) {
        int count = Halo3Tags::BlockCount(scnr, palette);
        for (int i = 0; i < count && i < 1024; ++i)
            if (PaletteDatum(scnr, palette, i) == tag) return i;
        return -1;
    }

    // A character with a unit somewhere up its parent chain (templates such
    // as "generic" have none and can't be spawned).
    bool Spawnable(uint32_t character) {
        __try {
            for (int depth = 0; depth < 8 && character != 0xFFFFFFFF; ++depth) {
                char* tag = Halo3Tags::Get(character);
                if (!tag) return false;
                if (Field<uint32_t>(tag, HALO3_CHAR_UNIT) != 0xFFFFFFFF) return true;
                character = Field<uint32_t>(tag, HALO3_CHAR_PARENT);
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Vehicles: the mission's vehicle palette (other 'vehi' tags are turrets
    // and parts), each followed by its model's other variants ("warthog
    // (gauss)"). Wrecked, damaged and cinematic ones are left out.
    void BuildVehicleList(char* scnr) {
        constexpr const char* kSkip[] = {"default", "damaged", "wrecked", "cinematic", "prop", "filler"};
        std::vector<Games::TagChoice> list;
        std::vector<VehicleChoice> choices;
        for (auto& vehicle : Halo3Tags::ListGroup(HALO3_TAG_VEHI)) {
            if (PaletteIndex(scnr, HALO3_SCNR_VEHICLE_PALETTE, vehicle.datum) < 0) continue;
            list.push_back(vehicle);
            char* tag = Halo3Tags::Get(vehicle.datum);
            char* model = tag ? Halo3Tags::Get(Field<uint32_t>(tag, HALO3_OBJE_MODEL)) : nullptr;
            uint32_t defaultName = tag ? Field<uint32_t>(tag, HALO3_OBJE_DEFAULT_VARIANT) : 0;
            int count = Halo3Tags::BlockCount(model, HALO3_HLMT_VARIANTS);
            for (int v = 0; v < count && v < 64; ++v) {
                char* variant = Halo3Tags::BlockElement(model, HALO3_HLMT_VARIANTS, HALO3_HLMT_VARIANT_SIZE, v);
                uint32_t name = variant ? Field<uint32_t>(variant, 0) : 0;
                std::string label = Halo3Tags::StringName(name);
                bool skip = !name || name == defaultName || label.empty();
                for (const char* s : kSkip) skip |= label.find(s) != std::string::npos;
                if (skip) continue;
                VehicleChoice c;
                c.vehicle = vehicle.datum;
                c.variant = name;
                c.name = vehicle.name + " (" + label + ")";
                list.push_back({kVehicleChoiceId | (uint32_t)choices.size(), c.name});
                choices.push_back(c);
            }
        }
        g_vehicles = list;
        std::lock_guard<std::mutex> lock(g_listsLock);
        g_vehicleChoices = choices;
    }

    // Objects: the mission's crate, equipment, machine, control and scenery
    // palettes, prefixed by kind and sorted within each. Editing helpers are
    // left out by name.
    void BuildObjectList(char* scnr) {
        struct Kind { int palette; const char* prefix; };
        constexpr Kind kKinds[] = {{HALO3_SCNR_CRATE_PALETTE, "crate"},
                                   {HALO3_SCNR_EQUIPMENT_PALETTE, "equipment"},
                                   {HALO3_SCNR_MACHINE_PALETTE, "machine"},
                                   {HALO3_SCNR_CONTROL_PALETTE, "control"},
                                   {HALO3_SCNR_SCENERY_PALETTE, "scenery"}};
        constexpr const char* kHelpers[] = {"invisible", "pathfinding", "ui_object", "fx_", "standin", "kill_"};
        g_objects.clear();
        for (const Kind& kind : kKinds) {
            std::vector<Games::TagChoice> found;
            int count = Halo3Tags::BlockCount(scnr, kind.palette);
            for (int i = 0; i < count && i < 1024; ++i) {
                uint32_t tag = PaletteDatum(scnr, kind.palette, i);
                if (tag == 0xFFFFFFFF || !Halo3Tags::Get(tag)) continue;
                std::string name = Halo3Tags::FullName(tag);
                name = name.substr(name.find_last_of('\\') + 1);
                bool helper = name.empty();
                for (const char* h : kHelpers) helper |= name.find(h) != std::string::npos;
                bool dup = false;
                for (auto& f : found) dup |= f.datum == tag;
                if (!helper && !dup) found.push_back({tag, std::string(kind.prefix) + ": " + name});
            }
            std::sort(found.begin(), found.end(),
                      [](const Games::TagChoice& a, const Games::TagChoice& b) { return a.name < b.name; });
            g_objects.insert(g_objects.end(), found.begin(), found.end());
        }
    }

    // --- Results for the menu -------------------------------------------------

    std::mutex g_resultLock;
    uint32_t g_spawnDone = 0;
    char g_spawnMessage[96] = "";

    void SpawnResult(const char* text) {
        std::lock_guard<std::mutex> lock(g_resultLock);
        sprintf_s(g_spawnMessage, "%s", text);
        ++g_spawnDone;
        LOG("Halo 3: spawn: %s", text);
    }

    std::string ShortName(uint32_t tag) {
        std::string name = Halo3Tags::FullName(tag);
        name = name.substr(name.find_last_of('\\') + 1);
        return name.empty() ? "selection" : name;
    }

    VehicleChoice ChoiceFor(uint32_t selection) {
        VehicleChoice c;
        if (!FindVehicleChoice(selection, c)) c.vehicle = selection;
        return c;
    }

    std::string SelectionName(uint32_t selection) {
        VehicleChoice c = ChoiceFor(selection);
        return c.name.empty() ? ShortName(c.vehicle) : c.name;
    }

    // --- Designer zones (as in Reach) --------------------------------------------
    // A tag the mission only loads in some parts of the level is in one or more
    // designer zones. When a spawn fails and its zone isn't loaded, the mod
    // loads it (pending masks + flag, which the game's zone update applies)
    // and retries each frame. Zones it loaded are kept for the rest of the map.

    uint32_t g_forcedZones = 0;
    char* g_forcedScenario = nullptr;

    uint32_t& ActiveZones() { return *(uint32_t*)(g_module + OFFSET_HALO3_DESIGNER_ZONES_ACTIVE); }

    uint32_t DesignerZonesOf(char* scnr, uint32_t tag) {
        int list = 0, palette = 0;
        switch (Halo3Tags::GroupOf(tag)) {
            case HALO3_TAG_CHAR: list = HALO3_DESIGNER_ZONE_CHARACTERS; palette = HALO3_SCNR_CHARACTER_PALETTE; break;
            case HALO3_TAG_WEAP: list = HALO3_DESIGNER_ZONE_WEAPONS; palette = HALO3_SCNR_WEAPON_PALETTE; break;
            case HALO3_TAG_VEHI: list = HALO3_DESIGNER_ZONE_VEHICLES; palette = HALO3_SCNR_VEHICLE_PALETTE; break;
            case HALO3_TAG_BIPD: list = HALO3_DESIGNER_ZONE_BIPEDS; palette = HALO3_SCNR_BIPED_PALETTE; break;
            case HALO3_TAG_EQIP: list = HALO3_DESIGNER_ZONE_EQUIPMENT; palette = HALO3_SCNR_EQUIPMENT_PALETTE; break;
            case HALO3_TAG_SCEN: list = HALO3_DESIGNER_ZONE_SCENERY; palette = HALO3_SCNR_SCENERY_PALETTE; break;
            case HALO3_TAG_MACH: list = HALO3_DESIGNER_ZONE_MACHINES; palette = HALO3_SCNR_MACHINE_PALETTE; break;
            case HALO3_TAG_CTRL: list = HALO3_DESIGNER_ZONE_CONTROLS; palette = HALO3_SCNR_CONTROL_PALETTE; break;
            case HALO3_TAG_BLOC: list = HALO3_DESIGNER_ZONE_CRATES; palette = HALO3_SCNR_CRATE_PALETTE; break;
            default: return 0;
        }
        int index = PaletteIndex(scnr, palette, tag);
        if (index < 0) return 0;
        uint32_t zones = 0;
        int count = Halo3Tags::BlockCount(scnr, HALO3_SCNR_DESIGNER_ZONES);
        for (int z = 0; z < count && z < 32; ++z) {
            char* zone = Halo3Tags::BlockElement(scnr, HALO3_SCNR_DESIGNER_ZONES, HALO3_SCNR_DESIGNER_ZONE_SIZE, z);
            int entries = Halo3Tags::BlockCount(zone, list);
            for (int e = 0; e < entries && e < 256; ++e) {
                char* entry = Halo3Tags::BlockElement(zone, list, 2, e);
                if (entry && Field<int16_t>(entry, 0) == index) zones |= 1u << z;
            }
        }
        return zones;
    }

    // The zones to load for a tag that failed to spawn, or 0 if it's in no
    // designer zone or one is already loaded.
    uint32_t ZoneToLoad(char* scnr, uint32_t tag) {
        uint32_t zones = DesignerZonesOf(scnr, tag);
        if (!zones || (zones & ActiveZones())) return 0;
        return zones & (0u - zones);  // the first one is enough
    }

    void LoadZones(uint32_t zones) {
        g_forcedZones |= zones;
        uint32_t missing = zones & ~ActiveZones();
        if (!missing) return;
        uint32_t* pending = (uint32_t*)(g_module + OFFSET_HALO3_DESIGNER_ZONES_PENDING);
        pending[0] &= ~missing;  // deactivate
        pending[1] |= missing;   // activate
        *(uint8_t*)(g_module + OFFSET_HALO3_ZONE_UPDATE_PENDING) = 1;
        LOG("Halo 3: loading designer zones %08X (active %08X)", missing, ActiveZones());
    }

    // Keeps the zones the mod loaded: zone sets no longer unload them (a tag
    // edit), and they're loaded again if the active mask loses them.
    void KeepForcedZones(char* scnr) {
        if (scnr != g_forcedScenario) {
            g_forcedScenario = scnr;
            g_forcedZones = 0;
        }
        if (!g_forcedZones || !scnr) return;
        int count = Halo3Tags::BlockCount(scnr, HALO3_SCNR_ZONE_SETS);
        for (int i = 0; i < count && i < 64; ++i)
            if (char* set = Halo3Tags::BlockElement(scnr, HALO3_SCNR_ZONE_SETS, HALO3_SCNR_ZONE_SET_SIZE, i))
                Field<uint32_t>(set, HALO3_ZONE_SET_FORBIDDEN) &= ~g_forcedZones;
        if (g_forcedZones & ~ActiveZones()) {
            LOG("Halo 3: designer zones %08X were unloaded; loading them again", g_forcedZones & ~ActiveZones());
            LoadZones(g_forcedZones);
        }
    }

    // --- Spawning ------------------------------------------------------------------

    // AI the mod spawned, followed after death for "Enable respawn".
    struct SpawnedAi {
        uint32_t unit = 0xFFFFFFFF;
        uint32_t character = 0xFFFFFFFF;
        int team = 0;
        ULONGLONG deadAt = 0;  // when it was first seen dead, 0 = alive
    };
    SpawnedAi g_spawnedAi[kMaxSpawnedAi];

    // A spawn waiting for its designer zone to load.
    struct ZoneWait {
        bool active = false;
        uint32_t selection = 0xFFFFFFFF;
        uint32_t zone = 0;
        ULONGLONG deadline = 0;
        int team = 0;
        float position[3] = {};
        float yaw = 0.0f;
    };
    ZoneWait g_wait;
    uint32_t g_spawnSerial = 0;
    bool g_haveSerial = false;

    // Halo 3 characters carry no loadout: squads arm their AI from the
    // scenario's weapon palette (a starting location's pick, else its fire
    // team's). A spawned character gets one of the loadouts the mission gives
    // it, at random (weighted by how often), or its parent's; else unarmed.
    void PickLoadout(char* scnr, uint32_t character, char* start);
    // When no squad of the mission arms a character (010_jungle has no armed brutes at all) its
    // spawn would be unarmed. Falls back to weapons that suit its kind, from those the mission's
    // weapon palette holds (a start location can only name palette entries). Returns true if set.
    bool FallbackLoadout(char* scnr, uint32_t character, char* start) {
        std::string name = ShortName(character);
        struct Rule { const char* prefix; std::vector<const char*> weapons; };
        static const Rule kRules[] = {
            {"brute_chieftain", {"gravity_hammer", "brute_shot"}},
            {"brute", {"spike_rifle", "brute_shot", "plasma_rifle", "needler"}},
            {"grunt", {"plasma_pistol", "needler", "plasma_rifle"}},
            {"jackal_sniper", {"beam_rifle"}},
            {"jackal", {"plasma_pistol", "covenant_carbine"}},
            {"elite", {"plasma_rifle", "covenant_carbine", "needler", "energy_blade"}},
            {"marine", {"battle_rifle", "assault_rifle", "smg", "magnum"}},
        };
        const Rule* rule = nullptr;
        for (const Rule& r : kRules)
            if (name.rfind(r.prefix, 0) == 0) { rule = &r; break; }
        if (!rule) return false;
        std::vector<int> options;
        for (auto& weapon : Halo3Tags::ListGroup(HALO3_TAG_WEAP))
            for (const char* w : rule->weapons)
                if (weapon.name == w) {
                    int index = PaletteIndex(scnr, HALO3_SCNR_WEAPON_PALETTE, weapon.datum);
                    if (index >= 0) options.push_back(index);
                }
        if (options.empty()) return false;
        Field<int16_t>(start, HALO3_START_WEAPON) = (int16_t)options[GetTickCount64() % options.size()];
        Field<int16_t>(start, HALO3_START_SECONDARY_WEAPON) = -1;
        LOG("Halo 3: no squad arms %s; gave it a fallback weapon", name.c_str());
        return true;
    }

    void PickLoadout(char* scnr, uint32_t character, char* start) {
        const uint32_t original = character;
        int16_t found[512][2];
        int count = 0;
        int squads = Halo3Tags::BlockCount(scnr, HALO3_SCNR_SQUADS);
        for (int depth = 0; depth < 8 && count == 0 && character != 0xFFFFFFFF; ++depth) {
            int index = PaletteIndex(scnr, HALO3_SCNR_CHARACTER_PALETTE, character);
            for (int i = 0; index >= 0 && i < squads && i < 1024 && count < 512; ++i) {
                char* squad = Halo3Tags::BlockElement(scnr, HALO3_SCNR_SQUADS, HALO3_SQUAD_SIZE, i);
                int teams = Halo3Tags::BlockCount(squad, HALO3_SQUAD_FIRE_TEAMS);
                for (int f = 0; f < teams && f < 64 && count < 512; ++f) {
                    char* team = Halo3Tags::BlockElement(squad, HALO3_SQUAD_FIRE_TEAMS, HALO3_FIRE_TEAM_SIZE, f);
                    if (!team) continue;
                    int starts = Halo3Tags::BlockCount(team, HALO3_FIRE_TEAM_STARTS);
                    for (int k = 0; k < starts && k < 64 && count < 512; ++k) {
                        char* st = Halo3Tags::BlockElement(team, HALO3_FIRE_TEAM_STARTS, HALO3_START_SIZE, k);
                        if (!st) continue;
                        int16_t who = Field<int16_t>(st, HALO3_START_CHARACTER);
                        if (who < 0) who = Field<int16_t>(team, HALO3_FIRE_TEAM_CHARACTER);
                        if (who != index) continue;
                        int16_t weapon = Field<int16_t>(st, HALO3_START_WEAPON);
                        int16_t secondary = Field<int16_t>(st, HALO3_START_SECONDARY_WEAPON);
                        if (weapon < 0) weapon = Field<int16_t>(team, HALO3_FIRE_TEAM_WEAPON);
                        if (secondary < 0) secondary = Field<int16_t>(team, HALO3_FIRE_TEAM_SECONDARY);
                        if (weapon < 0) continue;  // e.g. vehicle crews
                        found[count][0] = weapon;
                        found[count][1] = secondary;
                        ++count;
                    }
                }
            }
            if (count == 0) {
                char* tag = Halo3Tags::Get(character);
                character = tag ? Field<uint32_t>(tag, HALO3_CHAR_PARENT) : 0xFFFFFFFF;
            }
        }
        if (count == 0) {
            if (FallbackLoadout(scnr, original, start)) return;
            LOG("Halo 3: no squad in this mission arms this character; spawning it unarmed");
            return;
        }
        int pick = (int)(GetTickCount64() % (ULONGLONG)count);
        Field<int16_t>(start, HALO3_START_WEAPON) = found[pick][0];
        Field<int16_t>(start, HALO3_START_SECONDARY_WEAPON) = found[pick][1];
    }

    // --- Team picker (Settings::spawnTeam) ------------------------------------
    // 0 their own team, 1 Ally, 2 Covenant (the enemy), 3 Flood, 4 Rogue, 5 Random.
    // Halo 3's ai team enum (Assembly scnr.xml squad Team): 1 player, 2 human,
    // 3 covenant, 4 flood, 5 sentinel, 6 heretic, 7 prophet, 8 guilty, 9-15 unused.
    // In campaign two teams are friends only if bit b of the allegiance's friends[a]
    // is set (game_team_is_friendly, RVA 0x199B08: no implicit "same team", so every
    // team's own bit is in the matrix). Static only; the matrix layout is the one
    // ReadAllies already uses.
    constexpr int kRogueTeams[] = {9, 10, 11, 12, 13, 14, 15};  // unused teams; nobody is allied with them
    constexpr int kActorTeam = 0x20;  // int16 (actor_set_team writes it, RVA 0x55E6A6)
    constexpr int kRandomTeams[] = {HALO3_TEAM_HUMAN, HALO3_TEAM_COVENANT, 4 /* flood */, 5 /* sentinel */, 9, 10};

    uint32_t g_random = 0x2545F491u;  // xorshift
    float Random01() {
        g_random ^= g_random << 13;
        g_random ^= g_random >> 17;
        g_random ^= g_random << 5;
        return (g_random >> 8) / 16777216.0f;
    }

    // Rogue teams: no friends, not even themselves, so rogues fight everyone
    // including each other (CE's MakeRogueFightItself, same idea). Cleared again
    // every time because mission scripts may rewrite the matrix.
    void MakeRoguesHostile(char* allegiance) {
        for (int r : kRogueTeams) {
            Field<uint32_t>(allegiance, HALO3_ALLEGIANCE_FRIENDS + r * 4) = 0;
            for (int t = 0; t < HALO3_TEAM_COUNT; ++t)
                Field<uint32_t>(allegiance, HALO3_ALLEGIANCE_FRIENDS + t * 4) &= ~(1u << r);
        }
    }

    bool IsRogueTeam(int team) { return team >= kRogueTeams[0] && team <= kRogueTeams[_countof(kRogueTeams) - 1]; }

    // Live actors per team (index = team, 0..15), read off the actor array.
    void CountActorTeams(void** state, int* counts) {
        for (int t = 0; t < HALO3_TEAM_COUNT; ++t) counts[t] = 0;
        auto actors = (Blam::DataArray*)state[HALO3_TLS_SLOT_ACTORS];
        if (!actors) return;
        for (int i = 0; i < actors->highWater && i < 4096; ++i) {
            char* actor = actors->Entry(i);
            if (!*(uint16_t*)actor) continue;
            int16_t team = Field<int16_t>(actor, kActorTeam);
            if (team >= 0 && team < HALO3_TEAM_COUNT) ++counts[team];
        }
    }

    // Rogue = every man for himself: each rogue wants a team nobody else is on (AI on
    // one team are never hostile to each other), so take the unused rogue team with
    // the fewest AI on it, ties at random. Beyond seven rogues teams start to be shared.
    int FreeForAllTeam(void** state) {
        int counts[HALO3_TEAM_COUNT];
        CountActorTeams(state, counts);
        int best = -1, bestCount = 1 << 30, ties = 0;
        for (int t : kRogueTeams) {
            if (counts[t] < bestCount) { best = t; bestCount = counts[t]; ties = 1; }
            else if (counts[t] == bestCount && Random01() < 1.0f / ++ties) best = t;
        }
        return best;
    }

    // Random = a pick among the teams that exist in this level right now (teams that
    // have a live actor, the rogue teams the mod hands out left out); if none, the
    // classic factions.
    int RandomLevelTeam(void** state) {
        int counts[HALO3_TEAM_COUNT];
        CountActorTeams(state, counts);
        int teams[HALO3_TEAM_COUNT], n = 0;
        for (int t = 1; t < HALO3_TEAM_COUNT; ++t)
            if (counts[t] > 0 && !IsRogueTeam(t)) teams[n++] = t;
        if (n == 0) return kRandomTeams[(int)(Random01() * _countof(kRandomTeams)) % (int)_countof(kRandomTeams)];
        int team = teams[(int)(Random01() * n) % n];
        return team;
    }

    // The Covenant team a mission's enemies are on isn't always 3: on 070_waste the
    // brutes, grunts, jackals and hunters are team 7 (prophet) and team 3 is allied
    // to the player. Picks the first of covenant/prophet/heretic that player 1's
    // team isn't friends with; failing that (the player is Covenant) the humans.
    int HostileCovenantTeam(char* allegiance) {
        if (!allegiance) return HALO3_TEAM_COVENANT;
        uint32_t friends = Field<uint32_t>(allegiance, HALO3_ALLEGIANCE_FRIENDS + HALO3_TEAM_PLAYER * 4);
        for (int t : {HALO3_TEAM_COVENANT, 7, 6})
            if (!(friends & (1u << t))) return t;
        return !(friends & (1u << HALO3_TEAM_HUMAN)) ? HALO3_TEAM_HUMAN : HALO3_TEAM_COVENANT;
    }

    // The engine team for a picker choice, or 0 to leave the AI on its own.
    // Random and Rogue roll per call, so a respawn rolls again.
    int ResolveSpawnTeam(void** state, int choice) {
        char* allegiance = (char*)state[HALO3_TLS_SLOT_ALLEGIANCE];
        int team = 0;
        switch (choice) {
        case 1: {  // Ally: whichever of human/covenant/flood is friends with player 1's team
            team = HALO3_TEAM_HUMAN;
            if (allegiance) {
                uint32_t friends = Field<uint32_t>(allegiance, HALO3_ALLEGIANCE_FRIENDS + HALO3_TEAM_PLAYER * 4);
                for (int t : {HALO3_TEAM_HUMAN, HALO3_TEAM_COVENANT, 4})
                    if (friends & (1u << t)) { team = t; break; }
            }
            return team;
        }
        case 2: return HostileCovenantTeam(allegiance);
        case 3: return 4;  // flood
        case 4: team = FreeForAllTeam(state); break;
        case 5: team = RandomLevelTeam(state); break;
        default: return 0;
        }
        if (allegiance && team >= kRogueTeams[0]) MakeRoguesHostile(allegiance);
        return team;
    }

    // actor_set_team (RVA 0x55E6A0) on an active actor only rewrites the actor's
    // team and its perception props; the unit's own team (+0x174) is written
    // only for inactive actors. A spawned AI is activated first, so its unit kept
    // the starting team and other AI kept treating it as a friend. Write both.
    void** g_lastState = nullptr;
    void SetTeamBoth(void** state, uint32_t actor, int16_t team) {
        Engine<ActorSetTeamFn>(OFFSET_HALO3_ACTOR_SET_TEAM)(actor, team);
        auto actors = (Blam::DataArray*)state[HALO3_TLS_SLOT_ACTORS];
        auto objects = (Blam::DataArray*)state[HALO3_TLS_SLOT_OBJECTS];
        uint16_t index = actor & 0xFFFF;
        if (!actors || !objects || index >= actors->highWater) return;
        char* a = actors->Entry(index);
        if (*(uint16_t*)a != (actor >> 16)) return;
        if (char* unit = Blam::ObjectFromDatum(objects, Field<uint32_t>(a, HALO3_ACTOR_UNIT)))
            Field<int16_t>(unit, HALO3_UNIT_TEAM) = team;
    }

    // An AI as a squadless actor, with a starting location built like the
    // game's own squadless spawns (0x5BF004). Returns the actor or -1.
    uint32_t SpawnActor(void** state, char* scnr, uint32_t character, const float* position, float yaw, int team) {
        alignas(16) static uint8_t start[0x100];
        char* s = (char*)start;
        memset(start, 0, sizeof(start));
        memcpy(s + HALO3_START_POSITION, position, 3 * sizeof(float));
        Field<int16_t>(s, HALO3_START_REFERENCE_FRAME) = -1;
        Field<int16_t>(s, HALO3_START_BSP) = -1;
        Field<float>(s, HALO3_START_YAW) = yaw;
        Field<uint32_t>(s, HALO3_START_FLAGS) = HALO3_START_FLAGS_SQUADLESS;
        Field<int16_t>(s, HALO3_START_CHARACTER) = -1;
        Field<int16_t>(s, HALO3_START_WEAPON) = -1;
        Field<int16_t>(s, HALO3_START_SECONDARY_WEAPON) = -1;
        Field<int16_t>(s, HALO3_START_WEAPON + 4) = -1;  // unknown, -1 like its neighbours
        Field<int16_t>(s, HALO3_START_VEHICLE) = -1;
        Field<int16_t>(s, HALO3_START_SEAT) = -1;
        Field<int16_t>(s, HALO3_START_GRENADES) = -1;
        Field<int16_t>(s, HALO3_START_SWARM_COUNT) = 1;
        for (int k = 0; k < 3; ++k) Field<int16_t>(s, HALO3_START_EMITTER + 2 * k) = -1;
        Field<int16_t>(s, HALO3_START_SCRIPT_INDEX) = -1;
        Field<int16_t>(s, HALO3_START_EQUIPMENT) = -1;
        Field<int16_t>(s, HALO3_START_POINT_SET) = -1;
        PickLoadout(scnr, character, s);
        uint32_t actor = Engine<ActorCreateFn>(OFFSET_HALO3_ACTOR_CREATE)(character, -1, -1, start,
                                                                         HALO3_ACTOR_CREATE_FLAGS);
        if (actor == 0xFFFFFFFF) return actor;
        Engine<ActorSetActiveFn>(OFFSET_HALO3_ACTOR_SET_ACTIVE)(actor, true);
        int engineTeam = ResolveSpawnTeam(state, team);
        g_lastState = state;
        if (engineTeam > 0) SetTeamBoth(state, actor, (int16_t)engineTeam);
        return actor;
    }

    void Follow(void** state, Blam::DataArray* objects, uint32_t actorDatum, uint32_t character, int team) {
        auto actors = (Blam::DataArray*)state[HALO3_TLS_SLOT_ACTORS];
        uint16_t index = actorDatum & 0xFFFF;
        if (!actors || index >= actors->highWater) return;
        char* actor = actors->Entry(index);
        if (*(uint16_t*)actor != (actorDatum >> 16)) return;
        uint32_t unit = Field<uint32_t>(actor, HALO3_ACTOR_UNIT);
        if (!Blam::ObjectFromDatum(objects, unit)) {
            LOG("Halo 3: spawned actor %08X has no unit; not followed", actorDatum);
            return;
        }
        SpawnedAi* slot = nullptr;  // a free slot, else the body dead longest
        for (auto& ai : g_spawnedAi) {
            if (ai.unit == 0xFFFFFFFF || !Blam::ObjectFromDatum(objects, ai.unit)) { slot = &ai; break; }
            if (ai.deadAt && (!slot || ai.deadAt < slot->deadAt)) slot = &ai;
        }
        if (slot) *slot = {unit, character, team, 0};
    }

    uint32_t NewObject(uint32_t tag, uint32_t variant, const float* position, float yaw) {
        static uint8_t placement[HALO3_PLACEMENT_DATA_SIZE];
        memset(placement, 0, sizeof(placement));
        Engine<PlacementNewFn>(OFFSET_HALO3_OBJECT_PLACEMENT_NEW)(placement, tag, 0xFFFFFFFF, 0);
        if (variant) Field<uint32_t>((char*)placement, HALO3_PLACEMENT_VARIANT) = variant;
        memcpy(placement + HALO3_PLACEMENT_POSITION, position, 3 * sizeof(float));
        float* forward = (float*)(placement + HALO3_PLACEMENT_FORWARD);
        forward[0] = cosf(yaw);
        forward[1] = sinf(yaw);
        forward[2] = 0.0f;
        return Engine<ObjectNewFn>(OFFSET_HALO3_OBJECT_NEW)(placement);
    }

    // Makes a selection at a position: an AI (followed) or an object.
    uint32_t Create(void** state, Blam::DataArray* objects, char* scnr, uint32_t selection, int team,
                    const float* position, float yaw) {
        VehicleChoice c = ChoiceFor(selection);
        if (Halo3Tags::GroupOf(c.vehicle) != HALO3_TAG_CHAR) return NewObject(c.vehicle, c.variant, position, yaw);
        uint32_t actor = SpawnActor(state, scnr, c.vehicle, position, yaw, team);
        if (actor != 0xFFFFFFFF) Follow(state, objects, actor, c.vehicle, team);
        return actor;
    }

    void Spawn(void** state, const Players& p, Blam::DataArray* objects, uint32_t selection, int team) {
        uint32_t tag = ChoiceFor(selection).vehicle;
        char* scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
        if (p.count == 0) { SpawnResult("No player to spawn for"); return; }
        if (!scnr || !Halo3Tags::Get(tag)) { SpawnResult("Selection isn't in this map"); return; }
        g_wait = {};  // replaced by this newer request

        char* control = (char*)state[HALO3_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
        if (!control) { SpawnResult("No player to spawn for"); return; }
        float yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
        float pitch = Field<float>(control, HALO3_PLAYER_CONTROL_PITCH);
        float dir[3] = {cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch)};
        float eye[3];
        Engine<HeadPositionFn>(OFFSET_HALO3_UNIT_HEAD_POSITION)(p.datums[0], eye);
        float ray[3] = {dir[0] * kSpawnRange, dir[1] * kSpawnRange, dir[2] * kSpawnRange};

        alignas(16) static uint8_t result[HALO3_COLLISION_RESULT_SIZE];
        memset(result, 0, sizeof(result));
        uint64_t flags = *(uint64_t*)(g_module + OFFSET_HALO3_COLLISION_FLAGS);
        uint32_t vehicle = Field<uint32_t>(p.units[0], HALO3_OBJECT_PARENT);
        bool hit = Engine<CollisionTestFn>(OFFSET_HALO3_COLLISION_TEST_VECTOR)(flags, false, eye, ray, p.datums[0],
                                                                              vehicle, -1, result);
        float fraction = Field<float>((char*)result, HALO3_COLLISION_RESULT_FRACTION);
        if (!hit || !(fraction >= 0.0f && fraction <= 1.0f)) { SpawnResult("Nothing in range to spawn on"); return; }
        float at[3] = {eye[0] + ray[0] * fraction, eye[1] + ray[1] * fraction, eye[2] + ray[2] * fraction};
        float position[3] = {at[0] - dir[0] * kSpawnBackOff, at[1] - dir[1] * kSpawnBackOff,
                             at[2] - dir[2] * kSpawnBackOff + kSpawnLift};
        float facing = yaw;
        uint32_t group = Halo3Tags::GroupOf(tag);
        if (group == HALO3_TAG_VEHI || group == HALO3_TAG_SCEN || group == HALO3_TAG_MACH ||
            group == HALO3_TAG_CTRL || group == HALO3_TAG_BLOC) {
            // Backed off by its size so it isn't in a wall, and kept off the floor.
            float radius = Field<float>(Halo3Tags::Get(tag), HALO3_OBJE_BOUNDING_RADIUS);
            if (!(radius > 0.0f && radius < 100.0f)) radius = 2.0f;
            for (int k = 0; k < 3; ++k) position[k] = at[k] - dir[k] * radius;
            position[2] = (position[2] > at[2] ? position[2] : at[2]) + kVehicleLift;
        } else if (group == HALO3_TAG_CHAR) {
            facing = remainderf(yaw + kPi, 2.0f * kPi);  // AI face the player
        }

        uint32_t made = Create(state, objects, scnr, selection, team, position, facing);
        LOG("Halo 3: spawn %08X at %.2f %.2f %.2f (hit %.2f %.2f %.2f, fraction %.3f) -> object/actor %08X", tag,
            position[0], position[1], position[2], at[0], at[1], at[2], fraction, made);
        std::string name = SelectionName(selection);
        char text[96];
        uint32_t zone = made == 0xFFFFFFFF ? ZoneToLoad(scnr, tag) : 0;
        if (zone) {
            LoadZones(zone);
            g_wait.active = true;
            g_wait.selection = selection;
            g_wait.zone = zone;
            g_wait.deadline = GetTickCount64() + kZoneLoadMs;
            g_wait.team = team;
            memcpy(g_wait.position, position, sizeof(position));
            g_wait.yaw = facing;
            sprintf_s(text, "Loading %s...", name.c_str());
        } else if (made == 0xFFFFFFFF) {
            sprintf_s(text, "%s: not loaded in this area", name.c_str());
        } else {
            sprintf_s(text, "Spawned %s", name.c_str());
        }
        SpawnResult(text);
    }

    // Retries a spawn each frame once its zone is loaded (its resources may
    // take a few more frames), until it works or times out.
    void ContinueZoneWait(void** state, Blam::DataArray* objects, char* scnr) {
        if (!g_wait.active) return;
        ULONGLONG now = GetTickCount64();
        char text[96];
        if (ActiveZones() & g_wait.zone) {
            uint32_t made = Create(state, objects, scnr, g_wait.selection, g_wait.team, g_wait.position, g_wait.yaw);
            if (made != 0xFFFFFFFF) {
                LOG("Halo 3: spawn %08X: made after its zone loaded -> %08X", g_wait.selection, made);
                sprintf_s(text, "Spawned %s", SelectionName(g_wait.selection).c_str());
                SpawnResult(text);
                g_wait = {};
                return;
            }
        }
        if (now > g_wait.deadline) {
            sprintf_s(text, "Couldn't spawn %s here", SelectionName(g_wait.selection).c_str());
            SpawnResult(text);
            g_wait = {};
        }
    }

    // Spawned AI after death. Halo 3 doesn't revive them, so "Enable
    // respawn" is the mod's own, read once per death: on, an AI dead for
    // kRespawnMs is replaced by a fresh one of the same character and team
    // where it lay (the body is removed); off, it stays a body.
    void FollowSpawnedAi(void** state, const Players& p, Blam::DataArray* objects, char* scnr, bool respawn) {
        ULONGLONG now = GetTickCount64();
        for (auto& ai : g_spawnedAi) {
            if (ai.unit == 0xFFFFFFFF) continue;
            char* unit = Blam::ObjectFromDatum(objects, ai.unit);
            if (!unit) { ai = {}; continue; }
            if (Field<float>(unit, HALO3_OBJECT_HEALTH) > 0.0f) { ai.deadAt = 0; continue; }
            if (!ai.deadAt) {
                ai.deadAt = now;
                LOG("Halo 3: spawned AI unit %08X died", ai.unit);
                continue;
            }
            if (now - ai.deadAt < kRespawnMs) continue;
            SpawnedAi old = ai;
            ai = {};
            if (!respawn || !Halo3Tags::Get(old.character)) {
                LOG("Halo 3: spawned AI unit %08X left as a body (respawn off)", old.unit);
                continue;
            }
            float position[3];
            memcpy(position, &Field<float>(unit, HALO3_OBJECT_POSITION), sizeof(position));
            position[2] += kSpawnLift;
            float yaw = 0.0f;
            if (p.count > 0) {
                const float* to = &Field<float>(p.units[0], HALO3_OBJECT_POSITION);
                yaw = atan2f(to[1] - position[1], to[0] - position[0]);
            }
            Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(old.unit);
            uint32_t actor = SpawnActor(state, scnr, old.character, position, yaw, old.team);
            if (actor != 0xFFFFFFFF) Follow(state, objects, actor, old.character, old.team);
            LOG("Halo 3: spawned AI respawned: old unit %08X -> actor %08X", old.unit, actor);
        }
    }
}

namespace {
    // The engine's ray from `from` along `vec`, ignoring player 1 and their
    // vehicle if asked. Writes the hit point.
    bool CastRay(const Players& p, const float* from, const float* vec, bool ignorePlayer, float* hit) {
        alignas(16) static uint8_t result[HALO3_COLLISION_RESULT_SIZE];
        memset(result, 0, sizeof(result));
        uint64_t flags = *(uint64_t*)(g_module + OFFSET_HALO3_COLLISION_FLAGS);
        uint32_t ignore = 0xFFFFFFFF, ignore2 = 0xFFFFFFFF;
        if (ignorePlayer && p.count > 0) {
            ignore = p.datums[0];
            ignore2 = Field<uint32_t>(p.units[0], HALO3_OBJECT_PARENT);
        }
        if (!Engine<CollisionTestFn>(OFFSET_HALO3_COLLISION_TEST_VECTOR)(flags, false, from, vec, ignore, ignore2, -1,
                                                                        result))
            return false;
        float fraction = Field<float>((char*)result, HALO3_COLLISION_RESULT_FRACTION);
        if (!(fraction >= 0.0f && fraction <= 1.0f)) return false;
        for (int k = 0; k < 3; ++k) hit[k] = from[k] + vec[k] * fraction;
        return true;
    }

    char* ObjectOfType(Blam::DataArray* objects, uint32_t datum, int type) {
        char* object = Blam::ObjectFromDatum(objects, datum);
        return object && Field<uint8_t>(object, HALO3_OBJECT_TYPE) == type ? object : nullptr;
    }

    // The engine work behind the Fun, Chaos and Skulls pages (games/fun.h).
    class FunTitle : public Fun::Title {
    public:
        FunTitle(void** state, const Players& p, Blam::DataArray* objects) : m_state(state), m_p(p), m_objects(objects) {}
        const char* Name() override { return "Halo 3"; }
        uint32_t Player() override { return m_p.count > 0 ? m_p.datums[0] : 0xFFFFFFFF; }
        uint32_t PlayerVehicle() override {
            if (m_p.count == 0) return 0xFFFFFFFF;
            uint32_t top = 0xFFFFFFFF, at = m_p.datums[0];
            for (int depth = 0; depth < 5; ++depth) {
                char* object = Blam::ObjectFromDatum(m_objects, at);
                uint32_t parent = object ? Field<uint32_t>(object, HALO3_OBJECT_PARENT) : 0xFFFFFFFF;
                if (!ObjectOfType(m_objects, parent, HALO3_OBJECT_TYPE_VEHICLE)) break;
                top = at = parent;
            }
            return top;
        }
        bool View(float* eye, float* dir) override {
            char* control = (char*)m_state[HALO3_TLS_SLOT_PLAYER_CONTROL];  // entry 0 = local player 1
            if (!control || m_p.count == 0) return false;
            float yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
            float pitch = Field<float>(control, HALO3_PLAYER_CONTROL_PITCH);
            dir[0] = cosf(yaw) * cosf(pitch);
            dir[1] = sinf(yaw) * cosf(pitch);
            dir[2] = sinf(pitch);
            Engine<HeadPositionFn>(OFFSET_HALO3_UNIT_HEAD_POSITION)(m_p.datums[0], eye);
            return true;
        }
        bool Ray(const float* from, const float* vec, bool ignorePlayer, float* hit) override {
            return CastRay(m_p, from, vec, ignorePlayer, hit);
        }
        bool Position(uint32_t object, float* out) override {
            char* o = Blam::ObjectFromDatum(m_objects, object);
            if (o) memcpy(out, &Field<float>(o, HALO3_OBJECT_POSITION), 3 * sizeof(float));
            return o != nullptr;
        }
        float Radius(uint32_t object) override {
            char* o = Blam::ObjectFromDatum(m_objects, object);
            char* tag = o ? Halo3Tags::Get(Field<uint32_t>(o, HALO3_OBJECT_TAG_DATUM)) : nullptr;
            return tag ? Field<float>(tag, HALO3_OBJE_BOUNDING_RADIUS) : 0.0f;
        }
        void Teleport(uint32_t object, const float* position) override {
            // A 64-bit null location: a 32-bit 0 left garbage in the high half (see noclip).
            Engine<SetPositionFn>(OFFSET_HALO3_OBJECT_SET_POSITION)(object, position, nullptr, nullptr, nullptr, false);
            const float still[3] = {};
            Engine<SetVelocityFn>(OFFSET_HALO3_OBJECT_SET_VELOCITY)(object, still, still);
            if (m_p.count > 0 && object == m_p.datums[0]) Halo3Cheats::MoveNoclipAnchor(position);
        }
        void AddVelocity(uint32_t object, const float* add) override {
            char* o = Blam::ObjectFromDatum(m_objects, object);
            if (!o) return;
            const float* now = &Field<float>(o, HALO3_OBJECT_VELOCITY);
            float velocity[3] = {now[0] + add[0], now[1] + add[1], now[2] + add[2]};
            Engine<SetVelocityFn>(OFFSET_HALO3_OBJECT_SET_VELOCITY)(object, velocity, nullptr);
        }
        uint32_t Create(uint32_t selection, int team, const float* position, float yaw) override {
            char* scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
            if (!scnr) return 0xFFFFFFFF;
            uint32_t made = ::Create(m_state, m_objects, scnr, selection, team, position, yaw);
            if (made == 0xFFFFFFFF || Halo3Tags::GroupOf(ChoiceFor(selection).vehicle) != HALO3_TAG_CHAR) return made;
            auto actors = (Blam::DataArray*)m_state[HALO3_TLS_SLOT_ACTORS];  // an actor: its unit
            uint16_t index = made & 0xFFFF;
            if (!actors || index >= actors->highWater) return 0xFFFFFFFF;
            char* actor = actors->Entry(index);
            return *(uint16_t*)actor == (made >> 16) ? Field<uint32_t>(actor, HALO3_ACTOR_UNIT) : 0xFFFFFFFF;
        }
        void PrepareFor(uint32_t selection) override {
            char* scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
            if (auto zone = scnr ? ZoneToLoad(scnr, ChoiceFor(selection).vehicle) : 0) LoadZones(zone);
        }
        bool InUseByPlayer(uint32_t object) override {
            for (int i = 0; i < m_p.count; ++i) {
                if (Field<uint32_t>(m_p.units[i], HALO3_OBJECT_PARENT) == object) return true;
                for (int w = 0; w < HALO3_UNIT_WEAPON_COUNT; ++w)
                    if (Field<uint32_t>(m_p.units[i], HALO3_UNIT_WEAPONS + w * 4) == object) return true;
            }
            return false;
        }
        void Delete(uint32_t object) override { Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(object); }
        void Message(const char* text) override { SpawnResult(text); }
        bool ReadSkulls(uint64_t& mask) override {
            char* globals = (char*)m_state[HALO3_TLS_SLOT_GAME_GLOBALS];
            if (!globals) return false;
            mask = MenuSkulls(globals);
            return true;
        }
        void WriteSkulls(uint64_t mask) override {
            SetMenuSkulls((char*)m_state[HALO3_TLS_SLOT_GAME_GLOBALS], mask);  // read live; no refresh call
        }
        uint64_t SkullMask() override { return (1ull << kMenuSkullCount) - 1; }

    private:
        void** m_state;
        const Players& m_p;
        Blam::DataArray* m_objects;
    };
}

void Halo3Spawn::FunTick(void** state, const Players& p, Blam::DataArray* objects, const Games::Settings& s,
                         float dt) {
    if (!g_module) return;
    FunTitle title(state, p, objects);
    Fun::Tick(title, s, dt);
}

void Halo3Spawn::SetModule(HMODULE halo3) { g_module = (char*)halo3; }

void Halo3Spawn::Reset() {
    Fun::Reset();
    for (auto& ai : g_spawnedAi) ai = {};
    g_wait = {};
    g_forcedZones = 0;
    g_forcedScenario = nullptr;
    g_characters.clear();
    g_vehicles.clear();
    g_objects.clear();
    std::lock_guard<std::mutex> lock(g_listsLock);
    g_vehicleChoices.clear();
}

void Halo3Spawn::RefreshLists(char* scnr) {
    g_characters.clear();
    for (auto& character : Halo3Tags::ListGroup(HALO3_TAG_CHAR))
        if (Spawnable(character.datum)) g_characters.push_back(character);
    Guard("Halo 3 vehicle list", [&] { BuildVehicleList(scnr); });
    Guard("Halo 3 object list", [&] { BuildObjectList(scnr); });
    LOG("Halo 3: spawn lists: %zu characters, %zu vehicles, %zu objects", g_characters.size(), g_vehicles.size(),
        g_objects.size());
}

void Halo3Spawn::FillStatus(Games::Status& status) {
    status.spawnAi = &g_characters;
    status.spawnVehicles = &g_vehicles;
    status.spawnObjects = &g_objects;
    std::lock_guard<std::mutex> lock(g_resultLock);
    status.spawnDone = g_spawnDone;
    sprintf_s(status.spawnMessage, "%s", g_spawnMessage);
}

void Halo3Spawn::Tick(void** state, const Players& p, Blam::DataArray* objects, const Games::Settings& s) {
    if (!g_module) return;
    g_lastState = state;
    if (!g_haveSerial) {  // don't run a key press from before this session
        g_spawnSerial = s.spawnSerial;
        g_haveSerial = true;
    }
    char* scnr = nullptr;
    Guard("Halo 3 spawn zones", [&] {
        scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
        KeepForcedZones(scnr);
    });
    if (!scnr) return;
    if (s.spawnSerial != g_spawnSerial) {
        g_spawnSerial = s.spawnSerial;
        if (s.spawnEnabled && s.spawnSquad >= 0)
            Guard("Halo 3 squad", [&] { Halo3Upgrade::SpawnSquad(state, p, objects, s.spawnSquad, s.spawnTeam); });
        else if (s.spawnEnabled && s.spawnTag != 0xFFFFFFFF)
            Guard("Halo 3 spawn", [&] { Spawn(state, p, objects, s.spawnTag, s.spawnTeam); });
    }
    Guard("Halo 3 spawn zone wait", [&] { ContinueZoneWait(state, objects, scnr); });
    Guard("Halo 3 spawned AI", [&] { FollowSpawnedAi(state, p, objects, scnr, s.spawnRespawn); });
}

// --- Shared with the feature files (internal.h) ---------------------------------

char* Halo3Internal::Module() { return g_module; }

uint32_t Halo3Internal::SpawnActor(void** state, const float* position, float yaw, uint32_t character, int team) {
    char* scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
    if (!g_module || !scnr || !Halo3Tags::Get(character)) return 0xFFFFFFFF;
    return ::SpawnActor(state, scnr, character, position, yaw, team);
}

int Halo3Internal::ResolveSpawnTeam(void** state, int choice) { return ::ResolveSpawnTeam(state, choice); }

void Halo3Internal::SetActorTeam(uint32_t actor, int16_t team) {
    if (g_lastState) SetTeamBoth(g_lastState, actor, team);
    else Engine<ActorSetTeamFn>(OFFSET_HALO3_ACTOR_SET_TEAM)(actor, team);
}

void Halo3Internal::Follow(void** state, Blam::DataArray* objects, uint32_t actor, uint32_t character, int team) {
    ::Follow(state, objects, actor, character, team);
}

void Halo3Internal::Refollow(void** state, Blam::DataArray* objects, uint32_t oldUnit, uint32_t newActor,
                             uint32_t newCharacter) {
    for (auto& ai : g_spawnedAi) {
        if (ai.unit != oldUnit) continue;
        int team = ai.team;
        ai = {};
        ::Follow(state, objects, newActor, newCharacter, team);
        return;
    }
}

int Halo3Internal::FollowedUnits(uint32_t* out, int max) {
    int n = 0;
    for (auto& ai : g_spawnedAi)
        if (ai.unit != 0xFFFFFFFF && n < max) out[n++] = ai.unit;
    return n;
}

void Halo3Internal::ForgetFollowed() {
    for (auto& ai : g_spawnedAi) ai = {};
    g_wait = {};
}

void Halo3Internal::Toast(const char* text) { SpawnResult(text); }

void Halo3Internal::DeleteObject(uint32_t object) { Engine<ObjectDeleteFn>(OFFSET_HALO3_OBJECT_DELETE)(object); }

uint32_t Halo3Internal::NewObject(uint32_t tag, const float* position, float yaw) {
    return ::NewObject(tag, 0, position, yaw);
}

bool Halo3Internal::TeamsFriends(void** state, int a, int b) {
    if (a == b) return true;
    if (a < 0 || b < 0 || a >= HALO3_TEAM_COUNT || b >= HALO3_TEAM_COUNT) return false;
    char* allegiance = state ? (char*)state[HALO3_TLS_SLOT_ALLEGIANCE] : nullptr;
    if (!allegiance) return false;
    return (Field<uint32_t>(allegiance, HALO3_ALLEGIANCE_FRIENDS + a * 4) >> b) & 1;
}

bool Halo3Internal::AimPoint(void** state, const Players& p, float* at, float* dir, float* facing) {
    if (!g_module || p.count == 0) return false;
    char* control = (char*)state[HALO3_TLS_SLOT_PLAYER_CONTROL];
    if (!control) return false;
    float yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
    float pitch = Field<float>(control, HALO3_PLAYER_CONTROL_PITCH);
    dir[0] = cosf(yaw) * cosf(pitch);
    dir[1] = sinf(yaw) * cosf(pitch);
    dir[2] = sinf(pitch);
    float eye[3];
    Engine<HeadPositionFn>(OFFSET_HALO3_UNIT_HEAD_POSITION)(p.datums[0], eye);
    float ray[3] = {dir[0] * kSpawnRange, dir[1] * kSpawnRange, dir[2] * kSpawnRange};
    if (!CastRay(p, eye, ray, true, at)) return false;
    *facing = remainderf(yaw + kPi, 2.0f * kPi);
    if (*facing < 0.0f) *facing += 2.0f * kPi;
    return true;
}

const std::vector<Games::TagChoice>& Halo3Internal::Characters() { return g_characters; }

bool Halo3Internal::LoadZoneFor(uint32_t tag) {
    char* scnr = Halo3Tags::GetGlobal(HALO3_TAG_SCNR);
    uint32_t zone = scnr ? ZoneToLoad(scnr, tag) : 0;
    if (zone) LoadZones(zone);
    return zone != 0;
}
