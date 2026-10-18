#pragma once

// halo3.dll offsets for MCC 1.3528.0.0. Each one notes where it was
// confirmed so it can be re-found after a game update. "RVA" = address in
// halo3.dll relative to its base. Tag layouts are from Assembly's
// Halo3MCC plugins (github.com/XboxChaos/Assembly).

// --- Hooks --------------------------------------------------------------

// Main loop: runs once on the game thread per session (from AlphaRing).
#define OFFSET_HALO3_ENGINE 0xB2D40
// Per-frame player input update, called from the frame update (RVA 0xB2AA3).
// Engine calls made here run with the frame's per-thread context set up;
// from MCC's input polling they fault (null per-thread table entry).
#define OFFSET_HALO3_FRAME_INPUT 0xB414C

// --- Engine functions (call on the main thread only) ---------------------
// object_placement_data_new(placement*, tag datum, owner object, 0)  (RVA 0x357DA6 caller)
#define OFFSET_HALO3_OBJECT_PLACEMENT_NEW 0x341508
// object datum object_new(placement*)
#define OFFSET_HALO3_OBJECT_NEW           0x341AE8
// object_delete(object datum)  (RVA 0x357E40 caller)
#define OFFSET_HALO3_OBJECT_DELETE        0x342ED8
// bool unit_add_weapon_to_inventory(unit, weapon, mode)
//   mode 1: into a free slot (starting loadout, RVA 0x357E34)
//   mode 3: swap for the held weapon, dropping it (weapon pickup, RVA 0x3B32B6)
//   mode 4: dual wield (weapon pickup, RVA 0x3B3262)
#define OFFSET_HALO3_UNIT_ADD_WEAPON      0x358024
#define HALO3_ADD_WEAPON_FREE_SLOT        1
#define HALO3_ADD_WEAPON_SWAP_HELD        3
// unit_kill(unit datum)  (unit_kill evaluator, RVA 0x1E4DA8)
#define OFFSET_HALO3_UNIT_KILL            0x34D520
// object_teleport(object, position*, forward*, up*, bool)  (object_teleport, RVA 0x2596C3)
// object_set_position(object, position*, forward*, up*, location* or null, false): the last
// step of object_teleport (Halo 3 RVA 0x2595F2), which also reconnects the
// object to the map. Null vectors keep the current facing. Noclip uses it:
// the teleport also writes the facing into the unit's aim, and from MCC's
// input polling it faulted halfway (after disconnecting the unit from the
// map), leaving the player invisible (2026-10-02).
#define OFFSET_HALO3_OBJECT_SET_POSITION 0x340F38
// object_set_velocity's core (evaluators 0x1E3690/0x1E380C -> 0x39BAD0 local
// space -> here): (object, const float* linear world or null, const float*
// angular or null); writes +0x74/+0x80, updates the physics (0x14522C) and
// wakes the object. Reach's 0x46CD64. Found statically 2026-10-04.
#define OFFSET_HALO3_OBJECT_SET_VELOCITY 0x3411A4
// Barrel fire(weapon datum, int16 barrel, void*, int, bool): makes the
// barrel's projectiles, reading its projectile tag inside (0x368749, 0x3689FC
// -> object_placement_new 0x369113). The weapon update only marks a barrel
// as fired (0x366858: barrel state +0x1B4 bit 2, weapon +0x170 bit 0); after
// the objects update, 0x35EC1C -> 0x35EC84 calls this once per marked barrel,
// for every weapon (AI too). Hooked for the chaos gun and recoil push.
#define OFFSET_HALO3_BARREL_FIRE 0x3683A0
#define OFFSET_HALO3_OBJECT_TELEPORT      0x259410
#define HALO3_PLACEMENT_DATA_SIZE         0x400  // real size is ~0x158; oversized buffer
// unit_get_head_position(unit datum, float out[3]): the head marker
// (objects_can_see_object, RVA 0x2587B1)
#define OFFSET_HALO3_UNIT_HEAD_POSITION   0x355460

// Placement fields (the squad spawner fills them in before object_new, RVA
// 0x565638..0x565661): variant name string id, position, forward, up.
#define HALO3_PLACEMENT_VARIANT           0xC
#define HALO3_PLACEMENT_POSITION          0x1C
#define HALO3_PLACEMENT_FORWARD           0x28
#define HALO3_PLACEMENT_UP                0x34

