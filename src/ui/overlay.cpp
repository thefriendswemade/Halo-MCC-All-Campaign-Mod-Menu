#include "overlay.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"

#include "core/log.h"
#include "games/games.h"
#include "games/squads.h"
#include "gamepad.h"
#include "icon_codes.h"
#include "menu_config.h"
#include "settings_file.h"
#include "mcc/coop.h"
#include "mcc/mcc.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

constexpr ULONGLONG kBannerMs = 15000;

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

static PresentFn g_origPresent = nullptr;
static ResizeBuffersFn g_origResizeBuffers = nullptr;
static SetCursorPosFn g_origSetCursorPos = nullptr;
static ClipCursorFn g_origClipCursor = nullptr;

static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static ULONGLONG g_startTime = 0;

static volatile bool g_showMenu = false;
static bool g_menuLite = false;  // opened on MCC's own menu screens: only the co-op settings
static bool g_justOpened = false;
static bool g_blockUntilReleased = false;  // keeps the button that closed the menu out of the game
static Games::Settings g_settings;

// A short message in the middle of the screen (spawning: what's selected,
// what was spawned).
static std::string g_toast;
static ULONGLONG g_toastTime = 0;
constexpr ULONGLONG kToastMs = 2500;

static void Toast(const std::string& text) {
    g_toast = text;
    g_toastTime = GetTickCount64();
}
static Menu::Config g_menu = Menu::Defaults();  // the menu's own settings (all games)
static bool g_styleDirty = true;               // re-apply colours before the next frame
static bool g_fontDirty = true;                // rebuild the font before the next frame

// --- Killstreaks ------------------------------------------------------------
// One-shot actions (the menu's buttons and the key-bound ones) that can be
// pinned to a list and fired from the D-pad. Firing one bumps the same counter
// its menu button does.

struct Streak {
    const char* id;  // saved in the ini; never rename
    const char* label;
    uint32_t Games::Settings::*serial;
    int word;          // which feature word the title must support: 0 = features, 1 = features2, -1 = any
    uint64_t feature;
    bool (*usable)();  // optional extra condition (the option the action depends on is set up)
};

static const Streak kStreaks[] = {
    {"nuke", "Nuke (kill every AI)", &Games::Settings::nukeSerial, 0, Games::kNuke, nullptr},
    {"upgradeAi", "Upgrade AI now (one rank)", &Games::Settings::upgradeAiSerial, 0, Games::kUpgradeAi, nullptr},
    {"despawnAi", "Despawn all custom AI", &Games::Settings::despawnSerial, 0, Games::kDespawnAi, nullptr},
    {"randomTeams", "Randomize AI teams now", &Games::Settings::randomTeamsSerial, 0, Games::kRandomTeams, nullptr},
    {"randomSwap", "Swap all AI now", &Games::Settings::randomSwapSerial, 1, Games::kRandomSwap, nullptr},
    {"grenades", "Drop a grenade under every AI", &Games::Settings::grenadeSerial, 1, Games::kGrenadeDrop, nullptr},
    {"bombingRun", "Call in a bombing run", &Games::Settings::bombingRunSerial, 1, Games::kBombingRun, nullptr},
    {"saveCheckpoint", "Save checkpoint", &Games::Settings::saveCheckpointSerial, 0, Games::kCheckpoints, nullptr},
    {"loadCheckpoint", "Load checkpoint", &Games::Settings::loadCheckpointSerial, 0, Games::kCheckpoints, nullptr},
    {"upgradeVehicle", "Upgrade vehicle", &Games::Settings::upgradeVehicleSerial, 0, Games::kUpgradeVehicle, nullptr},
    {"giveWeapon", "Give weapon again", &Games::Settings::giveWeaponSerial, 0, Games::kGiveWeapon,
     [] { return g_settings.giveWeapon != 0xFFFFFFFF; }},
    {"giveEquipment", "Give equipment again", &Games::Settings::giveEquipmentSerial, 0, Games::kGiveEquipment,
     [] { return g_settings.giveEquipment != 0xFFFFFFFF; }},
    {"spawn", "Spawn selection", &Games::Settings::spawnSerial, -1, 0, [] { return g_settings.spawnEnabled; }},
    {"takeSeat", "Take pilot seat", &Games::Settings::takeSeatSerial, -1, 0, [] { return g_settings.pilotAiVehicles; }},
    {"teleport", "Teleport where you aim", &Games::Settings::teleportSerial, -1, 0,
     [] { return g_settings.teleportEnabled; }},
};

static ImFont* g_iconSmall = nullptr;  // the icon font (ui/icon_font.cpp), set by RebuildFont
static ImFont* g_iconLarge = nullptr;

static const char* IconUtf8(const char* id) {
    for (const IconCode& code : kIconCodes)
        if (!strcmp(code.id, id)) return code.utf8;
    return nullptr;
}

static uint64_t g_features[2] = {};  // what the running title supports, as of this frame
static bool g_armed = false;         // killstreak list armed: the D-pad belongs to the mod
static int g_streakSel = 0;

static std::vector<std::string>& PinnedIds(const char* title) { return g_menu.streaks[title ? title : ""]; }

static const Streak* FindStreak(const std::string& id) {
    for (const Streak& st : kStreaks)
        if (id == st.id) return &st;
    return nullptr;
}

static bool StreakAvailable(const Streak& st) {
    if (st.word >= 0 && !(g_features[st.word] & st.feature)) return false;
    return !st.usable || st.usable();
}

// The pinned actions that work right now, in pin order.
static std::vector<const Streak*> ActiveStreaks(const char* title) {
    std::vector<const Streak*> list;
    for (const std::string& id : PinnedIds(title)) {
        const Streak* st = FindStreak(id);
        if (st && StreakAvailable(*st)) list.push_back(st);
    }
    return list;
}

static void FireStreak(const Streak& st) {
    if (!strcmp(st.id, "spawn") && g_settings.spawnTag == 0xFFFFFFFF && g_settings.spawnSquad < 0) {
        Toast("Spawning: nothing selected");
        return;
    }
    ++(g_settings.*st.serial);
    LOG("Killstreak: %s", st.id);
    if (strcmp(st.id, "spawn")) Toast(st.label);  // spawning reports its own result
}

// --- Input ----------------------------------------------------------------

static LRESULT CALLBACK WndProcDetour(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_showMenu) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        // Keyboard and mouse go to the menu only. System keys (Alt+Tab,
        // Alt+F4, Alt+Enter) still reach the game window.
        bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool key = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR;
        if (mouse || key) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// The game recenters and confines the cursor for mouse look. Ignore that
// while the menu is open so the mouse can move freely.
static BOOL WINAPI SetCursorPosDetour(int x, int y) {
    if (g_showMenu) return TRUE;
    return g_origSetCursorPos(x, y);
}

static BOOL WINAPI ClipCursorDetour(const RECT* rect) {
    if (g_showMenu) return TRUE;
    return g_origClipCursor(rect);
}

// Keyboard state for bindings, read once per frame while the game window is
// focused. Mouse buttons and the left/right variants of Shift, Ctrl and Alt
// are skipped (the plain Shift/Ctrl/Alt codes cover them).
static bool g_keyDown[256] = {};
static bool g_keyPressed[256] = {};

static bool SkippedKey(int vk) {
    return vk < 0x08 || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == 0xFF;
}

static void PollKeys() {
    bool focused = GetForegroundWindow() == g_hwnd;
    for (int vk = 0; vk < 256; ++vk) {
        bool now = focused && !SkippedKey(vk) && (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_keyPressed[vk] = now && !g_keyDown[vk];
        g_keyDown[vk] = now;
    }
}

static bool AnyKeyDown() {
    for (bool down : g_keyDown)
        if (down) return true;
    return false;
}

// A binding fires when all its inputs are held and one of them went down this frame.
static bool ChordPressed(const Menu::Binding& b) {
    if (b.count == 0) return false;
    bool any = false;
    for (int i = 0; i < b.count; ++i) {
        uint32_t in = b.inputs[i];
        bool held, pressed;
        if (in & Menu::kPad) {
            uint32_t bit = 1u << (in & 0xFF);
            held = (Gamepad::HeldMask() & bit) != 0;
            pressed = (Gamepad::PressedMask() & bit) != 0;
        } else {
            int vk = in & 0xFF;
            held = g_keyDown[vk];
            pressed = g_keyPressed[vk];
        }
        if (!held) return false;
        any |= pressed;
    }
    return any;
}

static uint32_t PadMask(const Menu::Binding& b) {
    uint32_t mask = 0;
    for (int i = 0; i < b.count; ++i)
        if (b.inputs[i] & Menu::kPad) mask |= 1u << (b.inputs[i] & 0xFF);
    return mask;
}

// Capturing a new binding: the next `needed` distinct buttons (or keys) pressed
// become the binding. Menu navigation and closing pause meanwhile; it gives up
// after 10 seconds.
struct Capture {
    Menu::Binding* target = nullptr;
    const char* what = "";
    bool keyboard = false;
    int needed = 0;
    Menu::Binding got;
    ULONGLONG start = 0;
};
static Capture g_capture;
static bool g_waitForRelease = false;  // after a capture, until everything is let go

static void StartCapture(Menu::Binding* target, const char* what, bool keyboard, int needed) {
    g_capture = {target, what, keyboard, needed, {}, GetTickCount64()};
    LOG("Binding %s: press %d %s", what, needed, keyboard ? "keys" : "buttons");
}

static void AddCaptured(uint32_t input) {
    for (int i = 0; i < g_capture.got.count; ++i)
        if (g_capture.got.inputs[i] == input) return;
    if (g_capture.got.count < Menu::kMaxBindingInputs) g_capture.got.inputs[g_capture.got.count++] = input;
}

static void UpdateCapture() {
    if (!g_capture.target) return;
    if (GetTickCount64() - g_capture.start > 10000) {
        LOG("Binding %s: timed out, unchanged", g_capture.what);
        g_capture.target = nullptr;
        g_waitForRelease = true;
        return;
    }
    if (g_capture.keyboard) {
        for (int vk = 0; vk < 256; ++vk)
            if (g_keyPressed[vk]) AddCaptured(Menu::kKey | (uint32_t)vk);
    } else {
        uint32_t pressed = Gamepad::PressedMask();
        for (uint32_t bit = 0; bit < 18; ++bit)
            if (pressed & (1u << bit)) AddCaptured(Menu::kPad | bit);
    }
    if (g_capture.got.count >= g_capture.needed) {
        *g_capture.target = g_capture.got;
        LOG("Binding %s -> %s", g_capture.what, Menu::Describe(g_capture.got).c_str());
        g_capture.target = nullptr;
        g_waitForRelease = true;
    }
}

static bool MouseButtonHeld() {
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}

static void SetMenuOpen(bool open, bool lite = false) {
    if (open == g_showMenu) return;
    g_showMenu = open;
    g_justOpened = open;
    if (open) g_menuLite = lite;

    if (open && g_origClipCursor) {
        // Free the cursor, keeping it inside the game window.
        RECT rect;
        GetClientRect(g_hwnd, &rect);
        MapWindowPoints(g_hwnd, nullptr, (POINT*)&rect, 2);
        g_origClipCursor(&rect);
    }
    if (!open) g_blockUntilReleased = true;
    LOG("Menu %s", open ? "opened" : "closed");
}

// The menu opens during gameplay of a supported title (the full menu) and on
// MCC's own menu screens (just the co-op settings, `lite`). It closes when
// either changes into the other (a mission loading, back to MCC's menus).
static void UpdateMenuInput(bool inGame, bool lite, const char* title) {
    Gamepad::Poll();
    PollKeys();
    UpdateCapture();
    if (g_waitForRelease && !g_capture.target && !Gamepad::AnyHeld() && !AnyKeyDown()) g_waitForRelease = false;
    bool busy = g_capture.target || g_waitForRelease;
    // The tap that arms the killstreak list (D-pad down; Back + down stays a chord for the key-bound actions).
    bool arming = g_menu.killstreakMode && inGame && !lite && !g_armed && Gamepad::Pressed(XINPUT_GAMEPAD_DPAD_DOWN) &&
                  !Gamepad::Held(XINPUT_GAMEPAD_BACK);

    if (!g_showMenu) {
        if (!busy && (inGame || lite) && (ChordPressed(g_menu.openPad) || ChordPressed(g_menu.openKey)))
            SetMenuOpen(true, lite);
        // Spawning: the spawn key creates the selection where you're aiming.
        // While the killstreak list is armed the D-pad is its own, so the key-bound actions below are off
        // (they can still be pinned and fired from the list).
        else if (!busy && !g_armed && !arming && inGame && g_settings.spawnEnabled &&
                 (ChordPressed(Menu::SpawnPad(g_menu, title)) || ChordPressed(g_menu.spawnKey))) {
            if (g_settings.spawnTag == 0xFFFFFFFF && g_settings.spawnSquad < 0) {
                Toast("Spawning: nothing selected");
            } else {
                ++g_settings.spawnSerial;
                LOG("Spawn key pressed (tag %08X)", g_settings.spawnTag);
            }
        }
        // Take the pilot seat of the AI-only aircraft you're aiming at.
        else if (!busy && !g_armed && !arming && inGame && g_settings.pilotAiVehicles &&
                 (ChordPressed(g_menu.takeSeatPad) || ChordPressed(g_menu.takeSeatKey))) {
            ++g_settings.takeSeatSerial;
            LOG("Take-pilot-seat key pressed");
        }
        // Teleport where you're aiming (Fun page).
        else if (!busy && !g_armed && !arming && inGame && g_settings.teleportEnabled &&
                 (ChordPressed(Menu::TeleportPad(g_menu, title)) || ChordPressed(g_menu.teleportKey))) {
            ++g_settings.teleportSerial;
            LOG("Teleport key pressed");
        }
    } else if (!(g_menuLite ? lite : inGame) ||
               (!busy && (ChordPressed(g_menu.closePad) || ChordPressed(g_menu.openKey)))) {
        SetMenuOpen(false);
    }

    if (g_blockUntilReleased && !Gamepad::AnyHeld() && !MouseButtonHeld())
        g_blockUntilReleased = false;

    // Killstreak mode: D-pad down always goes to the mod (no title uses it); armed, the whole D-pad does.
    // The game never sees these, so the arming tap can't do anything in the game.
    bool streaks = g_menu.killstreakMode && inGame && !lite && !g_showMenu;
    if (!streaks) g_armed = false;
    else if (!busy) {
        if (!g_armed) {
            if (arming) {
                g_armed = true;
                LOG("Killstreaks armed");
            }
        } else if (Gamepad::Pressed(XINPUT_GAMEPAD_DPAD_LEFT)) {
            g_armed = false;
            LOG("Killstreaks disarmed");
        } else {
            std::vector<const Streak*> list = ActiveStreaks(title);
            int n = (int)list.size();
            if (n) {
                g_streakSel = g_streakSel < 0 || g_streakSel >= n ? 0 : g_streakSel;
                if (Gamepad::Pressed(XINPUT_GAMEPAD_DPAD_UP)) g_streakSel = (g_streakSel + n - 1) % n;
                if (Gamepad::Pressed(XINPUT_GAMEPAD_DPAD_DOWN)) g_streakSel = (g_streakSel + 1) % n;
                if (Gamepad::Pressed(XINPUT_GAMEPAD_DPAD_RIGHT)) FireStreak(*list[g_streakSel]);
            }
        }
    }
    MCC::SetDpadMask(!streaks ? 0
                     : g_armed ? (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                                  XINPUT_GAMEPAD_DPAD_RIGHT)
                               : XINPUT_GAMEPAD_DPAD_DOWN);

    MCC::SetInputBlocked(g_showMenu || g_blockUntilReleased);
    // On MCC's own screens the game manager isn't what reads the controller.
    MCC::SetControllerBlocked(g_menuLite && (g_showMenu || g_blockUntilReleased));
}

// --- Drawing --------------------------------------------------------------

// Menu-wide: a row's text wraps inside the menu instead of running off its
// edge. Checkboxes and sliders draw their widget with a hidden label and put
// the label beside it as wrapped text.
static void WrappedLabel(const char* label) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", label);
}

