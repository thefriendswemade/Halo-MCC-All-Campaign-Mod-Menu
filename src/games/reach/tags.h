#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "games/game.h"

// Reads Halo: Reach tag data (the loaded map's definitions) from memory.
// Callers must be inside SEH: pointers are only valid while a map is loaded.
namespace ReachTags {
    void SetModule(HMODULE reach);  // nullptr on unload

    // Tag data for a tag datum (e.g. an object's definition), or nullptr.
    char* Get(uint32_t datum);

    // The loaded scenario ('scnr') and globals ('matg') tags, or nullptr.
    char* Scenario();
    char* Globals();

    // Reads tag names from the loaded map's .map file (they aren't kept in
    // memory). Cached per map. Returns false if the names couldn't be read.
    bool LoadNames();

    // Every tag of one group ('weap', 'proj', ...) with its short name,
    // sorted by name. Safe to call any time.
    std::vector<Games::TagChoice> ListGroup(uint32_t groupMagic);

    // A tag's full name from the .map file, or "" if the names aren't loaded.
    std::string FullName(uint32_t datum);

    // A string id's text (loaded with the names), or "" if unknown.
    std::string StringName(uint32_t stringId);

    // A tag's group ('weap', 'char', ...), or 0.
    uint32_t GroupOf(uint32_t datum);

    // Element `index` of a tag block at `tag + blockOffset`, or nullptr.
    char* BlockElement(char* tag, int blockOffset, int elementSize, int index);
    int BlockCount(char* tag, int blockOffset);
    // The first element of a block whatever its count says (a block the mod
    // zeroed to disable it still has its data), or nullptr.
    char* BlockData(char* tag, int blockOffset);
}