// --- Spawning (found 2026-10-04 on 030_outskirts) ----------------------------
// bool collision_test_vector(u64 flags, bool, point*, vector*, ignore object,
// ignore object 2, int32, result*): 44 callers, the same shape as Reach's.
// It sets the result's fraction (+0x4) to 1 first; the hit object is +0x40
// (the camera's collision, RVA 0x2130C5). Flags: the global most callers
// load (RVA 0x222AFC8, 0x9A500000009 live).
#define OFFSET_HALO3_COLLISION_TEST_VECTOR 0x1FD748
#define OFFSET_HALO3_COLLISION_FLAGS       0x222AFC8  // u64
#define HALO3_COLLISION_RESULT_FRACTION    0x4
#define HALO3_COLLISION_RESULT_SIZE        0x400  // oversized buffer
// actor_create(character, squad, cell, starting location*, int16 flags) ->
// actor datum or -1: makes the character's unit at the starting location,
// arms it from the start's weapon palette indices (else its fire team's,
// RVA 0x55DB90) and attaches the actor. Squad and cell -1 with flags 4 is
// what the game's own squadless spawns pass (RVA 0x5BEF7D, record built by
// 0x5BF004; layout below). Actors: TLS slot 1, 0xAA0 each.
#define OFFSET_HALO3_ACTOR_CREATE          0x55D32C
#define HALO3_ACTOR_CREATE_FLAGS           4
#define HALO3_START_SIZE                   0x88   // scnr starting location
#define HALO3_START_POSITION               0x8
#define HALO3_START_REFERENCE_FRAME        0x14   // int16, -1
#define HALO3_START_BSP                    0x16   // int16, -1
#define HALO3_START_YAW                    0x18   // radians, then pitch
#define HALO3_START_FLAGS                  0x24   // 2, as 0x5BF273 sets it
#define HALO3_START_FLAGS_SQUADLESS        2
#define HALO3_START_CHARACTER              0x28   // int16 palette indices, -1 = none
#define HALO3_START_WEAPON                 0x2A
#define HALO3_START_SECONDARY_WEAPON       0x2C
#define HALO3_START_VEHICLE                0x30   // int16 -1; then seat, grenade type (-1 each)
#define HALO3_START_SEAT                   0x32
#define HALO3_START_GRENADES               0x34
#define HALO3_START_SWARM_COUNT            0x36   // 1
#define HALO3_START_EMITTER                0x44   // int16 -1 x3 (emitter, giant, biped body)
#define HALO3_START_SCRIPT_INDEX           0x6C   // int16 -1
#define HALO3_START_EQUIPMENT              0x6E   // int16 -1
#define HALO3_START_POINT_SET              0x78   // int16 -1
// actor_set_active(actor, bool) (actor +9), right after the squadless spawn
// (RVA 0x5BEFA0); actor_set_team(actor, int16 team): actor +0x20 and its
// unit's team (+0x174) (what ai_set_team does per actor, RVA 0x576F2C).
#define OFFSET_HALO3_ACTOR_SET_ACTIVE      0x55E808
#define OFFSET_HALO3_ACTOR_SET_TEAM        0x55E6A0
#define HALO3_TLS_SLOT_ACTORS              1      // data array, entry 0xAA0
#define HALO3_ACTOR_UNIT                   0x14   // datum (actor_set_team, RVA 0x55E76E)
// Designer zones (designer_zone_activate, RVA 0x1F6734): as in Reach, the
// mod writes the pending masks and the flag; the zone update applies them
// (checked live: zone 3 loaded in under 0.25 s on 030_outskirts).
#define OFFSET_HALO3_DESIGNER_ZONES_ACTIVE   0x46B709C  // u32 mask
#define OFFSET_HALO3_DESIGNER_ZONES_PENDING  0x20B96C4  // u32 deactivate, u32 activate
#define OFFSET_HALO3_ZONE_UPDATE_PENDING     0x20B96B3  // byte flag

// --- Code patches (found by byte pattern, from the user's Cheat Engine table) --
// AI can't fire: removes the compare before "skip firing", so the skip always happens.
#define HALO3_AI_CANT_FIRE_SIZE 6
#define HALO3_AOB_AI_CANT_FIRE  "41 0F 2F 4C 24 14"         // RVA 0x5B0628; patch 6 x NOP
// Invisible to AI (NO LONGER APPLIED, kept for reference): perception stops loading the target
// position for every AI, so enemies also ignored allies and each other. "AI can't see you" now only
// sets the player's ai_disregard unit flag (per unit; see HALO3_UNIT_FLAG_AI_DISREGARD).
#define HALO3_AOB_AI_INVISIBLE  "F3 44 0F 10 60 04 F3 44"   // RVA 0x5D8D85; patch first 6 bytes x NOP
// No recoil: skips the call that kicks the player's view after a shot (RVA
// 0x3675B6, in the loop over players using the weapon, so AI are unaffected).
// The Cheat Engine table instead zeroes the minimum recoil at RVA 0x367586.
#define HALO3_AOB_RECOIL        "F3 0F 5C C6 8B CE F3 0F 59 D0 F3 0F 58 D6 E8"  // RVA 0x3675A8
#define HALO3_RECOIL_CALL_OFFSET 14  // the call within the pattern; patch 5 x NOP
// AI can't hear: an actor hears a sound if hearing range^2 > distance^2; the
// range load at the start of this pattern becomes xorps xmm6,xmm6 (range 0).
// Covers every sound, not just the player's. The Cheat Engine table NOPs the
// load instead, leaving whatever was in xmm6.
#define HALO3_AOB_AI_HEARING    "F3 0F 10 70 18 0F 28 C3 F3 0F 59 C3"  // RVA 0x5D94AB
#define HALO3_AI_HEARING_PATCH  {0x0F, 0x57, 0xF6, 0x90, 0x90}         // 5 bytes
// Third person: the player director's update (vtable[1], RVA 0x211E80) picks
// the camera mode every tick: following (0) if the unit's seat wants it
// (RVA 0x1326E8, e.g. vehicles), otherwise first person (3). Setting the mode
// from outside (set_camera_third_person) is undone next tick, leaving the
// camera stuck mid-transition at the head. The jne becomes jmp, so it always
// picks following.
#define HALO3_AOB_THIRD_PERSON    "41 FF 50 20 BE 01 00 00 00 45 85 E4 75 3E"  // RVA 0x2120A1
#define HALO3_THIRD_PERSON_OFFSET 12                                          // the jne, RVA 0x2120AD
#define HALO3_THIRD_PERSON_PATCH  {0xEB}

