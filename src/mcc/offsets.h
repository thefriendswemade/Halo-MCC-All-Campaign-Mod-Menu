#pragma once

// MCC-Win64-Shipping.exe offsets for MCC 1.3528.0.0 (Steam). They change with
// every game update. Taken from AlphaRing (lib/game/inc/1.3528.0.0 and
// src/mcc/mcc.cpp), which is known to work on this version.

// The version the offsets were found on. It's only logged: the mod doesn't
// refuse other versions, it checks the exe's code at the hooked functions
// instead (see main.cpp), so small updates that don't move them keep working.
#define SUPPORTED_MCC_VERSION L"1.3528.0.0"

// Loads/unloads a title's engine DLL (halo1.dll, halo3.dll, ...).
#define OFFSET_MCC_MODULE_LOAD   0x1E423C
#define OFFSET_MCC_MODULE_UNLOAD 0x1E45A0

// Static game-manager object: the host interface every title's DLL talks to.
// Its first field is a function table; get_key_state is how titles read
// keyboard/mouse/controller input each tick.
// Which title is running. MCC loads every title's DLL at once, so "loaded"
// doesn't mean "running". The game-global object (pointed to from the exe)
// records the running title's module and ID.
#define OFFSET_MCC_IN_GAME            0x4000B9F  // bool
#define OFFSET_MCC_GAME_GLOBAL_PTR    0x4000BC8  // CGameGlobal*
#define MCC_GAME_GLOBAL_TITLE_MODULE  0x2BFE0    // HMODULE
#define MCC_GAME_GLOBAL_TITLE_ID      0x2BFE8    // 0 CE, 1 H2, 2 H3, 3 H4, 4 H2A MP, 5 ODST, 6 Reach

#define OFFSET_MCC_GAME_MANAGER            0x3F7B190
#define MCC_GAME_MANAGER_GET_KEY_STATE_SLOT (0x120 / 8)
#define MCC_INPUT_DATA_SIZE                0x130

// Local co-op (ported from AlphaRing, which uses the same 1.3528.0.0 offsets).
#define OFFSET_MCC_DEVICE_MANAGER   0x4001B78  // CDeviceManager* (pointer to pointer)
#define OFFSET_MCC_DELTA_TIME       0x5A16AC   // float(__fastcall)(int64 qpc ticks)
#define OFFSET_MCC_GET_INDEX_BY_XUID 0x38A09C  // maps a player id to a local player index
// Game manager function table slots (byte offset / 8).
#define MCC_GAME_MANAGER_GET_PLAYER_PROFILE_SLOT  (0x110 / 8)
#define MCC_GAME_MANAGER_SET_VIBRATION_SLOT       (0x138 / 8)
#define MCC_GAME_MANAGER_GET_XBOX_USER_ID_SLOT    (0x2C0 / 8)
#define MCC_GAME_MANAGER_GET_GAMEPAD_MAPPING_SLOT (0x3A0 / 8)
#define OFFSET_MCC_GAME_ENGINE      0x4000BA0  // CGameEngine* (its table's slot 3 is set_event)
#define MCC_GAME_ENGINE_SET_EVENT_SLOT 3
#define MCC_ENGINE_EVENT_LOAD_SETTING 5   // re-reads every player's profile and button layout
