#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The mod menu's own settings (look, size, position, button bindings).
// Shared by every game; saved in the [Mod Menu] section of HaloModMenu.ini.
namespace Menu {
    // One input in a binding: a controller button (bits 0-15 as XINPUT_GAMEPAD_*,
    // 16 = left trigger, 17 = right trigger) or a keyboard key (virtual-key code).
    constexpr uint32_t kPad = 0x10000000u;
    constexpr uint32_t kKey = 0x20000000u;
    constexpr uint32_t kPadLeftTrigger = 16;
    constexpr uint32_t kPadRightTrigger = 17;
    constexpr int kMaxBindingInputs = 4;

    // Inputs pressed together (a chord). It fires when the last one goes down.
    struct Binding {
        int count = 0;
        uint32_t inputs[kMaxBindingInputs] = {};
        bool operator==(const Binding& o) const;
    };

    enum Theme { kThemeDark, kThemeLight, kThemeClassic, kThemeHalo, kThemeCovenant, kThemeCount };
    extern const char* const kThemeNames[kThemeCount];

    // Fonts the menu can use: "Default" is ImGui's built-in one; the rest are
    // loaded from C:\Windows\Fonts if present.
    constexpr int kFontCount = 8;
    extern const char* const kFontNames[kFontCount];
    extern const wchar_t* const kFontFiles[kFontCount];  // nullptr for the built-in font

    struct Config {
        int theme = kThemeDark;
        float accent[4] = {};    // set from the theme by Defaults()
        float windowBg[4] = {};
        float text[4] = {};
        float hudText[4] = {};   // the active-cheat list in the corner

        Binding openPad;         // open (and close) from the controller
        Binding openKey;         // open (and close) from the keyboard
        Binding selectPad;       // controller "activate" in the menu
        Binding backPad;         // up to the main page
        Binding closePad;        // close the menu
        Binding spawnPad;        // spawn the selection where you aim (Spawning page); titles without their own
        std::map<std::string, Binding> spawnPadByGame;  // per title ("Halo: CE"), see SpawnPad
        std::map<std::string, Binding> teleportPadByGame;  // per title; see TeleportPad (ODST: D-pad right alone)
        Binding spawnKey;
        Binding takeSeatPad;     // take the pilot seat of the aircraft you aim at (Vehicles page)
        Binding takeSeatKey;
        Binding teleportPad;     // teleport where you aim (Fun page)
        Binding teleportKey;

        float width = 440.0f;
        float maxHeight = 0.0f;  // 0 = fit the page
        float posX = 40.0f;      // where the menu opens
        float posY = 40.0f;
        int font = 0;
        float fontSize = 13.0f;
        bool showHud = true;     // active-cheat list
        bool showBanner = true;  // "loaded" banner at startup
        bool killstreakMode = false;  // D-pad down arms the pinned-action list (D-pad down is then hidden from the game)
        int streakRows = 3;      // icons shown in the killstreak list: 3 or 5
        std::map<std::string, std::vector<std::string>> streaks;  // per title: pinned action ids, in pin order
    };

    Config Defaults();

    // The controller spawn key for a title (nullptr = no game: the shared
    // one). A title without its own yet gets its default: D-pad down alone
    // in Halo: CE and Halo 2 (Back changes their graphics there, and D-pad
    // down does nothing), the shared binding elsewhere.
    Binding& SpawnPad(Config& c, const char* title);
    Binding DefaultSpawnPad(const char* title);
    Binding& TeleportPad(Config& c, const char* title);
    Binding DefaultTeleportPad(const char* title);
    void ApplyThemeColors(Config& c, int theme);  // the theme's four colours

    std::string Serialize(const Config& c);
    void Parse(const std::string& text, Config& c);

    // "LT + RS", "F6", "Ctrl + M".
    std::string Describe(const Binding& b);
    std::string DescribeInput(uint32_t input);
}