// --- Thread-local game state -------------------------------------------
// Byte offset in the TLS block of the per-thread context index. It differs
// between MCC's input polling and the frame; engine code indexes a table
// with it, and the input-polling value selects an empty entry (the fault at
// RVA 0x23300E / 0x9B710).
#define HALO3_TLS_THREAD_INDEX 0x18
// Halo 3 keeps its game state in TLS on the main thread. The TLS block is an
// array of pointers; these are slot numbers (byte offset = slot * 8).
#define HALO3_TLS_SLOT_PLAYERS           2   // data array (RVA 0xE3CC2 reads +0x10)
#define HALO3_TLS_SLOT_OBJECTS           7   // data array (RVA 0x1F73CD reads +0x38)
#define HALO3_TLS_SLOT_AI_GLOBALS        6   // ai_grenades, RVA 0x1E6D54 reads +0x30
#define HALO3_TLS_SLOT_GAME_GLOBALS      9   // RVA 0x36AD6C reads +0x48
#define HALO3_TLS_SLOT_PLAYER_CONTROL   24   // RVA 0xF90F7 reads +0xC0
#define HALO3_TLS_SLOT_GAME_TIME        25   // RVA 0x35F226 reads +0xC8
#define HALO3_TLS_SLOT_PHYSICS_CONSTANTS 38  // RVA 0x1E43C9 reads +0x130
#define HALO3_TLS_SLOT_ALLEGIANCE       48   // team allegiances (RVA 0x199B5A reads +0x180)
#define HALO3_TLS_SLOT_DIRECTORS        49   // one per local player (RVA 0x1F642B reads +0x188)
#define HALO3_TLS_SLOT_CHUD             68   // HUD globals (chud_show, RVA 0x1F451C reads +0x220)

// HUD globals
#define HALO3_CHUD_SHOW 0x144  // byte, 0 hides the HUD (chud_show, RVA 0x1F452B)

// directors (camera control), one per local player
#define HALO3_DIRECTOR_SIZE   0x180
#define HALO3_DIRECTOR_CAMERA 0x8   // camera object; vtable[0] returns its mode (RVA 0x131B75)
#define HALO3_CAMERA_FOLLOWING    0  // third person
#define HALO3_CAMERA_FIRST_PERSON 3  // (set_camera_third_person false passes 3)
// Following camera update, vtable[2] (vtable RVA 0x7FB748, set at RVA 0x131F53):
// update(camera*, player index, float dt, observer command*), called by the
// director update (RVA 0x131760). The command is copied to director +0x68 and
// handed to the observer (RVA 0x13199A).
#define OFFSET_HALO3_FOLLOWING_CAMERA_UPDATE 0x213CF4
// Observer command: float distance from the focus point back to the camera
// (RVA 0x2141D9, the length of the unit's camera track offset). The camera's
// own scale at +0x20 only moves the focus point, so it doesn't back off.
#define HALO3_OBSERVER_COMMAND_DISTANCE 0x24
#define HALO3_OBSERVER_COMMAND_FOCUS_OFFSET 0x10  // float[3] (RVA 0x2141F0)

// allegiance globals: in campaign, team a counts team b as a friend if bit b of
// friends[a] is set (game_team_is_friendly, RVA 0x199B08; ai_allegiance edits it).
// Outside campaign, only the same team is friendly.
#define HALO3_ALLEGIANCE_FRIENDS 0x144  // uint32[16]
#define HALO3_TEAM_COUNT         16

