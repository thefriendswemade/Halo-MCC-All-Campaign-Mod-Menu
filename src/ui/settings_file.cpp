#include "settings_file.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"
#include "mcc/coop.h"

namespace {
    wchar_t g_path[MAX_PATH] = {};
    std::map<std::string, std::string> g_sections;  // game title -> its "name=value" lines
    std::string g_current;                           // the game whose settings the menu holds
    std::string g_written;                           // file contents as last loaded or saved
    const char* const kMenuSection = "Mod Menu";      // the menu's own settings, not a game
    const char* const kCoopSection = "Co-op";         // local co-op settings, shared by every game

    // Before settings were per game (2026-10-02) the file had no sections;
    // those values are copied to each of these titles once.
    const char* const kMigrateTo[] = {"Halo: CE", "Halo 2", "Halo 3", "Halo 3: ODST", "Halo: Reach"};

    enum class Type { Bool, Float, Int, U64 };
    struct Entry {
        const char* name;
        Type type;
        size_t offset;
    };

#define BOOL_ENTRY(f) {#f, Type::Bool, offsetof(Games::Settings, f)}
#define FLOAT_ENTRY(f) {#f, Type::Float, offsetof(Games::Settings, f)}
#define INT_ENTRY(f) {#f, Type::Int, offsetof(Games::Settings, f)}
#define U64_ENTRY(f) {#f, Type::U64, offsetof(Games::Settings, f)}
    const Entry kEntries[] = {
        BOOL_ENTRY(godMode), BOOL_ENTRY(infiniteAmmo), BOOL_ENTRY(bottomlessClip), BOOL_ENTRY(infiniteGrenades),
        BOOL_ENTRY(oneHitKill), BOOL_ENTRY(fly), BOOL_ENTRY(noclip), BOOL_ENTRY(noBoundaries), BOOL_ENTRY(aimbot),
        BOOL_ENTRY(aiBlind), BOOL_ENTRY(aiCantShoot), BOOL_ENTRY(fullAuto), BOOL_ENTRY(rapidFire),
        BOOL_ENTRY(noSpread), BOOL_ENTRY(noRecoil), BOOL_ENTRY(noOverheat), BOOL_ENTRY(infiniteEquipment),
        BOOL_ENTRY(noEquipmentDespawn), FLOAT_ENTRY(fov), BOOL_ENTRY(grenadeEnemiesOnly), BOOL_ENTRY(grenadeAuto), FLOAT_ENTRY(explosionScale), BOOL_ENTRY(infiniteSprint), BOOL_ENTRY(doubleJump), BOOL_ENTRY(infiniteJump), FLOAT_ENTRY(airJumps), BOOL_ENTRY(perfectCamo), BOOL_ENTRY(aiDeaf), BOOL_ENTRY(aiStayDead), BOOL_ENTRY(thirdPerson), BOOL_ENTRY(noHud),
        BOOL_ENTRY(envySkull), BOOL_ENTRY(vehicleRapidFire), BOOL_ENTRY(vehicleGodMode), BOOL_ENTRY(vehicleNoHijack),
        BOOL_ENTRY(vehicleNoEmp), BOOL_ENTRY(vehicleInfiniteBoost), BOOL_ENTRY(vehicleNoRecoil), BOOL_ENTRY(vehicleNoEject), BOOL_ENTRY(pilotAiVehicles),
        BOOL_ENTRY(spawnEnabled), BOOL_ENTRY(spawnRespawn), BOOL_ENTRY(spawnRandomGuns), INT_ENTRY(spawnTeam),
        FLOAT_ENTRY(cameraDistance), FLOAT_ENTRY(aimbotStrength), FLOAT_ENTRY(damage), FLOAT_ENTRY(speed),
        FLOAT_ENTRY(jump), FLOAT_ENTRY(gravity), FLOAT_ENTRY(gameSpeed), FLOAT_ENTRY(vehicleSpeed),
        FLOAT_ENTRY(vehicleGrip),
        U64_ENTRY(skulls), BOOL_ENTRY(chaosGun), BOOL_ENTRY(rain), INT_ENTRY(rainKind), INT_ENTRY(bombingRunCount), INT_ENTRY(bombingRunType), FLOAT_ENTRY(rainInterval),
        FLOAT_ENTRY(rainMax), FLOAT_ENTRY(rainRadius), BOOL_ENTRY(recoilPush), FLOAT_ENTRY(recoilStrength), BOOL_ENTRY(teleportEnabled),
        BOOL_ENTRY(randomSwapAuto), BOOL_ENTRY(randomSwapAllies), BOOL_ENTRY(upgradeAiAuto), BOOL_ENTRY(upgradeAiAllies), FLOAT_ENTRY(upgradeAiRange),
        BOOL_ENTRY(randomTeamsAuto), BOOL_ENTRY(randomTeamsSpartans), FLOAT_ENTRY(randomTeamsFactions),
        FLOAT_ENTRY(randomTeamsRange), INT_ENTRY(playerTeam), BOOL_ENTRY(nukeSpareAllies), BOOL_ENTRY(noFriendlyFire), BOOL_ENTRY(noBetrayal), BOOL_ENTRY(poisonTouch), BOOL_ENTRY(timeStop), BOOL_ENTRY(swordFly), BOOL_ENTRY(dot), FLOAT_ENTRY(dotSize), // Renamed from dotOffsetX/Y so the -88/-6 saved by the first build (before the
        // per-title defaults) isn't loaded into titles whose default is 0.
        {"dotShiftX", Type::Float, offsetof(Games::Settings, dotOffsetX)},
        {"dotShiftY", Type::Float, offsetof(Games::Settings, dotOffsetY)}, {"dotRed", Type::Float, offsetof(Games::Settings, dotColor)},
        {"dotGreen", Type::Float, offsetof(Games::Settings, dotColor) + sizeof(float)},
        {"dotBlue", Type::Float, offsetof(Games::Settings, dotColor) + 2 * sizeof(float)}, BOOL_ENTRY(launchOnHit), FLOAT_ENTRY(aiScaleEnemy), FLOAT_ENTRY(aiScaleFriendly), FLOAT_ENTRY(launchStrength), BOOL_ENTRY(lockCheckpoints),
    };
#undef BOOL_ENTRY
#undef FLOAT_ENTRY
#undef INT_ENTRY
#undef U64_ENTRY

