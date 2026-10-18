#pragma once

#include <windows.h>

#include "games/fun.h"
#include "games/game.h"

// Halo 3 cheat logic.
namespace Halo3Cheats {
    // halo3.dll loaded: find the code patches. Returns nothing; missing
    // patches just leave their cheats inactive (logged).
    void Load(HMODULE halo3);
    // halo3.dll unloading: undo code patches, forget everything else.
    void Unload();

    // Render thread, every frame: memory and tag edits. Returns false if game
    // state couldn't be read (e.g. mid level load).
    bool Apply(void** gameState, const Games::Settings& settings, Games::Status& status);

    // Game thread, inside the frame (after Halo 3's input update): cheats that
    // call engine functions (damage multiplier, give weapon).
    void MainThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input);

    // Game thread, from MCC's input polling (before the title processes
    // input): noclip. It worked best from here; its teleport faults partway
    // through (after moving the player) and the fault is ignored.
    void InputThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input);

    // Barrel-fire hook, around each shot: gives the chosen ammo type's zero-speed
    // projectile (a grenade) a muzzle speed for the shot only.
    void ShotSpeed(bool on);

    // Barrel-fire hook, around each shot: the chaos gun and recoil push
    // (games/fun.h), for the players' weapons and their vehicles' guns.
    void BeforeShot(void** gameState, uint32_t weapon, int16_t barrel, Fun::Shot& shot);

    // Teleport moved player 1: noclip holds them at the new spot.
    void MoveNoclipAnchor(const float* position);
}