// game globals
#define HALO3_GAME_GLOBALS_MODE   0x10    // byte: 1 = campaign (RVA 0x199B4A)
#define HALO3_GAME_MODE_CAMPAIGN  1
#define HALO3_GAME_GLOBALS_SKULLS 0xFB88  // 128-bit skull bitfield (RVA 0x36AD75)
#define HALO3_SKULL_BANDANNA      23      // infinite ammo (RVA 0x36AD7B)
#define HALO3_SKULL_ACROPHOBIA    45      // flight
// The original skulls are past MCC's u64: primary dword +0xFB90 (iron 0 ...
// mythic 8; game_skull_is_active_primary 0x136944), secondary dword +0xFB94
// (assassin 0, blind 1, cowbell 2, birthday party 3, IWHBYD 4; 0x136970).
// Only read in campaign. Copied from the game options at game start
// (0xEEB3A); no refresh call. Names from AlphaRing's eSkull.h.
#define HALO3_GAME_GLOBALS_PRIMARY_SKULLS   0xFB90
#define HALO3_GAME_GLOBALS_SECONDARY_SKULLS 0xFB94

// ai globals
#define HALO3_AI_GLOBALS_GRENADES 0x3C0  // byte, AI may throw grenades (ai_grenades, RVA 0x1E6D63)

// game time globals
#define HALO3_GAME_TIME_TICKS 0xC   // int32 current tick (RVA 0x3846A2)
#define HALO3_GAME_TIME_SPEED 0x10  // float, 1.0 = normal (AlphaRing game_time.h)

// physics constants
#define HALO3_PHYSICS_GRAVITY 0x0            // float (RVA 0x1E43D8)
#define HALO3_DEFAULT_GRAVITY 4.1712594f     // RVA 0x847220 (used by physics_set_gravity)

// player control: one entry per local player
#define HALO3_PLAYER_CONTROL_SIZE  0x110  // RVA 0xF90FB
#define HALO3_PLAYER_CONTROL_YAW   0xD4   // float radians
#define HALO3_PLAYER_CONTROL_PITCH 0xD8   // float radians (player0_set_pitch, RVA 0xF9106)

// --- Players (data array entries) ---------------------------------------
#define HALO3_PLAYER_UNIT_DATUM 0x28

// --- Objects (every object type) ------------------------------------------
#define HALO3_OBJECT_TAG_DATUM     0x0    // tag index of the object's definition
#define HALO3_OBJECT_PARENT        0x10   // object this one is attached to (e.g. a vehicle seat), or -1
// Flip ejection (found statically 2026-10-02, the same shape as Halo 2's):
// the unit update 0x37B6C8 compares its ticks upside down with the seat's
// turnover time (seat +0x24) and queues unit action 0x14 through the wrapper
// 0x379C08(unit, type) at 0x37BAA4 (returns to 0x37BAA9).
#define OFFSET_HALO3_UNIT_ACTION_QUEUE 0x379C08
// Checkpoints, as in Reach: game_save_immediate calls 0x1C92B8(3) (RVA
// 0x1F34D4); game_revert writes 0x0C01 to the word at 0x20B96AA (RVA 0x1F2FC2).
#define OFFSET_HALO3_GAME_SAVE        0x1C92B8
#define HALO3_GAME_SAVE_IMMEDIATE     3
#define OFFSET_HALO3_GAME_REVERT_FLAG 0x20B96AA
#define HALO3_GAME_REVERT_REQUEST     0x0C01
#define HALO3_ACTION_EJECT             0x14
#define HALO3_FLIP_EJECT_RETURN        0x37BAA9
#define HALO3_OBJECT_NEXT_SIBLING  0x08   // attached objects: first child, then siblings (RVA 0x37274B, 0x37278C)
#define HALO3_OBJECT_FIRST_CHILD   0x0C
#define HALO3_OBJECT_POSITION      0x50   // Vector3
#define HALO3_OBJECT_FORWARD       0x5C   // Vector3
#define HALO3_OBJECT_UP            0x68   // Vector3
#define HALO3_OBJECT_VELOCITY      0x74   // Vector3 (between up 0x68 and angular 0x80)
#define HALO3_OBJECT_TYPE          0x96   // byte: 0 biped, 1 vehicle, 2 weapon, ...
#define HALO3_OBJECT_PHYSICS_FLAGS 0xA8   // bit 8 = physics disabled (object_set_physics, RVA 0x1E34A6)
#define HALO3_OBJECT_HEALTH        0xF4   // float, normalized
#define HALO3_OBJECT_SHIELD        0xF8   // float, normalized
#define HALO3_OBJECT_DAMAGE_FLAGS  0x110

#define HALO3_OBJECT_TYPE_BIPED  0
#define HALO3_OBJECT_TYPE_VEHICLE 1
#define HALO3_OBJECT_TYPE_WEAPON 2
#define HALO3_OBJECT_TYPE_EQUIPMENT 3

#define HALO3_PHYSICS_FLAG_DISABLED             0x100u
#define HALO3_DAMAGE_FLAG_CANNOT_TAKE_DAMAGE    0x100u
#define HALO3_DAMAGE_FLAG_CANNOT_DIE_FROM_DAMAGE 0x10000u
#define HALO3_DAMAGE_FLAG_KILL                  0x40u  // set by unit_kill (RVA 0x34D556); the object update then kills