    std::string Serialize(const Games::Settings& s) {
        std::string out;
        char line[96];
        for (const Entry& e : kEntries) {
            const char* field = (const char*)&s + e.offset;
            if (e.type == Type::Bool) snprintf(line, sizeof(line), "%s=%d\n", e.name, *(const bool*)field ? 1 : 0);
            else if (e.type == Type::Int) snprintf(line, sizeof(line), "%s=%d\n", e.name, *(const int*)field);
            else if (e.type == Type::U64)
                snprintf(line, sizeof(line), "%s=%llx\n", e.name, (unsigned long long)*(const uint64_t*)field);
            else snprintf(line, sizeof(line), "%s=%.3f\n", e.name, *(const float*)field);
            out += line;
        }
        return out;
    }

    // Applies "name=value" lines to `s`; returns how many were recognised.
    int Parse(const std::string& text, Games::Settings& s) {
        int loaded = 0;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string name = line.substr(0, eq);
            const char* value = line.c_str() + eq + 1;
            for (const Entry& e : kEntries) {
                if (name != e.name) continue;
                char* field = (char*)&s + e.offset;
                if (e.type == Type::Bool) *(bool*)field = atoi(value) != 0;
                else if (e.type == Type::Int) *(int*)field = atoi(value);
                else if (e.type == Type::U64) *(uint64_t*)field = strtoull(value, nullptr, 16);
                else *(float*)field = (float)atof(value);
                ++loaded;
            }
        }
        return loaded;
    }

    std::string FileText() {
        std::string out;
        for (const auto& [title, lines] : g_sections) out += "[" + title + "]\n" + lines + "\n";
        return out;
    }
}

