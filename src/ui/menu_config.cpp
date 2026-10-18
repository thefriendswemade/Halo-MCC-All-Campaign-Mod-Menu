#include "menu_config.h"

#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Menu {
    const char* const kThemeNames[kThemeCount] = {"Dark", "Light", "Classic", "Halo (blue)", "Covenant (purple)"};

    const char* const kFontNames[kFontCount] = {"Default", "Segoe UI", "Consolas", "Arial",
                                                "Verdana", "Tahoma",   "Calibri",  "Bahnschrift"};
    const wchar_t* const kFontFiles[kFontCount] = {nullptr,          L"segoeui.ttf", L"consola.ttf",
                                                   L"arial.ttf",     L"verdana.ttf", L"tahoma.ttf",
                                                   L"calibri.ttf",   L"bahnschrift.ttf"};

    bool Binding::operator==(const Binding& o) const {
        if (count != o.count) return false;
        for (int i = 0; i < count; ++i)
            if (inputs[i] != o.inputs[i]) return false;
        return true;
    }

    namespace {
        void Set(float* c, float r, float g, float b, float a = 1.0f) {
            c[0] = r; c[1] = g; c[2] = b; c[3] = a;
        }

        uint32_t PadBit(WORD button) {
            for (uint32_t bit = 0; bit < 16; ++bit)
                if (button == (1u << bit)) return kPad | bit;
            return 0;
        }

        Binding Chord(std::initializer_list<uint32_t> inputs) {
            Binding b;
            for (uint32_t i : inputs) b.inputs[b.count++] = i;
            return b;
        }

        std::string BindingText(const Binding& b) {
            std::string s;
            char part[16];
            for (int i = 0; i < b.count; ++i) {
                sprintf_s(part, "%s%X", i ? "," : "", b.inputs[i]);
                s += part;
            }
            return s;
        }

        void ParseBinding(const char* v, Binding& b) {
            Binding out;
            while (*v && out.count < kMaxBindingInputs) {
                char* end = nullptr;
                unsigned long x = strtoul(v, &end, 16);
                if (end == v) break;
                out.inputs[out.count++] = (uint32_t)x;
                v = *end == ',' ? end + 1 : end;
            }
            if (out.count > 0) b = out;
        }

        void ParseColor(const char* v, float* c) {
            float r, g, b, a;
            if (sscanf_s(v, "%f,%f,%f,%f", &r, &g, &b, &a) == 4) Set(c, r, g, b, a);
        }
    }

    void ApplyThemeColors(Config& c, int theme) {
        c.theme = theme;
        switch (theme) {
            case kThemeLight:
                Set(c.accent, 0.26f, 0.59f, 0.98f); Set(c.windowBg, 0.94f, 0.94f, 0.94f, 0.96f);
                Set(c.text, 0.0f, 0.0f, 0.0f); Set(c.hudText, 0.15f, 0.35f, 0.75f);
                break;
            case kThemeClassic:
                Set(c.accent, 0.40f, 0.40f, 0.90f); Set(c.windowBg, 0.0f, 0.0f, 0.0f, 0.85f);
                Set(c.text, 0.90f, 0.90f, 0.90f); Set(c.hudText, 1.0f, 0.82f, 0.31f);
                break;
            case kThemeHalo:
                Set(c.accent, 0.20f, 0.65f, 0.95f); Set(c.windowBg, 0.03f, 0.07f, 0.12f, 0.92f);
                Set(c.text, 0.80f, 0.92f, 1.0f); Set(c.hudText, 0.45f, 0.85f, 1.0f);
                break;
            case kThemeCovenant:
                Set(c.accent, 0.62f, 0.30f, 0.90f); Set(c.windowBg, 0.08f, 0.03f, 0.12f, 0.92f);
                Set(c.text, 0.92f, 0.85f, 1.0f); Set(c.hudText, 0.80f, 0.55f, 1.0f);
                break;
            default:
                c.theme = kThemeDark;
                Set(c.accent, 0.26f, 0.59f, 0.98f); Set(c.windowBg, 0.06f, 0.06f, 0.06f, 0.94f);
                Set(c.text, 1.0f, 1.0f, 1.0f); Set(c.hudText, 1.0f, 0.82f, 0.31f);
                break;
        }
    }

    Config Defaults() {
        Config c;
        ApplyThemeColors(c, kThemeDark);
        c.openPad = Chord({kPad | kPadLeftTrigger, PadBit(XINPUT_GAMEPAD_RIGHT_THUMB)});
        c.openKey = Chord({kKey | VK_F6});
        c.selectPad = Chord({PadBit(XINPUT_GAMEPAD_X)});
        c.backPad = Chord({PadBit(XINPUT_GAMEPAD_Y)});
        c.closePad = Chord({PadBit(XINPUT_GAMEPAD_B)});
        c.spawnPad = Chord({PadBit(XINPUT_GAMEPAD_BACK), PadBit(XINPUT_GAMEPAD_DPAD_DOWN)});  // D-pad down is unused in Reach
        c.spawnKey = Chord({kKey | VK_F7});
        c.takeSeatPad = Chord({PadBit(XINPUT_GAMEPAD_BACK), PadBit(XINPUT_GAMEPAD_DPAD_LEFT)});
        c.takeSeatKey = Chord({kKey | VK_F8});
        c.teleportPad = Chord({PadBit(XINPUT_GAMEPAD_BACK), PadBit(XINPUT_GAMEPAD_DPAD_RIGHT)});
        c.teleportKey = Chord({kKey | VK_F9});
        return c;
    }

    static bool ClassicTitle(const char* title) {
        return title && (strcmp(title, "Halo: CE") == 0 || strcmp(title, "Halo 2") == 0);
    }

    Binding DefaultSpawnPad(const char* title) {
        return ClassicTitle(title) ? Chord({PadBit(XINPUT_GAMEPAD_DPAD_DOWN)}) : Defaults().spawnPad;
    }

    Binding& SpawnPad(Config& c, const char* title) {
        if (!title) return c.spawnPad;
        auto it = c.spawnPadByGame.find(title);
        if (it == c.spawnPadByGame.end())
            it = c.spawnPadByGame.emplace(title, ClassicTitle(title) ? DefaultSpawnPad(title) : c.spawnPad).first;
        return it->second;
    }

    // Teleport's controller binding: ODST defaults to D-pad right alone, other titles to the shared chord.
    Binding DefaultTeleportPad(const char* title) {
        if (title && strcmp(title, "Halo 3: ODST") == 0) return Chord({PadBit(XINPUT_GAMEPAD_DPAD_RIGHT)});
        return Defaults().teleportPad;
    }

    Binding& TeleportPad(Config& c, const char* title) {
        if (!title) return c.teleportPad;
        auto it = c.teleportPadByGame.find(title);
        if (it == c.teleportPadByGame.end())
            it = c.teleportPadByGame.emplace(title, DefaultTeleportPad(title)).first;
        return it->second;
    }

    std::string Serialize(const Config& c) {
        char buf[2048];
        auto color = [](const float* v) {
            static char s[4][64];
            static int i = 0;
            i = (i + 1) % 4;
            sprintf_s(s[i], "%.3f,%.3f,%.3f,%.3f", v[0], v[1], v[2], v[3]);
            return s[i];
        };
        std::string out;
        sprintf_s(buf, "theme=%d\naccent=%s\nwindowBg=%s\ntext=%s\n", c.theme, color(c.accent), color(c.windowBg),
                  color(c.text));
        out += buf;
        sprintf_s(buf, "hudText=%s\n", color(c.hudText));
        out += buf;
        out += "openPad=" + BindingText(c.openPad) + "\n";
        out += "openKey=" + BindingText(c.openKey) + "\n";
        out += "selectPad=" + BindingText(c.selectPad) + "\n";
        out += "backPad=" + BindingText(c.backPad) + "\n";
        out += "closePad=" + BindingText(c.closePad) + "\n";
        out += "spawnPad=" + BindingText(c.spawnPad) + "\n";
        for (auto& game : c.spawnPadByGame) out += "spawnPad." + game.first + "=" + BindingText(game.second) + "\n";
        out += "spawnKey=" + BindingText(c.spawnKey) + "\n";
        out += "takeSeatPad=" + BindingText(c.takeSeatPad) + "\n";
        out += "takeSeatKey=" + BindingText(c.takeSeatKey) + "\n";
        out += "teleportPad=" + BindingText(c.teleportPad) + "\n";
        for (auto& game : c.teleportPadByGame) out += "teleportPad." + game.first + "=" + BindingText(game.second) + "\n";
        out += "teleportKey=" + BindingText(c.teleportKey) + "\n";
        sprintf_s(buf, "width=%.0f\nmaxHeight=%.0f\nposX=%.0f\nposY=%.0f\nfont=%d\nfontSize=%.0f\nshowHud=%d\nshowBanner=%d\nkillstreakMode=%d\nstreakRows=%d\n",
                  c.width, c.maxHeight, c.posX, c.posY, c.font, c.fontSize, c.showHud ? 1 : 0, c.showBanner ? 1 : 0,
                  c.killstreakMode ? 1 : 0, c.streakRows);
        out += buf;
        for (auto& game : c.streaks) {
            if (game.second.empty()) continue;
            out += "streaks." + game.first + "=";
            for (size_t i = 0; i < game.second.size(); ++i) out += (i ? "," : "") + game.second[i];
            out += "\n";
        }
        return out;
    }

    void Parse(const std::string& text, Config& c) {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            const char* v = line.c_str() + eq + 1;
            if (k == "theme") c.theme = atoi(v);
            else if (k == "accent") ParseColor(v, c.accent);
            else if (k == "windowBg") ParseColor(v, c.windowBg);
            else if (k == "text") ParseColor(v, c.text);
            else if (k == "hudText") ParseColor(v, c.hudText);
            else if (k == "openPad") ParseBinding(v, c.openPad);
            else if (k == "openKey") ParseBinding(v, c.openKey);
            else if (k == "selectPad") ParseBinding(v, c.selectPad);
            else if (k == "backPad") ParseBinding(v, c.backPad);
            else if (k == "closePad") ParseBinding(v, c.closePad);
            else if (k == "spawnPad") ParseBinding(v, c.spawnPad);
            else if (k.rfind("spawnPad.", 0) == 0) ParseBinding(v, c.spawnPadByGame[k.substr(9)]);
            else if (k == "spawnKey") ParseBinding(v, c.spawnKey);
            else if (k == "takeSeatPad") ParseBinding(v, c.takeSeatPad);
            else if (k == "takeSeatKey") ParseBinding(v, c.takeSeatKey);
            else if (k == "teleportPad") ParseBinding(v, c.teleportPad);
            else if (k.rfind("teleportPad.", 0) == 0) ParseBinding(v, c.teleportPadByGame[k.substr(12)]);
            else if (k == "teleportKey") ParseBinding(v, c.teleportKey);
            else if (k == "width") c.width = (float)atof(v);
            else if (k == "maxHeight") c.maxHeight = (float)atof(v);
            else if (k == "posX") c.posX = (float)atof(v);
            else if (k == "posY") c.posY = (float)atof(v);
            else if (k == "font") c.font = atoi(v);
            else if (k == "fontSize") c.fontSize = (float)atof(v);
            else if (k == "showHud") c.showHud = atoi(v) != 0;
            else if (k == "showBanner") c.showBanner = atoi(v) != 0;
            else if (k == "killstreakMode") c.killstreakMode = atoi(v) != 0;
            else if (k == "streakRows") c.streakRows = atoi(v) == 5 ? 5 : 3;
            else if (k.rfind("streaks.", 0) == 0) {
                auto& ids = c.streaks[k.substr(8)];
                ids.clear();
                std::string list = v;
                while (!list.empty() && (list.back() == '\r' || list.back() == ' ')) list.pop_back();
                size_t at = 0;
                while (at < list.size()) {
                    size_t comma = list.find(',', at);
                    std::string id = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    if (!id.empty()) ids.push_back(id);
                    at = comma == std::string::npos ? list.size() : comma + 1;
                }
            }
        }
        // The first spawn default (Back + D-pad up, 2026-10-02) became Back + D-pad down.
        if (c.spawnPad == Chord({PadBit(XINPUT_GAMEPAD_BACK), PadBit(XINPUT_GAMEPAD_DPAD_UP)}))
            c.spawnPad = Chord({PadBit(XINPUT_GAMEPAD_BACK), PadBit(XINPUT_GAMEPAD_DPAD_DOWN)});
        if (c.theme < 0 || c.theme >= kThemeCount) c.theme = kThemeDark;
        if (c.font < 0 || c.font >= kFontCount) c.font = 0;
    }

    std::string DescribeInput(uint32_t input) {
        if (input & kPad) {
            static const char* const kNames[18] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start",
                                                   "Back",     "LS",         "RS",         "LB",          "RB",
                                                   "?",        "?",          "A",          "B",           "X",
                                                   "Y",        "LT",         "RT"};
            uint32_t bit = input & 0xFF;
            return bit < 18 ? kNames[bit] : "?";
        }
        if (input & kKey) {
            UINT vk = input & 0xFF;
            UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            switch (vk) {  // keys whose names need the extended-key flag
                case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
                case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                case VK_RCONTROL: case VK_RMENU:
                    scan |= 0x100;
                    break;
            }
            wchar_t name[64] = {};
            if (scan && GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0) {
                char out[64];
                WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out), nullptr, nullptr);
                return out;
            }
            char out[16];
            sprintf_s(out, "Key %02X", vk);
            return out;
        }
        return "?";
    }

    std::string Describe(const Binding& b) {
        if (b.count == 0) return "(none)";
        std::string s;
        for (int i = 0; i < b.count; ++i) {
            if (i) s += " + ";
            s += DescribeInput(b.inputs[i]);
        }
        return s;
    }
}