// --- Units (bipeds, vehicles) ---------------------------------------------
#define HALO3_UNIT_ACTOR_INDEX     0x168  // -1 if not AI-controlled
#define HALO3_UNIT_FLAGS           0x170  // (ai_disregard, RVA 0x1E7732)
// AI ignores this unit. Consumer (static): AI update 0x5CE990 copies unit+0x170 bit 7 into each
// perception prop's byte +0x2E (bit 8 -> prop+0xC0) every update; the prop vision evaluator
// 0x5DB40C returns early (no sight) when prop+0x2E != 0, and 0x5D9EBC skips its branch likewise.
#define HALO3_UNIT_FLAG_AI_DISREGARD 0x80u
#define HALO3_UNIT_TEAM            0x174  // int16 (unit_get_team_index, RVA 0x1E5433)
#define HALO3_UNIT_PLAYER_INDEX    0x178  // -1 if not a player (RVA 0x36ADAC)
#define HALO3_UNIT_CURRENT_WEAPON  0x262  // int8 (RVA 0x1F73EC)
#define HALO3_UNIT_DESIRED_WEAPON  0x266  // int8; the unit switches to this slot (RVA 0x35A0A2)
#define HALO3_PLAYER_WEAPON_SLOTS  2      // players carry 2 (slots 0-1); more fails to add
#define HALO3_UNIT_WEAPONS         0x268  // uint32 datum[4] (RVA 0x3563C5)
#define HALO3_UNIT_EQUIPMENT       0x288  // uint32 datum of carried equipment, or -1 (RVA 0x3509AB)
#define HALO3_UNIT_GRENADE_COUNTS  0x294  // int8[4] (unit_get_total_grenade_count, RVA 0x1E5FC3)
#define HALO3_UNIT_WEAPON_COUNT    4
#define HALO3_UNIT_GRENADE_TYPES   4

#define HALO3_TEAM_PLAYER 1
#define HALO3_TEAM_HUMAN  2
#define HALO3_TEAM_COVENANT 3

// --- Weapons --------------------------------------------------------------
#define HALO3_WEAPON_HEAT          0x188  // float; overheats at the tag's limit (RVA 0x36728C)
// Weapon state flags (word); bit 0x2 = overheated. The game clears it while
// cooling, once heat drops below the tag's recovery threshold (Halo 3 RVA
// 0x35FEAC). Zeroing heat mid-cooldown skipped that and left the weapon
// unable to fire (ODST beam rifle, 2026-10-02), so no overheat clears it too.
#define HALO3_WEAPON_FLAGS       0x170
#define HALO3_WEAPON_FLAG_OVERHEATED 0x2u
#define HALO3_WEAPON_AGE           0x18C  // float; battery drain for energy weapons (RVA 0x3606C0)
// Barrel state, 2 per weapon (RVA 0x35F508: index * 0x34, weapon + 0x1CC).
#define HALO3_WEAPON_BARRELS       0x1B0
#define HALO3_WEAPON_BARREL_SIZE   0x34
#define HALO3_WEAPON_BARREL_COUNT  2
#define HALO3_BARREL_TICKS_SINCE_SHOT 0x0 // int8, capped at 0x7F; a shot needs it to reach
                                          // ticks-per-round from the tag's rounds per second (RVA 0x36556B)
#define HALO3_BARREL_TICKS_MAX     0x7F
#define HALO3_BARREL_FIRE_DELAY    0x2    // int16 recovery ticks after a shot (RVA 0x368004)
#define HALO3_BARREL_ERROR         0x1C   // float spread, 0..1, grows while firing (RVA 0x35F5C8)
// Trigger state, 2 per weapon (RVA 0x36008C: index * 12, weapon + 0x21C).
#define HALO3_WEAPON_TRIGGERS      0x218
#define HALO3_WEAPON_TRIGGER_SIZE  0xC
#define HALO3_WEAPON_TRIGGER_COUNT 2
#define HALO3_TRIGGER_FLAGS        0x6    // word
#define HALO3_TRIGGER_FLAG_READY   0x1u   // set on trigger release, cleared by a semi-auto
                                          // shot (RVA 0x360D94, 0x361093)
#define HALO3_WEAPON_MAGAZINES     0x230  // array of magazine state (RVA 0x35F1E5)
#define HALO3_WEAPON_MAGAZINE_SIZE 0x18
#define HALO3_MAGAZINE_RESERVE     0x6    // int16 rounds not in the magazine (RVA 0x362FCB)
#define HALO3_MAGAZINE_LOADED      0xA    // int16 rounds in the magazine (RVA 0x35F212)

// --- Equipment ------------------------------------------------------------
// Using equipment stamps the activation tick; it stays active (then expires)
// until now - activation >= the tag's duration (RVA 0x384078). After a use,
// equipment whose uses left == 0 leaves the unit (RVA 0x357714); every check
// is == 0, so a negative count keeps it. Using equipment, and picking up
// other equipment, needs the carried one's effect to be over (RVA 0x3843D9,
// 0x3579FD).
#define HALO3_EQUIPMENT_ACTIVATED  0x174  // int32 tick of last use, or -1 (RVA 0x3846A8)
#define HALO3_EQUIPMENT_USES_SPENT 0x178  // uint8 (RVA 0x3846CE)

