#pragma once

#include <windows.h>
#include <xinput.h>

// Hooks into MCC itself, shared by every title.
namespace MCC {
    // Hooks title DLL load/unload and forwards them to Games::.
    bool InstallHooks();

    // While blocked, every title sees no keyboard, mouse or controller input.
    void SetInputBlocked(bool blocked);

    // Reads a controller straight from XInput, never blocked (the menu and
    // co-op use this; MCC's own reads can be blocked, see SetControllerBlocked).
    DWORD ReadController(DWORD user, XINPUT_STATE* state);

    // While blocked, MCC's own controller reads (its menus included) see
    // every controller idle. Used by the menu when it's open on MCC's menu screens.
    void SetControllerBlocked(bool blocked);

    // XINPUT_GAMEPAD_DPAD_* bits MCC's own controller reads see as released
    // (killstreak mode: the mod reads them itself through ReadController).
    void SetDpadMask(WORD buttons);

    // The title being played, as MCC reports it.
    struct Running {
        bool inGame = false;  // false in MCC's menus
        int titleId = -1;     // 0 CE, 1 H2, 2 H3, 3 H4, 4 H2A MP, 5 ODST, 6 Reach
        HMODULE module = nullptr;
    };
    Running GetRunning();
}
