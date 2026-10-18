#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "games/game.h"

// Halo: Reach cheat logic.
namespace ReachCheats {
    // haloreach.dll loaded: find the code patches. Missing patches just leave
    // their cheats inactive (logged).
    void Load(HMODULE reach);
    // haloreach.dll unloading: undo code patches, forget everything else.
    void Unload();

    // Render thread, every frame: memory and tag edits. Returns false if game
    // state couldn't be read (e.g. mid level load).
    bool Apply(void** gameState, const Games::Settings& settings, Games::Status& status);

    // Game thread, once per game tick (before the tick runs): cheats that
    // call engine functions (noclip, damage multiplier, give weapon, aimbot pull).
    void MainThread(void** gameState, const Games::Settings& settings, const Games::FrameInput& input);

    // Game thread, once per game tick (after the tick ran): edits the tick
    // would otherwise undo before the frame is drawn (perfect camo).
    void AfterTick(void** gameState, const Games::Settings& settings);

    // Object-update hook: true while time stop holds this object (an AI's
    // unit, or the vehicle it rides) still.
    bool ObjectFrozen(uint32_t datum);

    // Game-save hook (the request function every script checkpoint goes
    // through): true while "Lock checkpoints" is on and the mod's own Save
    // button isn't the caller.
    bool CheckpointsLocked();

    // The checkpoints kept for this mission, copied out for the menu.
    void CheckpointHistory(std::vector<Games::CheckpointEntry>& out);

    // Barrel-fire hook: false if this weapon may not fire (AI can't shoot).
    bool WeaponMayFire(void** gameState, uint32_t weapon);

    // Cause-damage hook: false to drop a hit (no friendly fire).
    bool DamageAllowed(void** gameState, const void* damageData, uint32_t victim);

    // Diagnostics for friendly fire: hits on allied AI seen by the hook since the last call.
    struct FriendlyFireStats {
        uint32_t onAllies = 0, blocked = 0, throughOwned = 0, noOwner = 0;
        uint32_t lastEffect = 0, lastFlags = 0, lastOwner = 0xFFFFFFFF;
    };
    FriendlyFireStats TakeFriendlyFireStats();

    // Flip-eject hook: false to skip the handler ("Cannot be ejected" keeps
    // players in a flipped vehicle).
    bool FlipEjectAllowed(void** gameState, uint32_t unit, void* event);

    // Barrel-fire hook, around each shot: the chaos gun swaps the barrel's
    // projectile for a random one (put back after the shot), recoil push
    // shoves the shooter. Only for the players' weapons and their vehicles'.
    struct Shot {
        uint32_t* projectile = nullptr;  // tag field to put back
        uint32_t saved = 0;
    };
    void BeforeShot(void** gameState, uint32_t weapon, int16_t barrel, Shot& shot);
    void AfterShot(const Shot& shot);
    // Boundary view (boundary_view.cpp). The observer's drawn camera; false if unreadable.
    bool ReadObserver(void** state, float* pos, float* fwd, float* up, float* tanHalfV);
    // Render thread: the camera, the kill volumes (killCount = the scenario's count before the mod zeroed
    // it) and the soft ceilings.
    // Game thread, every tick: ray-tests the pieces BoundaryFill asked about (is the world between the camera and
    // the piece?) so it can hide the ones behind walls and objects.
    void BoundaryOcclusion(void** state, const Games::Settings& settings, uint32_t unitDatum, uint32_t vehicle);
    void BoundaryFill(void** state, char* scnr, int killCount, const Games::Settings& settings,
                      Games::BoundaryView& view);

    // Explosion acceleration: the factor the damage-acceleration hook applies (1 = off). Any thread.
    float ExplosionScale();
    // Recoil pushes queued by shots since the last call (applied by the
    // game tick through the engine's set-velocity). Returns the count.
    struct PendingPush {
        uint32_t object;
        float velocity[3];  // to add
    };
    int TakePushes(PendingPush* out, int max);

    // Rain: a random pick of a kind (Games::Settings::rainKind) from this
    // map's spawn lists; -1 if there's nothing. Any thread.
    uint32_t RainPick(int kind, uint32_t spawnSelection);

    // This map's spawnable characters ('char' tags with a unit), for
    // upgrade AI. Empty until the lists are built. Any thread.
    std::vector<uint32_t> SpawnableCharacters();

    // This map's player-usable weapon tags. Empty until the lists are built. Any thread.
    std::vector<uint32_t> WeaponTags();
}