// --- Tags -----------------------------------------------------------------
// Tag data = *tag_base + address * 4 (RVA 0x35F1BF).
#define OFFSET_HALO3_TAG_BASE          0x1FCF4C8  // char*
#define OFFSET_HALO3_TAG_INDEX_HEADER  0x20AD058  // header* (RVA 0xA6B76)
#define HALO3_TAG_HEADER_GROUPS        0x8        // {u32 magic, parent, grandparent, name}* (tag_get, RVA 0xA70D6)
#define HALO3_TAG_HEADER_TAG_COUNT     0x10       // int32
#define HALO3_TAG_HEADER_INSTANCES     0x18       // {u16 group, u16 salt, u32 address}*
#define HALO3_TAG_HEADER_GLOBAL_COUNT  0x20       // int32
#define HALO3_TAG_HEADER_GLOBALS       0x28       // {u32 group magic, u32 datum}*  (scnr, matg, ...)

// Tag names aren't kept in memory in normal play (the table at RVA 0xA49148
// is only filled on a debug load path), so they're read from the .map file.
// Map header (Assembly LayoutsU13): file table count/offset/size/index at
// 0x20/0x24/0x28/0x2C; offsets are relative, add offset mask[0] at 0x4CC.
#define HALO3_MAP_HEADER_SIZE        0x4000
#define HALO3_MAP_FILE_TABLE_COUNT   0x20
#define HALO3_MAP_FILE_TABLE_OFFSET  0x24
#define HALO3_MAP_FILE_TABLE_SIZE    0x28
#define HALO3_MAP_FILE_INDEX_OFFSET  0x2C
#define HALO3_MAP_OFFSET_MASK0       0x4CC
// The loaded map's header, copied to memory at map load (RVA 0xA65F6).
#define OFFSET_HALO3_MAP_HEADER      0x20A9058
#define HALO3_MAP_INTERNAL_NAME      0xC0   // e.g. "010_jungle"

// Tag blocks are {int32 count, uint32 address, int32 unused}.
// Tag references are {u32 group, u32, u32, u32 datum}.
#define HALO3_TAGREF_DATUM 0xC

// bipd
#define HALO3_BIPD_JUMP_VELOCITY 0x2C8  // float

// vehi physics types (Assembly Halo3MCC vehi.xml): one block per type, only
// the vehicle's own filled in. Vehicle speed scales each type's max speeds
// and acceleration, or its engine's max angular velocity.
// Boost (unit part of vehi): time at peak power, then the cooldown.
// Boost update (RVA 0x34E3E4): vehicle +0x3F4 is the boost timer, +0x3F8 the
// cooldown (0 = none). Boosting lasts rise (+0x28C) + peak time; on release
// or timeout the cooldown is set from dead time.
#define HALO3_VEHI_BOOST_PEAK_TIME 0x290
#define HALO3_VEHI_BOOST_DEAD_TIME 0x298
// Seats (unit part of vehi; Assembly Halo3MCC vehi.xml): boarding needs
// "Boarding Seat" (flags bit 11) and a boarding seat index other than -1, as
// in Halo 2. The unit's seat index is at +0x24E (read at RVA 0xF4670).
#define HALO3_VEHI_SEATS              0x258  // block, element 0xD4
#define HALO3_VEHI_SEAT_SIZE          0xD4
#define HALO3_SEAT_FLAGS              0x0
#define HALO3_SEAT_FLAG_BOARDING_SEAT 0x800u
#define HALO3_SEAT_BOARDING_INDEX     0x42   // int16
#define HALO3_UNIT_SEAT               0x24E  // int16
// EMP: unit_emp (RVA 0x34D5B0) converts the tag's EMP Disabled Time to ticks
// and stores them at unit +0x37E; 0 ticks means no shutdown.
#define HALO3_VEHI_EMP_DISABLED_TIME  0x264  // float, seconds
#define HALO3_UNIT_EMP_TICKS          0x37E  // int16
#define HALO3_VEHI_TANK     0x2C0  // element 0x58: engine max angular velocity +0x30
#define HALO3_VEHI_JEEP     0x2CC  // 0x40: engine +0x18
#define HALO3_VEHI_PLANE    0x2D8  // 0x4C: forward/reverse speed +0x0/+0x4, acceleration +0x8
#define HALO3_VEHI_SCOUT    0x2E4  // 0x70: +0x8/+0xC/+0x10; engine max vehicle speeds +0x48/+0x5C
#define HALO3_VEHI_FIGHTER  0x2F0  // 0x64: +0x14/+0x18/+0x1C
#define HALO3_VEHI_MANTIS   0x308  // 0xAC: +0x14/+0x18/+0x1C
#define HALO3_VEHI_VTOL     0x314  // 0x74: max left/forward acceleration +0x44/+0x48
#define HALO3_VEHI_CHOPPER  0x320  // 0x70: engine +0x18
#define HALO3_VEHI_GUARDIAN 0x32C  // 0x30: +0x8/+0xC/+0x10