void SettingsFile::Init(HMODULE self) {
    DWORD len = GetModuleFileNameW(self, g_path, MAX_PATH);
    wchar_t* slash = len && len < MAX_PATH ? wcsrchr(g_path, L'\\') : nullptr;
    if (!slash) {
        g_path[0] = 0;
        return;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_path), L"HaloModMenu.ini");
}

void SettingsFile::Load(Menu::Config& menu) {
    g_sections.clear();
    menu = Menu::Defaults();
    if (!g_path[0]) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path, L"r") || !f) {
        LOG("Settings: no HaloModMenu.ini yet; every game starts with defaults");
        return;
    }
    std::string legacy, section;
    bool inSection = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            section = s.substr(1, s.size() - 2);
            g_sections[section];  // a section may be empty
            inSection = true;
        } else if (!s.empty()) {
            (inSection ? g_sections[section] : legacy) += s + "\n";
        }
    }
    fclose(f);
    if (!legacy.empty()) {
        for (const char* title : kMigrateTo)
            if (!g_sections.count(title)) g_sections[title] = legacy;
        LOG("Settings: copied the old shared settings to each game");
    }
    auto own = g_sections.find(kMenuSection);
    if (own != g_sections.end()) Menu::Parse(own->second, menu);
    auto coop = g_sections.find(kCoopSection);
    if (coop != g_sections.end()) Coop::Parse(coop->second);
    g_written = FileText();
    LOG("Settings: loaded settings for %d games from HaloModMenu.ini",
        (int)g_sections.size() - (own != g_sections.end() ? 1 : 0) - (coop != g_sections.end() ? 1 : 0));
}

void SettingsFile::SwitchGame(const char* title, Games::Settings& s) {
    if (!title || g_current == title) return;
    if (!g_current.empty()) g_sections[g_current] = Serialize(s);

    Games::Settings next;
    next.giveWeaponSerial = s.giveWeaponSerial;
    next.giveEquipmentSerial = s.giveEquipmentSerial;
    next.saveCheckpointSerial = s.saveCheckpointSerial;
    next.loadCheckpointSerial = s.loadCheckpointSerial;
    next.restoreCheckpointSerial = s.restoreCheckpointSerial;
    next.spawnSerial = s.spawnSerial;
    next.teleportSerial = s.teleportSerial;
    next.upgradeVehicleSerial = s.upgradeVehicleSerial;
    next.takeSeatSerial = s.takeSeatSerial;
    next.upgradeAiSerial = s.upgradeAiSerial;
    next.randomSwapSerial = s.randomSwapSerial;
    next.randomTeamsSerial = s.randomTeamsSerial;
    Games::DefaultDotOffset(title, next.dotOffsetX, next.dotOffsetY);
    auto it = g_sections.find(title);
    int loaded = it != g_sections.end() ? Parse(it->second, next) : 0;
    s = next;
    g_current = title;
    LOG("Settings: %s (%s)", title, loaded ? "its saved settings" : "defaults");
}

void SettingsFile::SaveIfChanged(const Games::Settings& s, const Menu::Config& menu) {
    if (!g_path[0]) return;
    if (!g_current.empty()) g_sections[g_current] = Serialize(s);
    g_sections[kMenuSection] = Menu::Serialize(menu);
    g_sections[kCoopSection] = Coop::Serialize();
    std::string text = FileText();
    if (text == g_written) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path, L"w") || !f) {
        LOG("Settings: couldn't write HaloModMenu.ini");
        g_written = text;  // don't retry every frame
        return;
    }
    fputs(text.c_str(), f);
    fclose(f);
    g_written = text;
}