static bool WrappedCheckbox(const char* label, bool* value) {
    std::string id = std::string("##") + label;
    bool changed = ImGui::Checkbox(id.c_str(), value);
    WrappedLabel(label);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {  // the text toggles it too
        *value = !*value;
        changed = true;
    }
    return changed;
}

// Killstreak mode: a "Pin" checkbox for a one-shot action. Pinned actions
// join the title's list in the order they're pinned. `sameLine` puts it beside
// the button just drawn; otherwise it's a row of its own.
static void PinBox(const char* id, const Games::Status& s, bool sameLine = true) {
    if (!g_menu.killstreakMode) return;
    std::vector<std::string>& ids = PinnedIds(s.title);
    auto it = std::find(ids.begin(), ids.end(), id);
    bool pinned = it != ids.end();
    if (sameLine) ImGui::SameLine();
    ImGui::PushID(id);
    if (WrappedCheckbox("Pin to killstreaks", &pinned)) {
        if (pinned) ids.push_back(id);
        else ids.erase(std::remove(ids.begin(), ids.end(), std::string(id)), ids.end());
        LOG("Killstreak %s %s", id, pinned ? "pinned" : "unpinned");
    }
    ImGui::PopID();
}

// One row per cheat. Greyed out when the running title doesn't support it.
static bool Toggle(const char* label, bool* value, uint64_t feature, const Games::Status& s) {
    ImGui::BeginDisabled(s.title && !(s.features & feature));
    bool changed = WrappedCheckbox(label, value);
    ImGui::EndDisabled();
    if (changed) LOG("%s -> %d", label, *value);
    return changed;
}

// The same for the second feature word.
static bool Toggle2(const char* label, bool* value, uint64_t feature, const Games::Status& s) {
    ImGui::BeginDisabled(s.title && !(s.features2 & feature));
    bool changed = WrappedCheckbox(label, value);
    ImGui::EndDisabled();
    if (changed) LOG("%s -> %d", label, *value);
    return changed;
}

// Slider that also steps with D-pad left and right while focused, so
// controller users don't need to enter ImGui's slider tweak mode. With
// `snapToOne`, a step that crosses 1x stops on it (from 0.1, steps of 0.25
// would skip it) and values near 1 snap to it.
static bool StepSlider(const char* label, float* value, float min, float max, const char* format, float step,
                       bool snapToOne = false) {
    // Snapping to 1 is for multipliers (1.00x); a slider that steps by whole
    // numbers (a percentage) must not stop at 1 on its way from 0 to 10.
    snapToOne = snapToOne && step < 1.0f;
    float before = *value;
    std::string id = std::string("##") + label;
    ImGui::SliderFloat(id.c_str(), value, min, max, format);
    bool focused = ImGui::IsItemFocused(), active = ImGui::IsItemActive();
    WrappedLabel(label);
    if (focused && !active) {
        float stepped = *value;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            stepped -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            stepped += step;
        bool crossesOne = (*value < 1.0f && stepped > 1.0f) || (*value > 1.0f && stepped < 1.0f);
        *value = snapToOne && crossesOne && min < 1.0f && max > 1.0f ? 1.0f : stepped;
    }
    *value = *value < min ? min : (*value > max ? max : *value);
    if (snapToOne && fabsf(*value - 1.0f) < 0.02f) *value = 1.0f;
    if (*value != before && !active) LOG("%s -> %.2f", label, *value);
    return *value != before;
}

static void Multiplier(const char* label, float* value, float min, float max, uint64_t feature,
                       const Games::Status& s, const char* format = "%.2fx", float step = 0.25f) {
    ImGui::BeginDisabled(s.title && !(s.features & feature));
    StepSlider(label, value, min, max, format, step, true);
    ImGui::EndDisabled();
}

// Pick-one list of tags from the loaded map. `none` adds a first entry
// meaning "no choice" (datum 0xFFFFFFFF). Returns true when a row is picked.
static bool TagCombo(const char* label, uint32_t* datum, const std::vector<Games::TagChoice>* list,
                     const char* none, uint64_t feature, const Games::Status& s) {
    bool picked = false;
    ImGui::BeginDisabled((s.title && !(s.features & feature)) || !list || list->empty());
    const char* preview = none ? none : "(pick one)";
    if (!list || list->empty()) preview = "(start a mission)";
    else
        for (const auto& c : *list)
            if (c.datum == *datum) preview = c.name.c_str();

    if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge)) {
        if (none && ImGui::Selectable(none, *datum == 0xFFFFFFFF)) {
            *datum = 0xFFFFFFFF;
            picked = true;
        }
        for (const auto& c : *list) {
            ImGui::PushID((int)c.datum);
            if (ImGui::Selectable(c.name.c_str(), c.datum == *datum)) {
                *datum = c.datum;
                picked = true;
                LOG("%s -> %s (%08X)", label, c.name.c_str(), c.datum);
            }
            if (c.datum == *datum) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    return picked;
}

// Menu pages: a main page listing categories, each opening a submenu. The
// open page is remembered when the menu closes. Add new categories to Page
// and kPageNames, then a Draw...Page function and a case in DrawMenu.
enum class Page {
    Main, Player, Weapons, Combat, Equipment, Vehicles, Ai, World, Spawning, Fun, Chaos, Skulls, GameState,
    Coop, MenuSettings, Credits, Count
};
static const char* const kPageNames[] = {"Main",     "Player", "Weapons", "Combat", "Equipment",
                                         "Vehicles", "AI",     "World",   "Spawning", "Fun",
                                         "Chaos",    "Skulls", "Game State", "Co-op", "Menu Settings", "Credits"};
static_assert(sizeof(kPageNames) / sizeof(kPageNames[0]) == (size_t)Page::Count, "a name per page");

static Page g_page = Page::Main;
static bool g_pageChanged = false;  // focus the first row of the new page

static void GoTo(Page page) {
    g_page = page;
    g_pageChanged = true;
    LOG("Menu page -> %s", kPageNames[(int)page]);
}

// Puts the controller/keyboard cursor on the next row when the menu opens or
// the page changes.
static void FocusFirstRow() {
    if (!g_justOpened && !g_pageChanged) return;
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNavCursorVisible(true);
}

static void DrawMainPage() {
    FocusFirstRow();
    for (int i = (int)Page::Main + 1; i < (int)Page::Count; ++i) {
        char label[64];
        sprintf_s(label, "%s  >", kPageNames[i]);
        if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) GoTo((Page)i);
    }
}

static bool IsCE(const Games::Status& s) { return s.title && !strcmp(s.title, "Halo: CE"); }
static bool IsHalo2(const Games::Status& s) { return s.title && !strcmp(s.title, "Halo 2"); }
static bool IsHalo3(const Games::Status& s) { return s.title && !strcmp(s.title, "Halo 3"); }
static bool IsOdst(const Games::Status& s) { return s.title && !strcmp(s.title, "Halo 3: ODST"); }
// Titles whose campaign has no squad of AI Spartans (the rest wording mentions them).
static bool NoAiSpartans(const Games::Status& s) { return IsCE(s) || IsHalo2(s) || IsHalo3(s) || IsOdst(s); }