// weap
#define HALO3_WEAP_MAGAZINES          0x424  // block, element 0x80
#define HALO3_WEAP_MAGAZINE_DEF_SIZE  0x80
#define HALO3_MAGDEF_ROUNDS_TOTAL_MAX 0x8    // int16
#define HALO3_MAGDEF_ROUNDS_LOADED_MAX 0xA   // int16
#define HALO3_WEAP_FLAGS              0x18C  // flags32
#define HALO3_WEAP_FLAG_NOT_FOR_PLAYER (1u << 23)  // "Cannot Be Used By Player" (turrets, vehicle guns)
// "Melee Only" (energy sword, gravity hammer): bit 18 of the same flags per
// Assembly's weap.xml; engine code also reads tag+0x18C >> 0x12 & 1 (RVA 0x363D81, 0x55FC69). Static only.
#define HALO3_WEAP_FLAG_MELEE_ONLY     (1u << 18)
#define HALO3_WEAP_AUTOAIM_ANGLE      0x328  // float radians
#define HALO3_WEAP_AUTOAIM_RANGE      0x32C
#define HALO3_WEAP_AUTOAIM_FALLOFF    0x330
#define HALO3_WEAP_DEVIATION_ANGLE    0x340  // how far autoaim may bend a projectile
#define HALO3_WEAP_BARRELS            0x43C  // block, element 0x134
#define HALO3_WEAP_BARREL_SIZE        0x134
// Barrel firing effects (Assembly Halo3MCC weap.xml): the shot's damage on
// the shooter and on the vehicle's riders, and the effect itself, whose
// accelerations can push the vehicle (as Reach's Scorpion cannon does).
#define HALO3_BARREL_FIRING_EFFECTS   0x128  // block, element 0xC4
#define HALO3_FIRING_EFFECT_SIZE      0xC4
#define HALO3_FIRING_EFFECT_EFFECT           0x4
#define HALO3_FIRING_EFFECT_SECONDARY_EFFECT 0x34
#define HALO3_FIRING_EFFECT_DAMAGE           0x44
#define HALO3_FIRING_EFFECT_SECONDARY_DAMAGE 0x74
#define HALO3_FIRING_EFFECT_RIDER_DAMAGE     0x84
#define HALO3_FIRING_EFFECT_SECONDARY_RIDER_DAMAGE 0xB4
// effe (Assembly Halo3 effe.xml, the Xbox 360 layout)
#define HALO3_EFFE_EVENTS               0x2C   // block, element 0x44
#define HALO3_EFFE_EVENT_SIZE           0x44
#define HALO3_EFFE_EVENT_ACCELERATIONS  0x2C   // block, element 0x14
#define HALO3_EFFE_ACCELERATION_SIZE    0x14
#define HALO3_EFFE_ACCELERATION         0x8    // float
#define HALO3_BARREL_PROJECTILE       0x98   // tag reference
// Barrel fire also reads these instead of the projectile: the secondary when
// barrel state +0x1B6 > 1, the crate projectile whenever it's set, and on
// barrel 0 with weapon +0x1AC > 0, barrel 1's projectile and secondary.
#define HALO3_BARREL_SECONDARY_PROJECTILE 0xA8  // tag reference
#define HALO3_BARREL_CRATE_PROJECTILE     0xC8  // tag reference

// eqip
#define HALO3_EQIP_USES 0x194  // int16 number of uses; -1 unlimited, 0 doesn't count (RVA 0x384199)

// matg (game globals tag)
#define HALO3_MATG_GRENADES           0x41C  // block, element 0x34; +0x0 int16 maximum count
#define HALO3_MATG_GRENADE_DEF_SIZE   0x34
#define HALO3_MATG_GRENADE_PROJECTILE 0x24  // tag reference in each grenade definition
#define HALO3_MATG_PLAYER_INFORMATION 0x434  // block; +0x0 walk, +0x4 run fwd, +0x8 back, +0xC side, +0x10 accel
#define HALO3_MATG_PLAYER_INFO_SIZE   0xC0

// obje (every object tag)
#define HALO3_OBJE_BOUNDING_RADIUS  0x4   // float
#define HALO3_OBJE_DEFAULT_VARIANT  0x30  // string id
#define HALO3_OBJE_MODEL            0x40  // datum of the hlmt (tag reference at 0x34)
// hlmt: model variants, name string id first
#define HALO3_HLMT_VARIANTS         0x64  // block, element 0x30
#define HALO3_HLMT_VARIANT_SIZE     0x30
// char: parent and unit datums (references at 0x4 and 0x14), as in Reach
#define HALO3_CHAR_PARENT           0x10
#define HALO3_CHAR_UNIT             0x20

