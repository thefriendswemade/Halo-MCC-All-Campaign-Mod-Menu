#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MinHook.h"

#include "core/log.h"
#include "core/proxy.h"
#include "mcc/mcc.h"
#include "mcc/offsets.h"
#include "ui/overlay.h"
#include "ui/settings_file.h"

// Reads a module's file version from its version resource. (We can't call
// GetFileVersionInfo here: we *are* version.dll.)
static bool GetModuleVersion(HMODULE module, wchar_t* out, size_t outLen) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
    if (!res) return false;
    HGLOBAL data = LoadResource(module, res);
    auto bytes = (const BYTE*)LockResource(data);
    DWORD size = SizeofResource(module, res);
    if (!bytes) return false;

    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        auto info = (const VS_FIXEDFILEINFO*)(bytes + i);
        if (info->dwSignature != 0xFEEF04BD) continue;
        swprintf_s(out, outLen, L"%u.%u.%u.%u",
                   HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        return true;
    }
    return false;
}

// First bytes of the exe functions we hook or call, as of 1.3528.0.0 (bytes
// that hold relative addresses are left out). If an update moved any of them,
// the offsets are stale and hooking would crash the game.
struct ExeSignature {
    const char* name;
    DWORD rva;
    BYTE bytes[16];
    size_t length;
};

static const ExeSignature kExeSignatures[] = {
    {"module load",   OFFSET_MCC_MODULE_LOAD,    {0x4D, 0x85, 0xC0, 0x0F, 0x84}, 5},
    {"module unload", OFFSET_MCC_MODULE_UNLOAD,  {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x48, 0x8B, 0x05}, 14},
    {"delta time",    OFFSET_MCC_DELTA_TIME,     {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8}, 10},
    {"xuid lookup",   OFFSET_MCC_GET_INDEX_BY_XUID, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83}, 16},
};

static bool ExeMatchesOffsets() {
    auto exe = (const BYTE*)GetModuleHandleW(nullptr);
    for (const ExeSignature& sig : kExeSignatures) {
        if (memcmp(exe + sig.rva, sig.bytes, sig.length) != 0) {
            LOG("Exe code at %s (RVA 0x%X) doesn't match what the offsets expect", sig.name, sig.rva);
            return false;
        }
    }
    return true;
}

static DWORD WINAPI InitThread(LPVOID) {
    wchar_t version[32] = L"unknown";
    GetModuleVersion(GetModuleHandleW(nullptr), version, _countof(version));
    LOG("HaloModMenu starting. MCC version %ls (built for: %ls)", version, SUPPORTED_MCC_VERSION);

    if (wcscmp(version, SUPPORTED_MCC_VERSION) != 0)
        LOG("MCC version differs from the one this mod was built for; checking whether the offsets still line up");

    if (!ExeMatchesOffsets()) {
        LOG("MCC's code has moved since this mod was built; mod disabled. Offsets need updating.");
        MessageBoxW(nullptr,
                    L"HaloModMenu was built for MCC " SUPPORTED_MCC_VERSION L", and this MCC update changed the "
                    L"game code the mod depends on.\n\nThe mod is disabled for this session so it can't crash "
                    L"the game. It needs to be updated; check the Releases page.",
                    L"HaloModMenu", MB_OK | MB_ICONWARNING);
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        LOG("MinHook failed to initialize");
        return 0;
    }

    MCC::InstallHooks();
    Overlay::InstallHooks();
    return 0;
}

static bool IsMccProcess() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return name && _wcsicmp(name + 1, L"MCC-Win64-Shipping.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        if (!Proxy::Load()) return FALSE;

        // Anything else in this folder that loads version.dll just gets the proxy.
        if (IsMccProcess()) {
            Log::Init(module);
            SettingsFile::Init(module);
            CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        }
    }
    return TRUE;
}
