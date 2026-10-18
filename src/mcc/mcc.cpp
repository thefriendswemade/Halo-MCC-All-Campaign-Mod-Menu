#include "mcc.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "MinHook.h"

#include "core/log.h"
#include "coop.h"
#include "games/games.h"
#include "offsets.h"

// MCC passes this to its module load/unload functions.
struct module_info_t {
    int32_t title;
    int32_t errorCode;
    HMODULE hModule;
};

using ModuleLoadFn = void(__fastcall*)(module_info_t*, int, int64_t);
using ModuleUnloadFn = int64_t(__fastcall*)(module_info_t*);
using GetKeyStateFn = bool(__fastcall*)(void* self, DWORD index, void* input);

static ModuleLoadFn g_origModuleLoad = nullptr;
static ModuleUnloadFn g_origModuleUnload = nullptr;
static GetKeyStateFn g_origGetKeyState = nullptr;

static std::atomic<bool> g_inputBlocked{false};
static std::atomic<bool> g_controllerBlocked{false};
static std::atomic<WORD> g_dpadMask{0};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetStateFn g_origXInputGetState = nullptr;  // trampoline once hooked
static XInputGetStateFn g_xinputExport = nullptr;        // the DLL's own function
static bool g_inputHooked = false;

// Titles call this once per player per tick, on their main thread, so it
// doubles as our per-tick main-thread callback. Returning an all-zero input
// block means "nothing pressed, mouse didn't move".
static bool __fastcall GetKeyStateDetour(void* self, DWORD index, void* input) {
    if (index == 0) Games::MainThreadTick();

    if (g_inputBlocked && input) {
        memset(input, 0, MCC_INPUT_DATA_SIZE);
        return true;
    }
    if (Coop::Active() && input) {
        memset(input, 0, MCC_INPUT_DATA_SIZE);
        return Coop::GetKeyState(self, index, input, g_origGetKeyState);
    }
    return g_origGetKeyState(self, index, input);
}

static DWORD WINAPI XInputGetStateDetour(DWORD user, XINPUT_STATE* state) {
    DWORD result = g_origXInputGetState(user, state);
    if (g_controllerBlocked && result == ERROR_SUCCESS && state) memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    else if (result == ERROR_SUCCESS && state) state->Gamepad.wButtons &= ~g_dpadMask.load();
    return result;
}

void MCC::SetDpadMask(WORD buttons) { g_dpadMask = buttons; }

static XInputGetStateFn FindXInput() {
    const wchar_t* dlls[] = {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* dll : dlls) {
        HMODULE module = GetModuleHandleW(dll);
        if (!module) continue;
        auto fn = (XInputGetStateFn)GetProcAddress(module, "XInputGetState");
        if (fn) return fn;
    }
    return nullptr;
}

DWORD MCC::ReadController(DWORD user, XINPUT_STATE* state) {
    if (!g_xinputExport) g_xinputExport = FindXInput();
    XInputGetStateFn fn = g_origXInputGetState ? g_origXInputGetState : g_xinputExport;
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

void MCC::SetControllerBlocked(bool blocked) {
    if (g_controllerBlocked.exchange(blocked) != blocked)
        LOG("MCC controller input %s", blocked ? "blocked" : "restored");
}

static bool IsInModule(HMODULE module, void* p) {
    auto base = (char*)module;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// Done on the first title load rather than at startup: by then the game
// manager is definitely constructed.
static void HookInput() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto table = *(void***)((char*)exe + OFFSET_MCC_GAME_MANAGER);
    void* target = table && IsInModule(exe, table) ? table[MCC_GAME_MANAGER_GET_KEY_STATE_SLOT] : nullptr;

    if (!target || !IsInModule(exe, target)) {
        LOG("Game manager not found (table %p); input blocking unavailable", (void*)table);
        return;
    }

    if (MH_CreateHook(target, (void*)&GetKeyStateDetour, (void**)&g_origGetKeyState) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Failed to hook get_key_state at %p", target);
        return;
    }

    g_inputHooked = true;
    LOG("Hooked MCC get_key_state at %p", target);

    Coop::InstallHooks(table);

    // MCC's own menus read controllers through XInput, not through the game manager.
    g_xinputExport = FindXInput();
    if (g_xinputExport && MH_CreateHook((void*)g_xinputExport, (void*)&XInputGetStateDetour,
                                        (void**)&g_origXInputGetState) == MH_OK &&
        MH_EnableHook((void*)g_xinputExport) == MH_OK)
        LOG("Hooked XInputGetState");
    else
        LOG("Couldn't hook XInputGetState; MCC's menus will still see the controller while ours is open");
}

static void __fastcall ModuleLoadDetour(module_info_t* info, int a2, int64_t a3) {
    g_origModuleLoad(info, a2, a3);

    if (!g_inputHooked) HookInput();
    if (info && info->hModule) {
        Coop::OnModuleLoad(info->hModule);
        Games::OnModuleLoad(info->hModule);
    }
}

static int64_t __fastcall ModuleUnloadDetour(module_info_t* info) {
    if (info && info->hModule) {
        Coop::OnModuleUnload(info->hModule);
        Games::OnModuleUnload(info->hModule);
    }
    return g_origModuleUnload(info);
}

bool MCC::InstallHooks() {
    auto exe = (char*)GetModuleHandleW(nullptr);
    void* load = exe + OFFSET_MCC_MODULE_LOAD;
    void* unload = exe + OFFSET_MCC_MODULE_UNLOAD;

    if (MH_CreateHook(load, (void*)&ModuleLoadDetour, (void**)&g_origModuleLoad) != MH_OK ||
        MH_CreateHook(unload, (void*)&ModuleUnloadDetour, (void**)&g_origModuleUnload) != MH_OK ||
        MH_EnableHook(load) != MH_OK || MH_EnableHook(unload) != MH_OK) {
        LOG("Failed to hook MCC module load/unload");
        return false;
    }

    LOG("Hooked MCC module load/unload");
    return true;
}

static bool ReadRunning(MCC::Running& out) {
    __try {
        auto exe = (char*)GetModuleHandleW(nullptr);
        auto global = *(char**)(exe + OFFSET_MCC_GAME_GLOBAL_PTR);
        out.inGame = *(bool*)(exe + OFFSET_MCC_IN_GAME);
        out.titleId = global ? *(int32_t*)(global + MCC_GAME_GLOBAL_TITLE_ID) : -1;
        out.module = global ? *(HMODULE*)(global + MCC_GAME_GLOBAL_TITLE_MODULE) : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

MCC::Running MCC::GetRunning() {
    static Running last{false, -2, nullptr};

    Running info;
    if (!ReadRunning(info)) return Running{};

    if (info.inGame != last.inGame || info.titleId != last.titleId || info.module != last.module) {
        LOG("MCC reports: in game %d, title %d, module %p", info.inGame, info.titleId, (void*)info.module);
        last = info;
    }
    return info;
}

void MCC::SetInputBlocked(bool blocked) {
    if (g_inputBlocked.exchange(blocked) != blocked)
        LOG("Game input %s", blocked ? "blocked" : "restored");
}