// scnr palettes (element 0x10, datum at +0xC), squads and designer zones
// (Assembly Halo3MCC scnr.xml; squads and zones checked live on 030_outskirts)
#define HALO3_PALETTE_ENTRY_SIZE      0x10
#define HALO3_PALETTE_ENTRY_DATUM     0xC
#define HALO3_SCNR_SCENERY_PALETTE    0xC0
#define HALO3_SCNR_BIPED_PALETTE      0xD8
#define HALO3_SCNR_VEHICLE_PALETTE    0xF0
#define HALO3_SCNR_EQUIPMENT_PALETTE  0x108
#define HALO3_SCNR_WEAPON_PALETTE     0x120
#define HALO3_SCNR_MACHINE_PALETTE    0x144
#define HALO3_SCNR_CONTROL_PALETTE    0x174
#define HALO3_SCNR_CHARACTER_PALETTE  0x3A8
#define HALO3_SCNR_CRATE_PALETTE      0x5C8
#define HALO3_SCNR_SQUADS             0x384  // element 0x40
#define HALO3_SQUAD_SIZE              0x40
#define HALO3_SQUAD_FIRE_TEAMS        0x30   // element 0x60; starting locations nested in each
#define HALO3_FIRE_TEAM_SIZE          0x60
#define HALO3_FIRE_TEAM_CHARACTER     0x8    // int16 palette indices
#define HALO3_FIRE_TEAM_WEAPON        0xA
#define HALO3_FIRE_TEAM_SECONDARY     0xC
#define HALO3_FIRE_TEAM_STARTS        0x54   // element HALO3_START_SIZE
#define HALO3_SCNR_ZONE_SETS          0x58   // element 0x28
#define HALO3_SCNR_ZONE_SET_SIZE      0x28
#define HALO3_ZONE_SET_FORBIDDEN      0x14   // designer zones this zone set unloads
#define HALO3_SCNR_DESIGNER_ZONES     0x6E4  // element 0xBC
#define HALO3_SCNR_DESIGNER_ZONE_SIZE 0xBC
#define HALO3_DESIGNER_ZONE_BIPEDS    0x8    // blocks of int16 palette indices
#define HALO3_DESIGNER_ZONE_VEHICLES  0x14
#define HALO3_DESIGNER_ZONE_WEAPONS   0x20
#define HALO3_DESIGNER_ZONE_EQUIPMENT 0x2C
#define HALO3_DESIGNER_ZONE_SCENERY   0x38
#define HALO3_DESIGNER_ZONE_MACHINES  0x44
#define HALO3_DESIGNER_ZONE_CONTROLS  0x5C
#define HALO3_DESIGNER_ZONE_CRATES    0x74
#define HALO3_DESIGNER_ZONE_CHARACTERS 0xA4

// scnr
#define HALO3_SCNR_SOFT_CEILINGS      0x234  // block, element 0xC; +0x0 flags16, +0x2 runtime flags16
#define HALO3_SCNR_SOFT_CEILING_SIZE  0xC
#define HALO3_SCNR_KILL_TRIGGERS      0x484  // block of int16 trigger volume indices
#define HALO3_SOFT_CEILING_IGNORE_ALL 0xF    // bipeds | vehicles | camera | huge vehicles

#define HALO3_TAG_WEAP 0x77656170  // 'weap'
#define HALO3_TAG_PROJ 0x70726F6A  // 'proj'
#define HALO3_PROJ_INITIAL_VELOCITY   0x23C  // float; 0 on the thrown grenades (the throw code supplies their speed)
#define HALO3_PROJ_FINAL_VELOCITY     0x240  // float
#define HALO3_TAG_MATG 0x6D617467  // 'matg'
#define HALO3_TAG_SCNR 0x73636E72  // 'scnr' (RVA 0xA6BEE)
#define HALO3_TAG_CHAR 0x63686172  // 'char'
#define HALO3_TAG_VEHI 0x76656869  // 'vehi'
#define HALO3_TAG_EQIP 0x65716970  // 'eqip'
#define HALO3_TAG_SCEN 0x7363656E  // 'scen'
#define HALO3_TAG_MACH 0x6D616368  // 'mach'
#define HALO3_TAG_CTRL 0x6374726C  // 'ctrl'
#define HALO3_TAG_BLOC 0x626C6F63  // 'bloc' (crates)
#define HALO3_TAG_BIPD 0x62697064  // 'bipd'

// String ids (Assembly's Halo3MCC header, as Reach's): an id is a
// namespace (bits 17-24) and an index within it.
#define HALO3_MAP_STRING_COUNT       0x30
#define HALO3_MAP_STRING_DATA        0x34
#define HALO3_MAP_STRING_DATA_SIZE   0x38
#define HALO3_MAP_STRING_INDEX       0x3C
#define HALO3_MAP_STRING_NAMESPACE_COUNT 0x40
#define HALO3_MAP_STRING_NAMESPACES  0x44