static void DrawPlayerPage(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Survival");
    Toggle("God mode", &c.godMode, Games::kGodMode, s);

    // Switch sides: the AI treat you as one of the team you pick.
    ImGui::SeparatorText("Team");
    ImGui::BeginDisabled(s.title && !(s.features & Games::kPlayerTeam));
    const char* const kYourTeams[] = {"Normal", "Covenant (humans fight you)", "Rogue (everyone fights you)",
                                      "Flood (Flood fight for you; humans and Covenant fight you)"};
    int teamCount = (IsCE(s) || IsHalo2(s) || IsHalo3(s)) ? (int)_countof(kYourTeams) : 3;  // Flood only where the game has them
    int yourTeam = c.playerTeam >= 0 && c.playerTeam < teamCount ? c.playerTeam : 0;
    if (ImGui::BeginCombo("Your team", kYourTeams[yourTeam])) {
        for (int i = 0; i < teamCount; ++i)
            if (ImGui::Selectable(kYourTeams[i], i == yourTeam)) {
                c.playerTeam = i;
                LOG("Your team -> %s", kYourTeams[i]);
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Movement");
    Multiplier("Speed", &c.speed, 0.25f, 5.0f, Games::kSpeed, s);
    Multiplier("Jump height", &c.jump, 0.25f, 5.0f, Games::kJump, s);
    // Halo 4 only (the other titles have no sprint limit): hidden elsewhere.
    if (s.features2 & Games::kInfiniteSprint)
        Toggle2("Infinite sprint (no stamina)", &c.infiniteSprint, Games::kInfiniteSprint, s);
    Toggle2("Double jump (jump again in mid-air)", &c.doubleJump, Games::kDoubleJump, s);
    ImGui::BeginDisabled(!c.doubleJump || c.infiniteJump || (s.title && !(s.features2 & Games::kDoubleJump)));
    StepSlider("Air jumps", &c.airJumps, 1.0f, 10.0f, "%.0f", 1.0f);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!c.doubleJump);
    Toggle2("Infinite jump (no limit)", &c.infiniteJump, Games::kDoubleJump, s);
    ImGui::EndDisabled();
    Toggle("Fly (Acrophobia skull)", &c.fly, Games::kFly, s);
    Toggle("Noclip (hold RB / G to fly, Shift fast)", &c.noclip, Games::kNoclip, s);

    ImGui::SeparatorText("View");
    // Third person puts the dot on where shots land, so turning it on turns the dot on (not off again).
    if (Toggle("Third person", &c.thirdPerson, Games::kThirdPerson, s) && c.thirdPerson) c.dot = true;
    ImGui::BeginDisabled(!c.thirdPerson);
    // Fine steps up close, then whole units out to a drone's-eye view.
    Multiplier("Camera distance", &c.cameraDistance, 0.0f, 30.0f, Games::kCameraDistance, s, "+%.2f",
               c.cameraDistance < 3.0f ? 0.1f : 1.0f);
    ImGui::EndDisabled();
    // Field of view: the game's own number until it's moved (0 = untouched).
    if (s.features2 & Games::kFov) {
        float shown = c.fov > 0.0f ? c.fov : s.fovDefault;
        if (StepSlider("Field of view", &shown, s.fovMin, s.fovMax, "%.0f", 1.0f))
            c.fov = shown == s.fovDefault ? 0.0f : shown;
    }
    Toggle("No HUD", &c.noHud, Games::kNoHud, s);

    ImGui::SeparatorText("Reticle");
    Toggle2("Permanent reticle dot (stays with the HUD hidden; marks where shots land in third person)", &c.dot,
            Games::kDot, s);
    ImGui::BeginDisabled(!c.dot);
    ImGui::ColorEdit3("Dot color", c.dotColor, ImGuiColorEditFlags_NoInputs);
    StepSlider("Dot size", &c.dotSize, 1.0f, 10.0f, "%.0f", 1.0f);
    ImGui::BeginDisabled(!c.thirdPerson);
    StepSlider("Third-person offset, left / right (pixels)", &c.dotOffsetX, -300.0f, 300.0f, "%.0f", 1.0f);
    StepSlider("Third-person offset, up / down (pixels)", &c.dotOffsetY, -200.0f, 200.0f, "%.0f", 1.0f);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

static void DrawWeaponsPage(Games::Settings& c, const Games::Status& s) {
    // Picking a weapon gives it straight away, replacing the held one.
    ImGui::SeparatorText("Give");
    if (TagCombo("Give weapon", &c.giveWeapon, s.weapons, nullptr, Games::kGiveWeapon, s)) ++c.giveWeaponSerial;
    ImGui::BeginDisabled(c.giveWeapon == 0xFFFFFFFF || (s.title && !(s.features & Games::kGiveWeapon)));
    if (ImGui::Button("Give again")) ++c.giveWeaponSerial;
    ImGui::EndDisabled();
    PinBox("giveWeapon", s);

    ImGui::SeparatorText("Ammo");
    Toggle("Infinite ammo", &c.infiniteAmmo, Games::kInfiniteAmmo, s);
    Toggle("Bottomless clip", &c.bottomlessClip, Games::kBottomlessClip, s);
    Toggle("Infinite grenades", &c.infiniteGrenades, Games::kInfiniteGrenades, s);
    TagCombo("Ammo type", &c.ammoType, s.projectiles, "Normal", Games::kAmmoType, s);

    ImGui::SeparatorText("Firing");
    Toggle("Full-auto (semi-auto weapons)", &c.fullAuto, Games::kFullAuto, s);
    Toggle("Rapid fire", &c.rapidFire, Games::kRapidFire, s);
    Toggle("No spread", &c.noSpread, Games::kNoSpread, s);
    Toggle("No recoil", &c.noRecoil, Games::kNoRecoil, s);
    Toggle("No overheat", &c.noOverheat, Games::kNoOverheat, s);

    // Explosions: how far their damage reaches (the radius skull's job, with a dial).
    if (s.features2 & Games::kExplosionSize) {
        ImGui::SeparatorText("Explosions");
        StepSlider("Explosion size (blast radius)", &c.explosionScale, 0.25f, 5.0f, "%.2fx", 0.25f, true);
    }
}

static void DrawCombatPage(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Damage");
    Toggle("One-hit kill", &c.oneHitKill, Games::kOneHitKill, s);
    Multiplier("Damage to enemies", &c.damage, 0.25f, 10.0f, Games::kDamage, s);

    ImGui::SeparatorText("Friendly fire");
    Toggle("Disable friendly fire (you and your allies can't hurt each other)", &c.noFriendlyFire,
           Games::kNoFriendlyFire, s);

    ImGui::SeparatorText("Aimbot");
    Toggle("Aimbot (bullets bend to targets)", &c.aimbot, Games::kAimbot, s);
    // Halo 2 only: in the other titles it isn't greyed out, it isn't shown.
    if (s.features2 & Games::kSwordFly)
        Toggle2("Perfect sword fly (the sword sees targets at any range)", &c.swordFly, Games::kSwordFly, s);
    // 0% = bullets bend only; higher pulls your view onto enemies while firing, 100% snaps to any in view.
    ImGui::BeginDisabled(!c.aimbot);
    Multiplier("Aimbot strength", &c.aimbotStrength, 0.0f, 100.0f, Games::kAimbot, s, "%.0f%%", 10.0f);
    ImGui::EndDisabled();
}

static void DrawEquipmentPage(Games::Settings& c, const Games::Status& s) {
    // Picking one gives it straight away, replacing the current armor ability.
    ImGui::SeparatorText("Give");
    if (TagCombo("Give equipment", &c.giveEquipment, s.equipment, nullptr, Games::kGiveEquipment, s)) ++c.giveEquipmentSerial;
    ImGui::BeginDisabled(c.giveEquipment == 0xFFFFFFFF || (s.title && !(s.features & Games::kGiveEquipment)));
    if (ImGui::Button("Give equipment again")) ++c.giveEquipmentSerial;
    ImGui::EndDisabled();
    PinBox("giveEquipment", s);

    ImGui::SeparatorText("Equipment");
    Toggle("Infinite equipment / armor ability", &c.infiniteEquipment, Games::kInfiniteEquipment, s);
    Toggle("No equipment despawn", &c.noEquipmentDespawn, Games::kNoEquipmentDespawn, s);
    Toggle("Perfect camo (moving doesn't reveal you)", &c.perfectCamo, Games::kPerfectCamo, s);
    Toggle("Envy skull (camo on the flashlight button)", &c.envySkull, Games::kEnvySkull, s);
}

static void BindingRow(const char* label, Menu::Binding* binding, bool keyboard, int* count);

static void DrawVehiclesPage(Games::Settings& c, const Games::Status& s) {
    // Applies to the vehicle you're in; the game shares its settings with every vehicle of that type.
    ImGui::SeparatorText("Handling");
    Multiplier("Vehicle speed", &c.vehicleSpeed, 0.25f, 5.0f, Games::kVehicleSpeed, s);
    // Higher holds turns tighter (the speed setting already scales grip with it; this helps boosting).
    Multiplier("Vehicle grip (broken)", &c.vehicleGrip, 0.5f, 5.0f, Games::kVehicleSpeed, s);
    Toggle("Infinite boost", &c.vehicleInfiniteBoost, Games::kVehicleInfiniteBoost, s);

    ImGui::SeparatorText("Vehicle weapons");
    Toggle("Vehicle rapid fire", &c.vehicleRapidFire, Games::kVehicleRapidFire, s);
    Toggle("No vehicle recoil", &c.vehicleNoRecoil, Games::kVehicleNoRecoil, s);

    ImGui::SeparatorText("Protection");
    Toggle("Vehicle god mode", &c.vehicleGodMode, Games::kVehicleGodMode, s);
    Toggle("Cannot be hijacked", &c.vehicleNoHijack, Games::kVehicleNoHijack, s);
    Toggle("Cannot be ejected (flipping)", &c.vehicleNoEject, Games::kVehicleNoEject, s);
    Toggle("Cannot be shut down (EMP)", &c.vehicleNoEmp, Games::kVehicleNoEmp, s);

    ImGui::SeparatorText("AI-only vehicles");
    Toggle("Use AI-only vehicles and seats (Pelican, Phantom...)", &c.pilotAiVehicles, Games::kPilotAiVehicles, s);
    // With that on: aim at one (up close), or any vehicle someone else pilots, and press the key to
    // throw its pilot out and take the seat.
    ImGui::BeginDisabled(!c.pilotAiVehicles || (s.title && !(s.features & Games::kTakePilotSeat)));
    static int padCount = 0, keyCount = 0;
    if (padCount == 0) padCount = g_menu.takeSeatPad.count > 0 ? g_menu.takeSeatPad.count : 2;
    if (keyCount == 0) keyCount = g_menu.takeSeatKey.count > 0 ? g_menu.takeSeatKey.count : 1;
    BindingRow("Take pilot seat (controller)", &g_menu.takeSeatPad, false, &padCount);
    BindingRow("Take pilot seat (keyboard)", &g_menu.takeSeatKey, true, &keyCount);
    ImGui::EndDisabled();
    // Swaps the vehicle you're in for the bigger one where you are, with you as its pilot.
    ImGui::BeginDisabled(s.title && !(s.features & Games::kUpgradeVehicle));
    if (ImGui::Button("Upgrade vehicle (Falcon > Pelican, Banshee > Phantom)")) ++c.upgradeVehicleSerial;
    ImGui::EndDisabled();
    PinBox("upgradeVehicle", s);
    // Take pilot seat is bound to a key rather than a button, so it gets a row of its own.
    PinBox("takeSeat", s, false);
}

static void DrawAiPage(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Senses and aim");
    Toggle("Enemies can't see you", &c.aiBlind, Games::kAiBlind, s);
    Toggle("AI can't hear", &c.aiDeaf, Games::kAiDeaf, s);
    Toggle("AI can't shoot", &c.aiCantShoot, Games::kAiCantShoot, s);

    // Kills every AI in the level (the mission's and the ones you spawned). Invulnerable AI (story
    // Spartans) come back unless "Invulnerable AI can die" is on.
    ImGui::SeparatorText("Allies");
    Toggle("Allies never turn against you (you can still kill them)", &c.noBetrayal, Games::kNoBetrayal, s);

    ImGui::SeparatorText("Killing AI");
    Toggle(NoAiSpartans(s) ? "Invulnerable AI can die" : "Invulnerable AI can die (Spartans stay dead)", &c.aiStayDead,
           Games::kAiStayDead, s);
    ImGui::BeginDisabled(s.title && !(s.features & Games::kNuke));
    if (ImGui::Button("Nuke (kill every AI)")) {
        ++c.nukeSerial;
        LOG("Nuke pressed");
    }
    ImGui::EndDisabled();
    PinBox("nuke", s);
    Toggle("Nuke spares your allies", &c.nukeSpareAllies, Games::kNuke, s);

    // Each AI becomes the next rank up (grunt > major > heavy > ultra > spec ops) where it stands, in
    // its squad. AI in vehicles wait until they get out.
    ImGui::SeparatorText("Upgrade AI");
    bool upgrade = !s.title || ((s.features & Games::kUpgradeAi) &&
                                (s.upgradeEnemiesApplicable || (s.upgradeAlliesApplicable && c.upgradeAiAllies)));
    ImGui::BeginDisabled(!upgrade);
    if (ImGui::Button("Upgrade AI now (one rank)")) {
        ++c.upgradeAiSerial;
        LOG("Upgrade AI pressed");
    }
    ImGui::EndDisabled();
    PinBox("upgradeAi", s);
    Toggle("Auto-upgrade AI as they appear", &c.upgradeAiAuto, upgrade ? Games::kUpgradeAi : 0, s);
    Toggle(NoAiSpartans(s) ? "Allies too (marines)" : "Allies too (troopers)", &c.upgradeAiAllies,
           s.upgradeAlliesApplicable ? Games::kUpgradeAi : 0, s);
    ImGui::BeginDisabled(!upgrade);
    StepSlider("Upgrade range", &c.upgradeAiRange, 0.0f, 150.0f, c.upgradeAiRange > 0.0f ? "%.0f m" : "everywhere",
               5.0f);
    ImGui::EndDisabled();
}

static void DrawWorldPage(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Physics");
    Multiplier("Gravity", &c.gravity, 0.1f, 3.0f, Games::kGravity, s);
    Multiplier("Game speed", &c.gameSpeed, 0.25f, 3.0f, Games::kGameSpeed, s);

    ImGui::SeparatorText("Level");
    Toggle("Remove boundaries (experimental)", &c.noBoundaries, Games::kNoBoundaries, s);

    ImGui::SeparatorText("Show boundaries");
    Toggle2("Draw kill volumes (wireframe)", &c.showBoundaries, Games::kBoundaryView, s);
    Toggle2("Draw soft ceilings (invisible walls and ceilings)", &c.showSoftCeilings, Games::kBoundaryView, s);
    Toggle2("Soft ceilings that don't affect you too (grey)", &c.softCeilingsAll, Games::kBoundaryView, s);
    Toggle2("Hide boundaries behind walls and objects", &c.boundaryHide, Games::kBoundaryView, s);
    ImGui::BeginDisabled(s.title && !(s.features2 & Games::kBoundaryView));
    StepSlider("Boundary opacity", &c.boundaryOpacity, 0.05f, 0.8f, "%.2f", 0.05f);
    {
        // Steps: 2 4 6 8 10, then tens up to 200, then everywhere (stored as 0).
        static const float kRanges[] = {2, 4, 6, 8, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 0};
        constexpr int kCount = (int)(sizeof(kRanges) / sizeof(kRanges[0]));
        float index = 4;
        for (int i = 0; i < kCount; ++i)
            if (kRanges[i] == c.boundaryRange) index = (float)i;
        char text[32];
        int shown = (int)index;
        if (kRanges[shown] > 0) snprintf(text, sizeof(text), "%.0f units", kRanges[shown]);
        else snprintf(text, sizeof(text), "everywhere");
        if (StepSlider("Boundary range", &index, 0.0f, (float)(kCount - 1), text, 1.0f))
            c.boundaryRange = kRanges[(int)(index + 0.5f)];
    }
    ImGui::EndDisabled();
}

static void DrawGameStatePage(Games::Settings& c, const Games::Status& s) {
    // Save: an immediate checkpoint, wherever you are. Load: the same as MCC's
    // "load last checkpoint".
    ImGui::BeginDisabled(s.title && !(s.features & Games::kCheckpoints));
    if (ImGui::Button("Save checkpoint")) {
        ++c.saveCheckpointSerial;
        LOG("Save checkpoint pressed");
    }
    ImGui::EndDisabled();
    PinBox("saveCheckpoint", s);
    ImGui::BeginDisabled(s.title && !(s.features & Games::kCheckpoints));
    if (ImGui::Button("Load checkpoint")) {
        ++c.loadCheckpointSerial;
        LOG("Load checkpoint pressed");
    }
    ImGui::EndDisabled();
    PinBox("loadCheckpoint", s);

    ImGui::SeparatorText("Lock");
    Toggle("Lock checkpoints (the game saves no new ones)", &c.lockCheckpoints, Games::kCheckpointLock, s);

    // Every checkpoint saved this mission, newest first. Restoring one makes
    // it the current checkpoint and loads it.
    ImGui::SeparatorText("Checkpoint history (this mission)");
    bool historyOn = !s.title || (s.features & Games::kCheckpointHistory);
    ImGui::BeginDisabled(!historyOn);
    if (!s.checkpoints || s.checkpoints->empty()) {
        ImGui::TextDisabled(historyOn ? "No checkpoints yet. They appear as the game saves them." : "Not available in this game.");
    } else {
        int64_t now = (int64_t)time(nullptr);
        const auto& list = *s.checkpoints;
        for (size_t n = list.size(); n-- > 0;) {
            const Games::CheckpointEntry& e = list[n];
            int64_t age = now - e.when;
            char ago[32];
            if (age < 90) snprintf(ago, sizeof(ago), "%lld s ago", (long long)age);
            else snprintf(ago, sizeof(ago), "%lld min ago", (long long)(age / 60));
            ImGui::PushID((int)e.id);
            char text[96];
            // "###" keeps the button's identity fixed while the age in its text changes.
            snprintf(text, sizeof(text), "Go back to %s  (%s)%s###restore", e.label.c_str(), ago, n + 1 == list.size() ? "  - latest" : "");
            if (ImGui::Button(text)) {
                c.restoreCheckpoint = e.id;
                ++c.restoreCheckpointSerial;
                LOG("Restore checkpoint %u pressed (%s)", e.id, e.label.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndDisabled();
}

// --- Menu Settings page ------------------------------------------------------

static void ApplyStyle() {
    const Menu::Config& m = g_menu;
    if (m.theme == Menu::kThemeLight) ImGui::StyleColorsLight();
    else if (m.theme == Menu::kThemeClassic) ImGui::StyleColorsClassic();
    else ImGui::StyleColorsDark();

    auto col = [](const float* v, float alpha = -1.0f, float scale = 1.0f) {
        return ImVec4(v[0] * scale, v[1] * scale, v[2] * scale, alpha < 0.0f ? v[3] : alpha);
    };
    ImVec4* c = ImGui::GetStyle().Colors;
    const float* a = m.accent;
    c[ImGuiCol_WindowBg] = col(m.windowBg);
    c[ImGuiCol_PopupBg] = col(m.windowBg, 0.98f);
    c[ImGuiCol_Text] = col(m.text);
    c[ImGuiCol_TextDisabled] = col(m.text, 0.5f);
    c[ImGuiCol_CheckMark] = col(a, 1.0f);
    c[ImGuiCol_SliderGrab] = col(a, 1.0f, 0.9f);
    c[ImGuiCol_SliderGrabActive] = col(a, 1.0f);
    c[ImGuiCol_Button] = col(a, 0.40f);
    c[ImGuiCol_ButtonHovered] = col(a, 1.0f);
    c[ImGuiCol_ButtonActive] = col(a, 1.0f, 0.8f);
    c[ImGuiCol_Header] = col(a, 0.31f);
    c[ImGuiCol_HeaderHovered] = col(a, 0.80f);
    c[ImGuiCol_HeaderActive] = col(a, 1.0f);
    c[ImGuiCol_FrameBg] = col(a, 0.20f, 0.5f);
    c[ImGuiCol_FrameBgHovered] = col(a, 0.40f);
    c[ImGuiCol_FrameBgActive] = col(a, 0.67f);
    c[ImGuiCol_TitleBgActive] = col(a, 1.0f, 0.55f);
    c[ImGuiCol_SeparatorHovered] = col(a, 0.78f);
    c[ImGuiCol_SeparatorActive] = col(a, 1.0f);
    c[ImGuiCol_NavCursor] = col(a, 1.0f);
    c[ImGuiCol_ResizeGrip] = col(a, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = col(a, 0.67f);
    c[ImGuiCol_ResizeGripActive] = col(a, 0.95f);
}

// Rebuilds the font atlas (before NewFrame); the DX11 backend re-uploads it.
static void RebuildFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_iconSmall = g_iconLarge = nullptr;
    ImFont* font = nullptr;
    const wchar_t* file = Menu::kFontFiles[g_menu.font];
    if (file) {
        wchar_t dir[MAX_PATH];
        UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%.*ls\\Fonts\\%ls", (int)n, dir, file);
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) font = io.Fonts->AddFontFromFileTTF(utf8, g_menu.fontSize);
        if (!font) LOG("Menu font %ls not found; using the default", file);
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = g_menu.fontSize;
        io.Fonts->AddFontDefault(&config);
    }

    // The killstreak icons: two sizes of the embedded icon font (the list's icons and the selected one).
    static const ImWchar iconRange[] = {kIconFirst, kIconLast, 0};
    ImFontConfig iconConfig;
    iconConfig.FontDataOwnedByAtlas = false;  // the bytes live in the DLL
    g_iconSmall = io.Fonts->AddFontFromMemoryTTF((void*)kIconFontData, (int)kIconFontSize, 48.0f, &iconConfig, iconRange);
    g_iconLarge = io.Fonts->AddFontFromMemoryTTF((void*)kIconFontData, (int)kIconFontSize, 96.0f, &iconConfig, iconRange);
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

// One binding row: its current inputs, and a button that captures new ones.
static void BindingRow(const char* label, Menu::Binding* binding, bool keyboard, int* count) {
    ImGui::PushID(label);
    bool capturing = g_capture.target == binding;
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    if (capturing) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "press %d %s (%d/%d)...", g_capture.needed,
                           keyboard ? "keys" : "buttons", g_capture.got.count, g_capture.needed);
    } else {
        ImGui::TextDisabled("%s", Menu::Describe(*binding).c_str());
    }
    if (count) {
        float n = (float)*count;
        ImGui::BeginDisabled(capturing);
        if (StepSlider(keyboard ? "Keys" : "Buttons", &n, 1.0f, (float)Menu::kMaxBindingInputs, "%.0f", 1.0f))
            *count = (int)(n + 0.5f);
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(g_capture.target != nullptr);
    if (ImGui::Button(count ? "Reset binding" : "Rebind")) StartCapture(binding, label, keyboard, count ? *count : 1);
    ImGui::EndDisabled();
    if (capturing) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_capture.target = nullptr;
            g_waitForRelease = true;
        }
    }
    ImGui::PopID();
}

// --- Spawning page ---------------------------------------------------------

// Name of a tag in any of the spawn lists, or nullptr.
static const char* SpawnName(const Games::Status& s, uint32_t datum) {
    for (const auto* list : {s.weapons, s.spawnVehicles, s.spawnObjects, s.spawnAi})
        if (list)
            for (const auto& c : *list)
                if (c.datum == datum) return c.name.c_str();
    return nullptr;
}

// One kind of thing to spawn, as a collapsible list. Picking a row makes it
// the selection and closes every list.
static bool g_collapseSpawnLists = false;

static void SpawnList(const char* label, const std::vector<Games::TagChoice>* list, Games::Settings& c) {
    if (g_collapseSpawnLists) ImGui::SetNextItemOpen(false);
    if (!ImGui::CollapsingHeader(label)) return;
    ImGui::PushID(label);
    if (!list) ImGui::TextDisabled("    Not available yet in this game");
    else if (list->empty()) ImGui::TextDisabled("    (start a mission)");
    else
        for (const auto& t : *list) {
            ImGui::PushID((int)t.datum);
            if (ImGui::Selectable(t.name.c_str(), t.datum == c.spawnTag)) {
                c.spawnTag = t.datum;
                c.spawnSquad = -1;
                g_collapseSpawnLists = true;  // applied to every list next frame
                Toast("Spawning: " + t.name);
                LOG("Spawn selection -> %s (%08X)", t.name.c_str(), t.datum);
            }
            ImGui::PopID();
        }
    ImGui::PopID();
}

static void BindingRow(const char* label, Menu::Binding* binding, bool keyboard, int* count);

// Squads: several AI at once, in a cluster where you aim. Picking one
// selects it like any other row (the spawn key then makes the whole squad).
static void SquadList(Games::Settings& c, const Games::Status& s) {
    if (g_collapseSpawnLists) ImGui::SetNextItemOpen(false);
    if (!ImGui::CollapsingHeader("Squads")) return;
    ImGui::PushID("Squads");
    if (!s.title || !(s.features & Games::kSquadSpawning)) {
        ImGui::TextDisabled("    Not available yet in this game");
    } else {
        const char* group = nullptr;
        for (int i = 0; i < Games::kSquadCount; ++i) {
            const Games::Squad& squad = Games::kSquads[i];
            if (!Games::SquadInTitle(squad, s.title)) continue;  // each game has its own list
            if (!group || strcmp(group, squad.group)) {
                group = squad.group;
                ImGui::SeparatorText(group);
            }
            bool available = Games::SquadAvailable(s, i);
            ImGui::BeginDisabled(!available);
            ImGui::PushID(i);
            char label[96];
            sprintf_s(label, available ? "%s" : "%s  (not in this map)", squad.name);
            if (ImGui::Selectable(label, c.spawnSquad == i)) {
                c.spawnSquad = i;
                g_collapseSpawnLists = true;
                Toast(std::string("Spawning squad: ") + squad.name);
                LOG("Spawn squad selection -> %s", squad.name);
            }
            ImGui::PopID();
            ImGui::EndDisabled();
        }
    }
    ImGui::PopID();
}

// The rows above the lists: the spawn key, then how spawned AI behave, then
// despawning and the current selection (right above the lists it picks from).
static void DrawSpawnControls(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Spawn key");
    Toggle("Enabled", &c.spawnEnabled, Games::kSpawning, s);
    ImGui::TextWrapped("Aim at a spot and press the spawn key to create the selection there.");

    // The controller key is per game (see Menu::SpawnPad).
    Menu::Binding& pad = Menu::SpawnPad(g_menu, s.title);
    static int padCount = 0, keyCount = 0;
    static const char* padTitle = nullptr;
    if (padCount == 0 || padTitle != s.title) padCount = pad.count > 0 ? pad.count : 1;
    padTitle = s.title;
    if (keyCount == 0) keyCount = g_menu.spawnKey.count > 0 ? g_menu.spawnKey.count : 1;
    BindingRow("Spawn key (controller)", &pad, false, &padCount);
    BindingRow("Spawn key (keyboard)", &g_menu.spawnKey, true, &keyCount);
    PinBox("spawn", s, false);

    ImGui::SeparatorText("Spawned AI");
    // Which side spawned AI fight for. The saved number is the same in every game
    // (0 their own, 1 ally, 2 Covenant, 3 Flood, 4 rogue, 5 random); Flood is only
    // listed in the games that have them.
    const char* const kTeamNames[] = {"Their own team",
                                      "Ally (whoever's on your side)",
                                      "Covenant (fights you)",
                                      "Flood",
                                      "Rogue (fights everyone)",
                                      "Random team"};
    bool hasFlood = s.title && (!strcmp(s.title, "Halo: CE") || !strcmp(s.title, "Halo 2") || !strcmp(s.title, "Halo 3"));
    int team = c.spawnTeam >= 0 && c.spawnTeam < (int)_countof(kTeamNames) ? c.spawnTeam : 0;
    if (team == 3 && !hasFlood) team = 0;
    if (ImGui::BeginCombo("AI team", kTeamNames[team])) {
        for (int i = 0; i < (int)_countof(kTeamNames); ++i) {
            if (i == 3 && !hasFlood) continue;
            if (ImGui::Selectable(kTeamNames[i], i == team)) {
                c.spawnTeam = i;
                LOG("Spawn AI team -> %s", kTeamNames[i]);
            }
        }
        ImGui::EndCombo();
    }
    if (IsCE(s) || IsHalo2(s)) {  // CE's actor variants come with their own weapon; Halo 2 picks a loadout the mission uses
        ImGui::BeginDisabled(true);
        WrappedCheckbox("Random guns (not in this game)", &c.spawnRandomGuns);
        ImGui::EndDisabled();
    } else {
        Toggle("Random guns (Elites, Brutes, Grunts, marines)", &c.spawnRandomGuns, Games::kSquadSpawning, s);
    }
    WrappedCheckbox("Enable respawn (spawned AI come back after dying)", &c.spawnRespawn);

    ImGui::SeparatorText("Spawn");
    ImGui::BeginDisabled(s.title && !(s.features & Games::kDespawnAi));
    if (ImGui::Button("Despawn all custom AI")) {
        ++c.despawnSerial;
        LOG("Despawn all custom AI pressed");
    }
    ImGui::EndDisabled();
    PinBox("despawnAi", s);

    const char* name = SpawnName(s, c.spawnTag);
    char selection[128];
    if (c.spawnSquad >= 0 && c.spawnSquad < Games::kSquadCount)
        sprintf_s(selection, "Squad: %s", Games::kSquads[c.spawnSquad].name);
    else
        sprintf_s(selection, "%s", c.spawnTag == 0xFFFFFFFF ? "(nothing selected)" : name ? name : "(not in this map)");
    ImGui::BeginDisabled(true);
    ImGui::InputText("Current selection", selection, sizeof(selection), ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
}

static void DrawSpawningPage(Games::Settings& c, const Games::Status& s) {
    bool supported = !s.title || (s.features & Games::kSpawning);
    ImGui::BeginDisabled(!supported);
    DrawSpawnControls(c, s);

    bool collapse = g_collapseSpawnLists;
    SpawnList("AI", s.spawnAi, c);
    SquadList(c, s);
    SpawnList("Vehicles", s.spawnVehicles, c);
    SpawnList("Objects", s.spawnObjects, c);
    SpawnList("Weapons", s.weapons, c);
    if (collapse) g_collapseSpawnLists = false;
    ImGui::EndDisabled();
}

// --- Fun, Chaos and Skulls pages ---------------------------------------------

static void DrawFunPage(Games::Settings& c, const Games::Status& s) {
    // Every shot shoves you away from where you aim: shoot the floor to fly.
    Toggle("Recoil push (also pushes your vehicle)", &c.recoilPush, Games::kRecoilPush, s);
    ImGui::BeginDisabled(!c.recoilPush);
    Multiplier("Push per shot", &c.recoilStrength, 0.5f, 20.0f, Games::kRecoilPush, s, "%.1f", 0.5f);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Hits");
    Toggle("Launch on hit (every hit sends the enemy flying)", &c.launchOnHit, Games::kLaunchOnHit, s);
    ImGui::BeginDisabled(!c.launchOnHit);
    if (IsCE(s)) {
        // CE's gravity is lower, so 6 already throws them very high: that is the top of
        // the scale here, with fine steps below it.
        if (c.launchStrength > 6.0f) c.launchStrength = 6.0f;
        Multiplier("Launch speed", &c.launchStrength, 0.25f, 6.0f, Games::kLaunchOnHit, s, "%.2f", 0.25f);
    } else if (s.title && !strcmp(s.title, "Halo 4")) {
        // Halo 4's units are heavier: 12 is already far enough.
        if (c.launchStrength > 12.0f) c.launchStrength = 12.0f;
        Multiplier("Launch speed", &c.launchStrength, 2.0f, 12.0f, Games::kLaunchOnHit, s, "%.0f", 2.0f);
    } else {
        Multiplier("Launch speed", &c.launchStrength, 2.0f, 60.0f, Games::kLaunchOnHit, s, "%.0f", 2.0f);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Size");
    ImGui::BeginDisabled(s.title && !(s.features2 & Games::kAiScale));
    StepSlider("Enemy AI size", &c.aiScaleEnemy, 0.1f, 10.0f, "%.2fx", 0.25f, true);
    StepSlider("Friendly AI size", &c.aiScaleFriendly, 0.1f, 10.0f, "%.2fx", 0.25f, true);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Time");
    Toggle("Time stop (every AI freezes; you keep moving)", &c.timeStop, Games::kTimeStop, s);

    ImGui::SeparatorText("Touch");
    Toggle("Poison touch (AI that touch you die, friend or foe)", &c.poisonTouch, Games::kPoisonTouch, s);

    ImGui::SeparatorText("Teleport");
    Toggle("Teleport key (to where you're aiming)", &c.teleportEnabled, Games::kTeleport, s);
    ImGui::BeginDisabled(!c.teleportEnabled || (s.title && !(s.features & Games::kTeleport)));
    Menu::Binding& teleportPad = Menu::TeleportPad(g_menu, s.title);
    static int padCount = 0, keyCount = 0;
    static const char* teleportTitle = nullptr;
    if (padCount == 0 || teleportTitle != s.title) padCount = teleportPad.count > 0 ? teleportPad.count : 1;
    teleportTitle = s.title;
    if (keyCount == 0) keyCount = g_menu.teleportKey.count > 0 ? g_menu.teleportKey.count : 1;
    BindingRow("Teleport (controller)", &teleportPad, false, &padCount);
    BindingRow("Teleport (keyboard)", &g_menu.teleportKey, true, &keyCount);
    ImGui::EndDisabled();
    PinBox("teleport", s, false);
}

static void DrawChaosPage(Games::Settings& c, const Games::Status& s) {
    ImGui::SeparatorText("Chaos gun");
    Toggle("Chaos gun (every shot is a random projectile)", &c.chaosGun, Games::kChaosGun, s);

    ImGui::SeparatorText("Rain");
    Toggle("Make it rain (things drop from the sky around you)", &c.rain, Games::kRain, s);
    ImGui::BeginDisabled(!c.rain || (s.title && !(s.features & Games::kRain)));
    static const char* const kKinds[Games::kRainKindCount] = {"Everything", "Vehicles", "Objects",
                                                              "AI",         "Weapons",  "The spawn selection"};
    int kind = c.rainKind >= 0 && c.rainKind < Games::kRainKindCount ? c.rainKind : 0;
    if (ImGui::BeginCombo("Raining", kKinds[kind])) {
        for (int i = 0; i < Games::kRainKindCount; ++i)
            if (ImGui::Selectable(kKinds[i], i == kind)) {
                c.rainKind = i;
                LOG("Rain kind -> %s", kKinds[i]);
            }
        ImGui::EndCombo();
    }
    StepSlider("Every", &c.rainInterval, 0.25f, 10.0f, "%.2f s", 0.25f);
    StepSlider("At most at once", &c.rainMax, 1.0f, 60.0f, "%.0f", 1.0f);
    StepSlider("Radius", &c.rainRadius, 5.0f, 75.0f, "%.0f m", 1.0f);
    ImGui::EndDisabled();

    // Humans (your side) are one team; every other team fights everyone.
    ImGui::SeparatorText("Random AI teams");
    bool teams = !s.title || (s.features & Games::kRandomTeams);
    Toggle("Random team for each new AI", &c.randomTeamsAuto, Games::kRandomTeams, s);
    if (!NoAiSpartans(s))  // CE, Halo 2, Halo 3 and ODST have no AI Spartans
        Toggle("Spartans too", &c.randomTeamsSpartans, Games::kRandomTeams, s);
    ImGui::BeginDisabled(!teams);
    StepSlider("Teams", &c.randomTeamsFactions, 2.0f, IsCE(s) ? 5.0f : 6.0f, "%.0f", 1.0f);
    StepSlider("Team range", &c.randomTeamsRange, 0.0f, 150.0f, c.randomTeamsRange > 0.0f ? "%.0f m" : "everywhere",
               5.0f);
    if (ImGui::Button("Randomize AI teams now")) {
        ++c.randomTeamsSerial;
        LOG("Randomize AI teams pressed");
    }
    ImGui::EndDisabled();
    PinBox("randomTeams", s);

    // Each AI becomes a fresh one of a random character, where it stands, in its squad.
    ImGui::SeparatorText("Random AI swap");
    bool swap = !s.title || (s.features2 & Games::kRandomSwap);
    Toggle2("Random character for each new AI", &c.randomSwapAuto, Games::kRandomSwap, s);
    Toggle2(NoAiSpartans(s) ? "Allies too (Marines)" : "Allies too (Marines, Spartans)",
            &c.randomSwapAllies, Games::kRandomSwap, s);
    ImGui::BeginDisabled(!swap);
    if (ImGui::Button("Swap all AI now")) {
        ++c.randomSwapSerial;
        LOG("Swap all AI pressed");
    }
    ImGui::EndDisabled();
    PinBox("randomSwap", s);

    // A grenade at the feet of AI: all of them now, and/or each new one as it appears.
    if (s.features2 & Games::kGrenadeDrop) {
        ImGui::SeparatorText("We all feel down sometimes");
        if (ImGui::Button("Drop a grenade under every AI")) {
            ++c.grenadeSerial;
            LOG("Drop grenades pressed");
        }
        PinBox("grenades", s);
        Toggle2("Enemies only", &c.grenadeEnemiesOnly, Games::kGrenadeDrop, s);
        Toggle2("Do this to all new AI (the game's and your own)", &c.grenadeAuto, Games::kGrenadeDrop, s);
    }

    // Flying vehicles come in side by side and carpet the ground ahead with grenades.
    if (s.features2 & Games::kBombingRun) {
        ImGui::SeparatorText("Bombing run");
        if (ImGui::Button("Call in a bombing run")) {
            ++c.bombingRunSerial;
            LOG("Bombing run pressed");
        }
        PinBox("bombingRun", s);
        static const char* const kOrdnance[] = {"Marines (frag grenades)", "Covenant (plasma grenades)",
                                                "Brutes (spike grenades)", "White phosphorus (firebombs)"};
        int ordnance = c.bombingRunType >= 0 && c.bombingRunType < 4 ? c.bombingRunType : 0;
        if (ImGui::BeginCombo("Ordnance", kOrdnance[ordnance])) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(kOrdnance[i], i == ordnance)) {
                    c.bombingRunType = i;
                    LOG("Bombing run ordnance -> %s", kOrdnance[i]);
                }
            ImGui::EndCombo();
        }
        float abreast = (float)c.bombingRunCount;
        if (StepSlider("Aircraft abreast", &abreast, 1.0f, 12.0f, "%.0f", 1.0f)) c.bombingRunCount = (int)(abreast + 0.5f);
        ImGui::TextDisabled("Starts a countdown, then they fly over the area ahead of you. Take cover.");
    }
}

static void DrawSkullsPage(Games::Settings& c, const Games::Status& s) {
    bool supported = !s.title || (s.features & Games::kSkulls);
    if (!supported || !s.skullNames) {
        ImGui::TextDisabled("Not available yet in this game");
        return;
    }
    ImGui::TextWrapped("Ticked skulls are forced on. Skulls you picked in MCC stay on either way (shown as \"on\").");
    for (int i = 0; i < s.skullCount && i < 64; ++i) {
        const char* name = s.skullNames[i];
        if (!name) continue;
        uint64_t bit = 1ull << i;
        bool forced = (c.skulls & bit) != 0;
        char label[96];
        bool fromGame = !forced && (s.skullsActive & bit);
        sprintf_s(label, "%s%s", name, fromGame ? "  (on)" : "");
        ImGui::PushID(i);
        if (WrappedCheckbox(label, &forced)) {
            c.skulls = forced ? (c.skulls | bit) : (c.skulls & ~bit);
            LOG("Skull %s -> %d", name, forced);
        }
        ImGui::PopID();
    }
}

// --- Co-op and Credits pages --------------------------------------------------

// Local co-op / splitscreen (ported from AlphaRing). Not per game: the same
// settings apply to every title. The game reads who is playing when a mission
// loads, so change these, then restart the mission or load a checkpoint.
static void DrawCoopPage(bool lite = false) {
    Coop::Settings& c = Coop::Get();
    static const char* const kPads[] = {"Controller 1", "Controller 2", "Controller 3", "Controller 4", "None"};

    ImGui::SeparatorText("Local co-op");
    if (WrappedCheckbox("Enable local co-op", &c.enabled)) LOG("Co-op -> %d", c.enabled);
    float players = (float)c.players;
    if (StepSlider("Players", &players, 1.0f, 4.0f, "%.0f", 1.0f)) c.players = (int)(players + 0.5f);
    if (WrappedCheckbox("Player 1 uses keyboard and mouse", &c.p1KeyMouse)) {
        Coop::AssignDefaultControllers();
        LOG("Co-op: player 1 keyboard and mouse -> %d", c.p1KeyMouse);
    }

    ImGui::SeparatorText("Controllers");
    for (int i = 0; i < c.players; ++i) {
        char label[16];
        sprintf_s(label, "Player %d", i + 1);
        ImGui::BeginDisabled(i == 0 && c.p1KeyMouse);
        int pick = i == 0 && c.p1KeyMouse ? 4 : c.controller[i];
        if (ImGui::BeginCombo(label, kPads[pick])) {
            for (int n = 0; n < 5; ++n)
                if (ImGui::Selectable(kPads[n], n == pick)) {
                    c.controller[i] = n;
                    LOG("Co-op: player %d -> %s", i + 1, kPads[n]);
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    if (c.p1KeyMouse) ImGui::TextDisabled("Player 1 is on keyboard and mouse.");

    if (!lite) {
        ImGui::BeginDisabled(!Coop::Active());
        if (ImGui::Button("Reload player settings")) Coop::ReloadSettings();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Makes everyone use player 1's field of view and button layout.");
    }

    ImGui::SeparatorText("Screen");
    WrappedCheckbox("Remove black bars (Halo 3, ODST, Reach)", &c.removeBlackBars);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (lite)
        ImGui::TextWrapped(
            "These settings are for every title. Turn co-op on here, then start the mission from MCC's menu: "
            "Halo: CE counts its players when the mission starts. Everyone shares player 1's profile and button "
            "layout. Anti-cheat must be off. Settings are saved.");
    else
    ImGui::TextWrapped(
        "Everyone shares player 1's profile and button layout. The game picks up the players when a mission "
        "loads: after changing these, restart the mission or load a checkpoint. If two players are swapped, "
        "change their controller here (Windows numbers controllers in the order it finds them). Anti-cheat must "
        "be off. Settings are saved.");
    ImGui::PopStyleColor();
}

static void DrawCreditsPage() {
    ImGui::SeparatorText("Halo Mod Menu");
    ImGui::TextWrapped("The whole menu, every cheat and every title's support, was made by Th3FriendsWeMade.");

    ImGui::SeparatorText("AlphaRing");
    ImGui::TextWrapped("Local co-op is based on AlphaRing by WinterSquire (with contributions from Priception and "
                       "xTrxplex).");
    ImGui::TextDisabled("github.com/WinterSquire/AlphaRing");

    ImGui::SeparatorText("AlphaRing, updated");
    ImGui::TextWrapped("Th3FriendsWeMade updated AlphaRing for MCC 1.3528.0.0: player 1 on a controller and saved "
                       "splitscreen settings. Then brought it into this menu.");
    ImGui::TextDisabled("github.com/nrkorte/AlphaRing-Local-Co-Op-Mod");

    ImGui::SeparatorText("Libraries");
    ImGui::TextWrapped("Dear ImGui (Omar Cornut), MinHook (Tsuda Kageyu), Assembly (XboxChaos) tag research.");

    ImGui::SeparatorText("Killstreak icons");
    ImGui::TextWrapped("Icons from Game-icons.net by Lorc, Delapouite and Skoll, licensed CC BY 3.0 (recoloured and "
                       "combined).");
    ImGui::TextDisabled("game-icons.net  |  creativecommons.org/licenses/by/3.0");
}

static void DrawMenuSettingsPage() {
    Menu::Config& m = g_menu;
    // How many inputs the open bindings get on the next "Reset binding".
    static int padCount = 0, keyCount = 0;
    if (padCount == 0) padCount = m.openPad.count > 0 ? m.openPad.count : 2;
    if (keyCount == 0) keyCount = m.openKey.count > 0 ? m.openKey.count : 1;

    ImGui::SeparatorText("Theme");
    if (ImGui::BeginCombo("Theme", Menu::kThemeNames[m.theme])) {
        for (int i = 0; i < Menu::kThemeCount; ++i)
            if (ImGui::Selectable(Menu::kThemeNames[i], i == m.theme)) {
                Menu::ApplyThemeColors(m, i);
                g_styleDirty = true;
                LOG("Menu theme -> %s", Menu::kThemeNames[i]);
            }
        ImGui::EndCombo();
    }
    const ImGuiColorEditFlags colorFlags = ImGuiColorEditFlags_AlphaBar;
    g_styleDirty |= ImGui::ColorEdit4("Accent", m.accent, colorFlags);
    g_styleDirty |= ImGui::ColorEdit4("Background", m.windowBg, colorFlags);
    g_styleDirty |= ImGui::ColorEdit4("Text", m.text, colorFlags);
    ImGui::ColorEdit4("Active-cheat list", m.hudText, colorFlags);

    ImGui::SeparatorText("Layout");
    StepSlider("Width", &m.width, 320.0f, 900.0f, "%.0f px", 20.0f);
    StepSlider("Max height", &m.maxHeight, 0.0f, 1400.0f, m.maxHeight > 0.0f ? "%.0f px" : "fit page", 50.0f);
    if (ImGui::BeginCombo("Font", Menu::kFontNames[m.font])) {
        for (int i = 0; i < Menu::kFontCount; ++i)
            if (ImGui::Selectable(Menu::kFontNames[i], i == m.font) && i != m.font) {
                m.font = i;
                g_fontDirty = true;
                LOG("Menu font -> %s", Menu::kFontNames[i]);
            }
        ImGui::EndCombo();
    }
    // The font is rebuilt once the slider is let go (or on each controller step).
    if (StepSlider("Font size", &m.fontSize, 10.0f, 28.0f, "%.0f px", 1.0f) && !ImGui::IsItemActive())
        g_fontDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) g_fontDirty = true;
    ImGui::Text("Opens at %.0f, %.0f", m.posX, m.posY);
    if (ImGui::Button("Use current position")) {
        ImVec2 pos = ImGui::GetWindowPos();
        m.posX = pos.x;
        m.posY = pos.y;
        LOG("Menu position -> %.0f, %.0f", m.posX, m.posY);
    }
    ImGui::SameLine();
    if (ImGui::Button("Default position")) {
        m.posX = 40.0f;
        m.posY = 40.0f;
    }

    ImGui::SeparatorText("Controls");
    BindingRow("Open menu (controller)", &m.openPad, false, &padCount);
    BindingRow("Open menu (keyboard)", &m.openKey, true, &keyCount);
    BindingRow("Select", &m.selectPad, false, nullptr);
    BindingRow("Back", &m.backPad, false, nullptr);
    BindingRow("Close", &m.closePad, false, nullptr);

    ImGui::SeparatorText("Overlay");
    WrappedCheckbox("Show active-cheat list", &m.showHud);
    WrappedCheckbox("Show startup banner", &m.showBanner);

    ImGui::SeparatorText("Killstreaks");
    WrappedCheckbox("Killstreak mode (D-pad down arms it; D-pad down no longer reaches the game)", &m.killstreakMode);
    char rows[48];
    snprintf(rows, sizeof(rows), "Icons shown in the list: %d (click for %d)", m.streakRows, m.streakRows == 3 ? 5 : 3);
    if (ImGui::Button(rows)) m.streakRows = m.streakRows == 3 ? 5 : 3;
}

// Puts one page's options back to their defaults (the main page's reset puts
// back everything for this game).
static void ResetPage(Page page, Games::Settings& c, const char* title) {
    Games::Settings d;
    Games::DefaultDotOffset(title, d.dotOffsetX, d.dotOffsetY);
    switch (page) {
        case Page::Main: {
            Games::Settings old = c;  // don't re-trigger a give, save or load
            c = d;
            c.giveWeaponSerial = old.giveWeaponSerial;
            c.giveEquipmentSerial = old.giveEquipmentSerial;
            c.spawnSerial = old.spawnSerial;
            c.upgradeVehicleSerial = old.upgradeVehicleSerial;
            c.takeSeatSerial = old.takeSeatSerial;
            c.teleportSerial = old.teleportSerial;
            c.upgradeAiSerial = old.upgradeAiSerial;
            c.randomSwapSerial = old.randomSwapSerial;
            c.randomTeamsSerial = old.randomTeamsSerial;
            c.nukeSerial = old.nukeSerial;
            c.grenadeSerial = old.grenadeSerial;
            c.bombingRunSerial = old.bombingRunSerial;
            c.despawnSerial = old.despawnSerial;
            c.saveCheckpointSerial = old.saveCheckpointSerial;
            c.loadCheckpointSerial = old.loadCheckpointSerial;
            c.restoreCheckpointSerial = old.restoreCheckpointSerial;
            break;
        }
        case Page::Player:
            c.godMode = d.godMode; c.speed = d.speed; c.jump = d.jump; c.fly = d.fly;
            c.infiniteSprint = d.infiniteSprint; c.doubleJump = d.doubleJump; c.infiniteJump = d.infiniteJump; c.airJumps = d.airJumps;
            c.fov = d.fov; c.noclip = d.noclip; c.thirdPerson = d.thirdPerson; c.cameraDistance = d.cameraDistance; c.dot = d.dot; c.dotSize = d.dotSize; c.dotOffsetX = d.dotOffsetX; c.dotOffsetY = d.dotOffsetY; memcpy(c.dotColor, d.dotColor, sizeof(c.dotColor));
            c.noHud = d.noHud; c.playerTeam = d.playerTeam;
            break;
        case Page::Weapons:
            c.giveWeapon = d.giveWeapon; c.infiniteAmmo = d.infiniteAmmo; c.bottomlessClip = d.bottomlessClip;
            c.infiniteGrenades = d.infiniteGrenades; c.ammoType = d.ammoType; c.fullAuto = d.fullAuto; c.rapidFire = d.rapidFire;
            c.noSpread = d.noSpread; c.noRecoil = d.noRecoil; c.noOverheat = d.noOverheat;
            c.explosionScale = d.explosionScale;
            break;
        case Page::Combat:
            c.oneHitKill = d.oneHitKill; c.damage = d.damage; c.aimbot = d.aimbot;
            c.aimbotStrength = d.aimbotStrength; c.noFriendlyFire = d.noFriendlyFire; c.swordFly = d.swordFly;
            break;
        case Page::Equipment:
            c.infiniteEquipment = d.infiniteEquipment; c.noEquipmentDespawn = d.noEquipmentDespawn;
            c.perfectCamo = d.perfectCamo;
            c.envySkull = d.envySkull;
            break;
        case Page::Vehicles:
            c.vehicleSpeed = d.vehicleSpeed; c.vehicleGrip = d.vehicleGrip; c.vehicleRapidFire = d.vehicleRapidFire;
            c.vehicleNoRecoil = d.vehicleNoRecoil; c.vehicleGodMode = d.vehicleGodMode;
            c.vehicleInfiniteBoost = d.vehicleInfiniteBoost; c.vehicleNoHijack = d.vehicleNoHijack;
            c.vehicleNoEject = d.vehicleNoEject; c.pilotAiVehicles = d.pilotAiVehicles; c.vehicleNoEmp = d.vehicleNoEmp;
            g_menu.takeSeatPad = Menu::Defaults().takeSeatPad;
            g_menu.takeSeatKey = Menu::Defaults().takeSeatKey;
            break;
        case Page::Ai:
            c.aiBlind = d.aiBlind; c.aiCantShoot = d.aiCantShoot; c.aiDeaf = d.aiDeaf;
            c.aiStayDead = d.aiStayDead; c.nukeSpareAllies = d.nukeSpareAllies; c.noBetrayal = d.noBetrayal;
            c.upgradeAiAuto = d.upgradeAiAuto; c.upgradeAiAllies = d.upgradeAiAllies;
            c.upgradeAiRange = d.upgradeAiRange;
            break;
        case Page::World:
            c.gravity = d.gravity; c.gameSpeed = d.gameSpeed; c.noBoundaries = d.noBoundaries;
            break;
        case Page::Spawning:
            c.spawnEnabled = d.spawnEnabled; c.spawnTag = d.spawnTag; c.spawnTeam = d.spawnTeam; c.spawnSquad = d.spawnSquad;
            c.spawnRespawn = d.spawnRespawn; c.spawnRandomGuns = d.spawnRandomGuns;
            Menu::SpawnPad(g_menu, title) = Menu::DefaultSpawnPad(title);
            g_menu.spawnKey = Menu::Defaults().spawnKey;
            break;
        case Page::Fun:
            c.recoilPush = d.recoilPush; c.recoilStrength = d.recoilStrength; c.teleportEnabled = d.teleportEnabled;
            c.poisonTouch = d.poisonTouch; c.timeStop = d.timeStop;
            c.launchOnHit = d.launchOnHit; c.launchStrength = d.launchStrength;
            c.aiScaleEnemy = d.aiScaleEnemy; c.aiScaleFriendly = d.aiScaleFriendly;
            Menu::TeleportPad(g_menu, title) = Menu::DefaultTeleportPad(title);
            g_menu.teleportKey = Menu::Defaults().teleportKey;
            break;
        case Page::Chaos:
            c.chaosGun = d.chaosGun; c.rain = d.rain; c.rainKind = d.rainKind; c.rainInterval = d.rainInterval;
            c.rainMax = d.rainMax; c.rainRadius = d.rainRadius;
            c.randomTeamsAuto = d.randomTeamsAuto; c.randomTeamsSpartans = d.randomTeamsSpartans;
            c.randomSwapAuto = d.randomSwapAuto; c.randomSwapAllies = d.randomSwapAllies;
            c.randomTeamsFactions = d.randomTeamsFactions; c.randomTeamsRange = d.randomTeamsRange;
            c.grenadeEnemiesOnly = d.grenadeEnemiesOnly; c.grenadeAuto = d.grenadeAuto;
            c.bombingRunCount = d.bombingRunCount; c.bombingRunType = d.bombingRunType;
            break;
        case Page::Skulls:
            c.skulls = d.skulls;
            break;
        case Page::MenuSettings:
            g_menu = Menu::Defaults();
            g_styleDirty = g_fontDirty = true;
            break;
        case Page::Coop:
            Coop::Reset();
            break;
        case Page::Credits:
        case Page::GameState:
        case Page::Count:
            break;
    }
    LOG("Reset: %s", page == Page::Main ? "all settings for this game" : kPageNames[(int)page]);
}

// The menu on MCC's own screens: just the co-op settings (all titles at once)
// and the credits.
static void DrawLiteMenu() {
    ImGui::SetNextWindowPos(ImVec2(g_menu.posX, g_menu.posY), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(g_menu.width, 0), ImVec2(g_menu.width, FLT_MAX));
    if (g_justOpened) ImGui::SetNextWindowFocus();
    bool open = true;
    if (ImGui::Begin("Halo Mod Menu", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Local co-op  (all titles)");
        ImGui::Separator();
        if (g_justOpened) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::PushID("lite");
        DrawCoopPage(true);
        ImGui::Separator();
        if (ImGui::Button("Reset co-op")) Coop::Reset();
        if (ImGui::CollapsingHeader("Credits")) DrawCreditsPage();
        ImGui::PopID();

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Controller: D-pad move and adjust, right stick scroll, %s select, %s close",
                           Menu::Describe(g_menu.selectPad).c_str(), Menu::Describe(g_menu.closePad).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
    g_justOpened = false;
    if (!open) SetMenuOpen(false);
}

static void DrawMenu(const Games::Status& s) {
    // Opens where the Menu Settings say; fixed width; the height fits the
    // page (up to the max height, then it scrolls).
    ImGui::SetNextWindowPos(ImVec2(g_menu.posX, g_menu.posY), ImGuiCond_Appearing);
    // Never taller than the screen below where it opens (long lists scroll).
    float screen = ImGui::GetIO().DisplaySize.y - g_menu.posY - 20.0f;
    float maxHeight = g_menu.maxHeight > 0.0f ? g_menu.maxHeight : FLT_MAX;
    if (screen > 200.0f && maxHeight > screen) maxHeight = screen;
    ImGui::SetNextWindowSizeConstraints(ImVec2(g_menu.width, 0), ImVec2(g_menu.width, maxHeight));
    if (g_justOpened) ImGui::SetNextWindowFocus();

    Games::Settings& c = g_settings;
    Page page = g_page;  // the page drawn this frame
    bool open = true;
    if (ImGui::Begin("Halo Mod Menu", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* game = s.title ? s.title : "No game running";
        if (g_page == Page::Main) {
            if (s.title && !s.supported) ImGui::Text("%s  (not supported yet)", game);
            else if (!s.title) ImGui::TextDisabled("%s", game);
            else ImGui::Text("%s", game);
        } else {
            ImGui::Text("%s  >  %s", game, kPageNames[(int)g_page]);
        }
        ImGui::Separator();

        // Back (Y) or Backspace goes up to the main page, unless a dropdown is open.
        bool busy = g_capture.target || g_waitForRelease;
        bool up = !busy && (ChordPressed(g_menu.backPad) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
        if (page != Page::Main && up && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) GoTo(Page::Main);
        if (page != Page::Main) {
            FocusFirstRow();
            if (ImGui::Button("<  Back", ImVec2(-FLT_MIN, 0))) GoTo(Page::Main);
        }
        // Each page is drawn under its own ID so rows with the same label on
        // different pages don't share state.
        ImGui::PushID((int)page);
        switch (page) {
            case Page::Main: DrawMainPage(); break;
            case Page::Player: DrawPlayerPage(c, s); break;
            case Page::Weapons: DrawWeaponsPage(c, s); break;
            case Page::Combat: DrawCombatPage(c, s); break;
            case Page::Equipment: DrawEquipmentPage(c, s); break;
            case Page::Vehicles: DrawVehiclesPage(c, s); break;
            case Page::Ai: DrawAiPage(c, s); break;
            case Page::World: DrawWorldPage(c, s); break;
            case Page::Spawning: DrawSpawningPage(c, s); break;
            case Page::Fun: DrawFunPage(c, s); break;
            case Page::Chaos: DrawChaosPage(c, s); break;
            case Page::Skulls: DrawSkullsPage(c, s); break;
            case Page::GameState: DrawGameStatePage(c, s); break;
            case Page::Coop: DrawCoopPage(); break;
            case Page::Credits: DrawCreditsPage(); break;
            case Page::MenuSettings: DrawMenuSettingsPage(); break;
            case Page::Count: break;
        }
        ImGui::PopID();

        // Main page: reset everything for this game. A category: just its options.
        if (page != Page::GameState && page != Page::Credits) {
            ImGui::Separator();
            const char* reset = page == Page::Main ? "Reset all"
                                : page == Page::MenuSettings ? "Reset menu settings" : "Reset page";
            if (ImGui::Button(reset)) ResetPage(page, c, s.title);
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Controller: D-pad move and adjust, right stick scroll, %s select, %s back, %s close",
                           Menu::Describe(g_menu.selectPad).c_str(), Menu::Describe(g_menu.backPad).c_str(),
                           Menu::Describe(g_menu.closePad).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();

    g_justOpened = false;
    // A page change made this frame keeps the flag so the new page focuses
    // its first row next frame.
    if (g_page == page) g_pageChanged = false;
    if (!open) SetMenuOpen(false);
}

// Active cheats listed in the corner so it's obvious what's on.
// The permanent reticle dot. Drawn by the mod, so it survives the blind skull
// and a hidden HUD. In third person the gun is to one side of the camera, so
// shots land left of the game's reticle: the dot takes a fixed offset (tuned
// in Reach, adjustable in the menu).
static void DrawDot(const Games::Status& s, ImDrawList* draw) {
    const Games::Settings& c = g_settings;
    if (!c.dot || !s.inGame || g_menuLite || !(s.features2 & Games::kDot)) return;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float scale = size.y / 1080.0f;
    ImVec2 at(size.x * 0.5f, size.y * 0.5f);
    if (c.thirdPerson) {
        at.x += c.dotOffsetX * scale;
        at.y += c.dotOffsetY * scale;
    }
    float r = c.dotSize * scale;
    ImU32 fill = IM_COL32((int)(c.dotColor[0] * 255), (int)(c.dotColor[1] * 255), (int)(c.dotColor[2] * 255), 255);
    draw->AddCircleFilled(at, r + 1.5f, IM_COL32(0, 0, 0, 200), 16);
    draw->AddCircleFilled(at, r, fill, 16);
}

// Boundary view: kill-volume wireframes and soft-ceiling triangles, projected
// with the game's camera. Halo's axes: x forward, y left, z up.
static void DrawBoundaries(const Games::Status& s, ImDrawList* draw) {
    const Games::BoundaryView* v = s.boundaryView;
    if (!v || !v->valid || !s.inGame || g_menuLite) return;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float scale = (size.y * 0.5f) / v->tanHalfV;  // pixels per unit of (sideways / depth)
    float left[3] = {v->up[1] * v->fwd[2] - v->up[2] * v->fwd[1], v->up[2] * v->fwd[0] - v->up[0] * v->fwd[2],
                     v->up[0] * v->fwd[1] - v->up[1] * v->fwd[0]};
    // To camera space: (depth, left, up).
    auto toCam = [&](const float* p, float* out) {
        float d[3] = {p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2]};
        out[0] = d[0] * v->fwd[0] + d[1] * v->fwd[1] + d[2] * v->fwd[2];
        out[1] = d[0] * left[0] + d[1] * left[1] + d[2] * left[2];
        out[2] = d[0] * v->up[0] + d[1] * v->up[1] + d[2] * v->up[2];
    };
    auto toScreen = [&](const float* c) { return ImVec2(size.x * 0.5f - c[1] / c[0] * scale, size.y * 0.5f - c[2] / c[0] * scale); };
    const float kNear = 0.15f;

    auto drawLine = [&](const float* wa, const float* wb, ImU32 color, float thickness) {
        float a[3], b[3];
        toCam(wa, a);
        toCam(wb, b);
        if (a[0] < kNear && b[0] < kNear) return;
        if (a[0] < kNear) {  // clip to the near plane
            float t = (kNear - a[0]) / (b[0] - a[0]);
            for (int k = 0; k < 3; ++k) a[k] += (b[k] - a[k]) * t;
        } else if (b[0] < kNear) {
            float t = (kNear - b[0]) / (a[0] - b[0]);
            for (int k = 0; k < 3; ++k) b[k] += (a[k] - b[k]) * t;
        }
        draw->AddLine(toScreen(a), toScreen(b), color, thickness);
    };
    for (const auto& l : v->lines) drawLine(l.a, l.b, l.color, 2.0f);

    // Soft ceilings: each triangle clipped to the near plane, then filled and outlined.
    for (const auto& t : v->tris) {
        float c[4][3];
        int n = 0;
        float in[3][3];
        for (int m = 0; m < 3; ++m) toCam(t.v[m], in[m]);
        float nearestDepth = std::max(in[0][0], std::max(in[1][0], in[2][0]));
        if (nearestDepth < kNear) continue;
        for (int m = 0; m < 3; ++m) {
            const float* a = in[m];
            const float* b = in[(m + 1) % 3];
            if (a[0] >= kNear) { memcpy(c[n++], a, sizeof(float) * 3); }
            if ((a[0] >= kNear) != (b[0] >= kNear)) {
                float k = (kNear - a[0]) / (b[0] - a[0]);
                for (int j = 0; j < 3; ++j) c[n][j] = a[j] + (b[j] - a[j]) * k;
                ++n;
            }
        }
        if (n < 3) continue;
        ImVec2 pts[4];
        for (int m = 0; m < n; ++m) pts[m] = toScreen(c[m]);
        uint32_t rgb = t.color & 0x00FFFFFFu;
        // Farther boundaries fade, so what's near stands out and the rest reads as distant.
        float fade = 1.0f - 0.75f * std::min(t.dist / v->fadeRange, 1.0f);
        draw->AddConvexPolyFilled(pts, n, rgb | ((uint32_t)(v->opacity * fade * 255.0f) << 24));
        if (t.outline) draw->AddPolyline(pts, n, rgb | ((uint32_t)(150.0f * fade) << 24), ImDrawFlags_Closed, 1.5f);
    }

    // The nearest boundary point: a ring there with its distance (a number beats guessing depth from the tint).
    if (v->nearest.valid) {
        float c[3];
        toCam(v->nearest.p, c);
        if (c[0] >= kNear) {
            ImVec2 at = toScreen(c);
            float px = size.y / 1080.0f;
            draw->AddCircle(at, 7.0f * px, IM_COL32(255, 255, 255, 230), 16, 2.0f);
            char text[32];
            snprintf(text, sizeof(text), "%.1f", v->nearest.dist);
            ImVec2 ext = ImGui::CalcTextSize(text);
            draw->AddRectFilled(ImVec2(at.x + 10 * px, at.y - ext.y * 0.5f - 2), ImVec2(at.x + 10 * px + ext.x + 8, at.y + ext.y * 0.5f + 2),
                                IM_COL32(0, 0, 0, 160), 3.0f);
            draw->AddText(ImVec2(at.x + 10 * px + 4, at.y - ext.y * 0.5f), IM_COL32(255, 255, 255, 240), text);
        }
    }
}

static void DrawHud(const Games::Status& s) {
    auto* draw = ImGui::GetForegroundDrawList();
    DrawDot(s, draw);
    DrawBoundaries(s, ImGui::GetBackgroundDrawList());  // behind the menu window
    if (g_menu.showBanner && GetTickCount64() - g_startTime < kBannerMs) {
        std::string banner = "Halo Mod Menu loaded - " + Menu::Describe(g_menu.openPad) + " or " +
                             Menu::Describe(g_menu.openKey) + " to open";
        draw->AddText(ImVec2(12, 12), IM_COL32(120, 220, 255, 255), banner.c_str());
    }

    // Spawning results come back from the game as a counter and a message.
    static uint32_t s_spawnDone = 0;
    if (s.spawnDone != s_spawnDone) {
        s_spawnDone = s.spawnDone;
        if (s.spawnMessage[0]) Toast(s.spawnMessage);
    }
    ULONGLONG toastAge = GetTickCount64() - g_toastTime;
    if (!g_toast.empty() && toastAge < kToastMs) {
        ImFont* font = ImGui::GetFont();
        float size = ImGui::GetFontSize() * 1.6f;
        ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, g_toast.c_str());
        ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 pos((display.x - extent.x) * 0.5f, display.y * 0.82f - extent.y);  // bottom center, clear of Reach's HUD
        int alpha = toastAge > kToastMs - 500 ? (int)(255 * (kToastMs - toastAge) / 500) : 255;
        draw->AddRectFilled(ImVec2(pos.x - 12, pos.y - 6), ImVec2(pos.x + extent.x + 12, pos.y + extent.y + 6),
                            IM_COL32(0, 0, 0, alpha * 160 / 255), 6.0f);
        draw->AddText(font, size, pos, IM_COL32(255, 210, 80, alpha), g_toast.c_str());
    }
    if (g_armed) {
        // Bottom left: the selected action with the one before and after it.
        std::vector<const Streak*> list = ActiveStreaks(s.title);
        int n = (int)list.size();
        ImFont* font = ImGui::GetFont();
        ImVec2 display = ImGui::GetIO().DisplaySize;
        float scale = display.y / 1080.0f;
        float textSize = ImGui::GetFontSize() * 1.4f;
        const ImU32 gold = IM_COL32(255, 210, 80, 255), dim = IM_COL32(210, 210, 210, 120);
        const float pad = 10.0f * scale, smallPx = 30.0f * scale, bigPx = 52.0f * scale, gap = 5.0f * scale;
        float x = display.x * 0.02f, bottom = display.y * 0.78f;

        if (!n) {
            std::string text = "KILLSTREAKS: none pinned (pin them in the menu)";
            ImVec2 extent = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, text.c_str());
            draw->AddRectFilled(ImVec2(x - pad, bottom - extent.y - pad), ImVec2(x + extent.x + pad, bottom + pad),
                                IM_COL32(0, 0, 0, 150), 6.0f);
            draw->AddText(font, textSize, ImVec2(x, bottom - extent.y), IM_COL32(120, 220, 255, 255), text.c_str());
        } else {
            // The selected entry in the middle, with entries before and after it (wrapping).
            int sel = g_streakSel >= 0 && g_streakSel < n ? g_streakSel : 0;
            int shown = std::min(n, g_menu.streakRows);
            int above = (shown - 1) / 2, below = shown - 1 - above;
            float header = textSize * 1.3f;
            float height = header + bigPx + (shown - 1) * smallPx + shown * gap;
            float top = bottom - height;

            const Streak* cur = list[sel];
            std::string name = cur->label;
            float width = bigPx + pad + font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, name.c_str()).x;
            width = std::max(width, font->CalcTextSizeA(textSize * 0.8f, FLT_MAX, 0.0f, "KILLSTREAKS").x);
            draw->AddRectFilled(ImVec2(x - pad, top - pad), ImVec2(x + width + pad, bottom + pad), IM_COL32(0, 0, 0, 150), 6.0f);
            draw->AddText(font, textSize * 0.8f, ImVec2(x, top), IM_COL32(120, 220, 255, 255), "KILLSTREAKS");

            // An icon, or the name when the action has none (or the icon font didn't load).
            auto entry = [&](const Streak* st, bool selected, float y) {
                float size = selected ? bigPx : smallPx;
                ImFont* iconFont = selected ? g_iconLarge : g_iconSmall;
                const char* icon = iconFont ? IconUtf8(st->id) : nullptr;
                float left = x + (bigPx - size) * 0.5f;  // smallPx icons sit centred over the selected one
                if (icon) draw->AddText(iconFont, size, ImVec2(left, y), selected ? gold : dim, icon);
                else if (!selected) draw->AddText(font, textSize * 0.8f, ImVec2(left, y), dim, st->label);
                if (selected) draw->AddText(font, textSize, ImVec2(x + bigPx + pad, y + (bigPx - textSize) * 0.5f), gold, st->label);
            };
            float y = top + header;
            for (int k = -above; k <= below; ++k) {
                bool selected = k == 0;
                entry(list[((sel + k) % n + n) % n], selected, y);
                y += (selected ? bigPx : smallPx) + gap;
            }
        }
    }
    if (!s.supported || !g_menu.showHud) return;

    const Games::Settings& c = g_settings;
    char text[512] = "";
    auto add = [&](bool on, const char* name) {
        if (!on) return;
        if (text[0]) strcat_s(text, " | ");
        strcat_s(text, name);
    };
    add(c.godMode, "GOD");
    add(c.infiniteAmmo, "AMMO");
    add(c.bottomlessClip, "CLIP");
    add(c.infiniteGrenades, "NADES");
    add(c.oneHitKill, "1HK");
    add(c.speed != 1.0f, "SPEED");
    add(c.jump != 1.0f, "JUMP");
    add(c.infiniteSprint, "INFINITE SPRINT");
    add(c.doubleJump, c.infiniteJump ? "INFINITE JUMP" : "DOUBLE JUMP");
    add(c.fly, "FLY");
    add(c.noclip, "NOCLIP");
    add(c.gravity != 1.0f, "GRAVITY");
    add(c.gameSpeed != 1.0f, "TIME");
    add(c.noBoundaries, "NO BOUNDS");
    add(c.damage != 1.0f, "DAMAGE");
    add(c.aimbot, "AIMBOT");
    add(c.swordFly, "SWORD FLY");
    add(c.ammoType != 0xFFFFFFFF, "AMMO TYPE");
    add(c.aiBlind, "UNSEEN");
    add(c.aiCantShoot, "AI NO FIRE");
    add(c.fullAuto, "FULL AUTO");
    add(c.rapidFire, "RAPID");
    add(c.vehicleSpeed != 1.0f, "VEH SPEED");
    add(c.vehicleGrip != 1.0f, "VEH GRIP");
    add(c.vehicleRapidFire, "VEH RAPID");
    add(c.vehicleNoRecoil, "VEH NO RECOIL");
    add(c.vehicleGodMode, "VEH GOD");
    add(c.vehicleNoHijack, "NO HIJACK");
    add(c.vehicleNoEject, "NO EJECT");
    add(c.pilotAiVehicles, "PILOT AI VEH");
    add(c.vehicleNoEmp, "NO EMP");
    add(c.vehicleInfiniteBoost, "INF BOOST");
    add(c.noSpread, "NO SPREAD");
    add(c.noRecoil, "NO RECOIL");
    add(c.noOverheat, "NO HEAT");
    add(c.infiniteEquipment, "EQUIP");
    add(c.noEquipmentDespawn, "EQUIP STAYS");
    add(c.perfectCamo, "PERFECT CAMO");
    add(c.aiDeaf, "AI DEAF");
    add(c.aiStayDead, "AI STAY DEAD");
    add(c.noFriendlyFire, "NO FRIENDLY FIRE");
    add(c.noBetrayal, "NO BETRAYAL");
    add(c.poisonTouch, "POISON TOUCH");
    add(c.timeStop, "TIME STOP");
    add(c.launchOnHit, "LAUNCH ON HIT");
    add(c.aiScaleEnemy != 1.0f || c.aiScaleFriendly != 1.0f, "AI SIZE");
    add(c.lockCheckpoints, "CHECKPOINTS LOCKED");
    add(c.upgradeAiAuto, "AI UPGRADE");
    add(c.randomSwapAuto, "AI SWAP");
    add(c.randomTeamsAuto, "AI TEAMS");
    add(c.playerTeam == 1, "COVENANT");
    add(c.playerTeam == 2, "ROGUE");
    add(c.playerTeam == 3, "FLOOD");
    add(c.thirdPerson, "3RD PERSON");
    add(c.spawnEnabled, c.spawnRespawn ? "SPAWN (RESPAWN)" : "SPAWN");
    add(c.recoilPush, "RECOIL PUSH");
    add(c.teleportEnabled, "TELEPORT");
    add(c.chaosGun, "CHAOS GUN");
    add(c.rain, "RAIN");
    add(c.skulls != 0, "SKULLS");
    const float* h = g_menu.hudText;
    if (text[0]) draw->AddText(ImVec2(12, 30), ImGui::ColorConvertFloat4ToU32(ImVec4(h[0], h[1], h[2], h[3])), text);
}

// Noclip controls from keyboard and controller. Cleared while the menu is open.
static Games::FrameInput ReadFrameInput() {
    Games::FrameInput in;
    // The jump press counter never goes backwards, and presses made while the
    // menu is open (A selects there) aren't counted: a counter that dropped to 0
    // while the menu was open read as a new press when it closed, and the air
    // jump fired again on opening and closing the menu.
    static bool s_jumpWas = false;
    static uint32_t s_jumpPresses = 0;
    auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (g_showMenu || GetForegroundWindow() != g_hwnd) {
        s_jumpWas = Gamepad::Held(XINPUT_GAMEPAD_A) || key(VK_SPACE);
        in.jumpPresses = s_jumpPresses;
        return in;
    }

    in.firing = Gamepad::RightTriggerHeld() || key(VK_LBUTTON);
    in.exitHeld = Gamepad::Held(XINPUT_GAMEPAD_X) || key('E');
    in.jumpHeld = Gamepad::Held(XINPUT_GAMEPAD_A) || key(VK_SPACE);
    if (in.jumpHeld && !s_jumpWas) ++s_jumpPresses;
    s_jumpWas = in.jumpHeld;
    in.jumpPresses = s_jumpPresses;
    if (!g_settings.noclip) return in;
    in.noclipForward = Gamepad::Held(XINPUT_GAMEPAD_RIGHT_SHOULDER) || key('G');
    in.fast = key(VK_SHIFT);
    return in;
}

// --- D3D11 hooks ----------------------------------------------------------

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

static bool InitOverlay(IDXGISwapChain* swapChain) {
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    g_swapChain = swapChain;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't write imgui.ini into the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;  // fed by Gamepad::FeedImGui
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProcDetour);
    g_startTime = GetTickCount64();

    LOG("Overlay initialized (window %p, %ux%u)", (void*)g_hwnd, desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

static HRESULT __stdcall PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!g_swapChain && !InitOverlay(swapChain))
        return g_origPresent(swapChain, syncInterval, flags);

    // Only draw on the game's main swap chain.
    if (swapChain != g_swapChain)
        return g_origPresent(swapChain, syncInterval, flags);

    // Each game keeps its own settings: swap them in before this frame's
    // Tick when the running game changes.
    SettingsFile::SwitchGame(Games::RunningTitle(), g_settings);
    Games::Status status = Games::Tick(g_settings, ReadFrameInput());
    g_features[0] = status.features;
    g_features[1] = status.features2;
    UpdateMenuInput(status.supported && status.inGame, !MCC::GetRunning().inGame, status.title);
    Coop::Sync();

    if (!g_rtv) CreateRenderTarget();

    if (g_styleDirty) {
        ApplyStyle();
        g_styleDirty = false;
    }
    if (g_fontDirty) {
        RebuildFont();
        g_fontDirty = false;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    // While a binding is being captured, and until its buttons are let go, the
    // menu doesn't navigate: otherwise finishing a capture with the select
    // button (X) would press "Reset binding" again and start a new capture.
    bool capturing = g_capture.target != nullptr || g_waitForRelease;
    Gamepad::FeedImGui(io, g_showMenu && !capturing, PadMask(g_menu.selectPad));
    if (capturing) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = g_showMenu;
    ImGui::NewFrame();

    if (g_showMenu) {
        if (g_menuLite) DrawLiteMenu();
        else DrawMenu(status);
    }
    DrawHud(status);
    // Saved once a slider is let go, not on every step of a drag.
    if (!ImGui::IsAnyItemActive()) SettingsFile::SaveIfChanged(g_settings, g_menu);

    ImGui::Render();
    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_origPresent(swapChain, syncInterval, flags);
}

static HRESULT __stdcall ResizeBuffersDetour(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height,
                                             DXGI_FORMAT format, UINT flags) {
    if (swapChain == g_swapChain && g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;  // recreated on the next Present
    }
    return g_origResizeBuffers(swapChain, count, width, height, format, flags);
}

// Creates a throwaway device + swap chain to read the IDXGISwapChain vtable,
// which is shared by the game's swap chain.
static bool HookSwapChain() {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr),
                   nullptr, nullptr, nullptr, nullptr, L"HaloModMenuDummy", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                              nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);

    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vtable = *(void***)swapChain;
        void* present = vtable[8];
        void* resize = vtable[13];

        ok = MH_CreateHook(present, (void*)&PresentDetour, (void**)&g_origPresent) == MH_OK &&
             MH_CreateHook(resize, (void*)&ResizeBuffersDetour, (void**)&g_origResizeBuffers) == MH_OK &&
             MH_EnableHook(present) == MH_OK && MH_EnableHook(resize) == MH_OK;

        swapChain->Release();
        device->Release();
        context->Release();
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    LOG(ok ? "Hooked D3D11 Present/ResizeBuffers" : "Failed to hook D3D11 (hr=%08X)", (unsigned)hr);
    return ok;
}

static bool HookCursor() {
    void* setPos = nullptr;
    void* clip = nullptr;
    bool ok = MH_CreateHookApiEx(L"user32", "SetCursorPos", (void*)&SetCursorPosDetour, (void**)&g_origSetCursorPos, &setPos) == MH_OK &&
              MH_CreateHookApiEx(L"user32", "ClipCursor", (void*)&ClipCursorDetour, (void**)&g_origClipCursor, &clip) == MH_OK &&
              MH_EnableHook(setPos) == MH_OK && MH_EnableHook(clip) == MH_OK;
    if (!ok) g_origClipCursor = nullptr;
    LOG(ok ? "Hooked cursor functions" : "Failed to hook cursor functions");
    return ok;
}

bool Overlay::InstallHooks() {
    SettingsFile::Load(g_menu);
    Gamepad::Init();
    return HookCursor() && HookSwapChain();
}
