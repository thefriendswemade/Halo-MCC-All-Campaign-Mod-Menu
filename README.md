# HaloModMenu

**A cheat and sandbox menu for the campaigns of Halo: The Master Chief Collection (Steam).**

Press a button while you play, and a menu opens over the game. From there you can turn on god mode, infinite ammo, flying, slow motion, third person, spawn enemies, allies, weapons and vehicles, play splitscreen co-op in every campaign, and much more. It works in **Halo: CE, Halo 2, Halo 3, Halo 3: ODST, Halo: Reach and Halo 4**.

It's for **campaign only**. Multiplayer isn't supported, and you can't use this mod with anti-cheat on, so don't take it online.

**[Download the latest release](https://github.com/nrkorte/Halo-MCC-All-Campaign-Mod-Menu/releases/latest)**: open that page, then click `HaloModMenu-v….zip` under **Assets**.

> **Note:** this mod was built with **MCC version 1.3528.0.0** and MAY need to be updated in the future. Most MCC updates (multiplayer, bug fixes) don't touch the campaigns, so it should keep working after them. If an update does change the game code the mod depends on, the mod shows a warning and turns itself off for that session until it's updated. Technical details are further down, below the install guide.

---

# How to install

You need Halo: MCC on **Steam** for PC. It takes about five minutes.

### Step 1: Download

1. Go to the [latest release](https://github.com/nrkorte/Halo-MCC-All-Campaign-Mod-Menu/releases/latest).
2. Under **Assets**, click `HaloModMenu-v….zip` to download it.
3. Right-click the zip and choose **Extract All**. You'll get a folder with `version.dll`, `install.ps1`, `uninstall.ps1`, a README and a checksum file.

### Step 2: Tell Windows Defender it's OK

Windows Defender (the antivirus built into Windows) often **flags this mod as a virus and deletes it**. It isn't one: the source code is all in this repo, and the download is built automatically from that source. Defender flags it because the mod uses the same techniques that cheats and malware use (it loads as a replacement for a system file and edits the game while it runs), and because it's new and unknown.

To stop Defender deleting it, **before you install**, add an exclusion for the MCC folder:

1. Press the Windows key, type **Windows Security** and open it.
2. Click **Virus & threat protection**.
3. Under "Virus & threat protection settings" click **Manage settings**.
4. Scroll down to **Exclusions** and click **Add or remove exclusions**.
5. Click **Add an exclusion**, then **Folder**.
6. Pick your MCC folder (it's in the next step if you don't know where it is). Only exclude that folder, not your whole drive.

If Defender already removed the file, open Windows Security, go to **Protection history**, and restore it.

### Step 3: Install

**Easy way:** open the extracted folder, right-click `install.ps1` and choose **Run with PowerShell**. It finds your MCC install through Steam and copies `version.dll` into it. Close MCC first. If it says it can't find the game, use the manual way below.

**Manual way:**
1. Open Steam and go to your Library.
2. Right-click **Halo: The Master Chief Collection**, then **Manage**, then **Browse local files**.
3. Open the `mcc` folder, then `binaries`, then `win64`. (This is also the folder to exclude in Step 2.)
4. Copy `version.dll` from the extracted folder into this `win64` folder.

### Step 4: Launch with anti-cheat off

Start MCC from Steam. When the launcher asks, choose **Play Halo: MCC (Anti-Cheat Disabled)**. The mod does **not** load with anti-cheat on, and you can't play online matchmaking this way.

### Step 5: Open the menu

Start any campaign mission. A banner appears for about 15 seconds when the mod loads. Then:

| | Controller | Keyboard / mouse |
|---|---|---|
| Open or close the menu | LT + click the right stick | F6 |
| Move | D-pad | Mouse or arrow keys |
| Select / switch on | X | Click, or Space |
| Back to the main page | Y | Backspace |
| Close | B | F6 |
| Scroll the menu | Right stick | Mouse wheel |

While the menu is open, the game ignores your controller and keyboard. You can change any of these buttons in **Menu Settings**, the last category in the menu.

### How to uninstall

Close MCC, then open the extracted folder, right-click `uninstall.ps1` and choose **Run with PowerShell**. It removes the mod and asks whether to delete your saved settings too. It only deletes a `version.dll` that really is HaloModMenu, so other mods are left alone.

Or do it by hand: delete `version.dll` from the `win64` folder, and `HaloModMenu.ini` and `HaloModMenu.log` too if you want to remove your saved settings. Uninstall before you play online with anti-cheat on.

### If something goes wrong

| Problem | Try this |
|---|---|
| The menu doesn't open | Make sure you launched with **Anti-Cheat Disabled**. The menu opens on MCC's own screens too, but it only shows the co-op settings there; start a mission to get the full menu. |
| A warning says the game code changed | An MCC update moved something the mod relies on (it was built with 1.3528.0.0). The mod needs an update; check the Releases page. |
| `version.dll` keeps disappearing | Defender is deleting it. Redo Step 2, then restore it from Protection history or copy it again. |
| It worked, then stopped after a Steam update | Same as the warning above. Some features can also break individually if an update changed one title's code; check `HaloModMenu.log`. |
| Something else | Open an issue on GitHub and attach `HaloModMenu.log` from the `win64` folder. |

---

# What it does

Open the menu in a mission and pick a category. Every option can be switched off again, and your choices are saved separately for each game and come back next time you play. Options a game can't support are greyed out.

| Category | What's in it |
|---|---|
| **Player** | God mode, speed, jump height, flying and noclip (fly through walls), third person, no HUD, field of view, permanent reticle dot, teleport to where you're aiming, infinite sprint (Halo 4), change which team you're on |
| **Weapons** | Infinite ammo, infinite grenades, never reload, rapid fire, full-auto, no recoil, no spread, no overheating, recoil push, aimbot, give yourself any weapon on the map |
| **Combat** | One-hit kills, damage multiplier, friendly fire and betrayal options |
| **Equipment** | Infinite equipment, equipment that doesn't expire, give yourself equipment (in games that have it) |
| **AI** | Enemies that can't see, hear or shoot you, upgrade enemies, shuffle AI teams, nuke or despawn everything you spawned |
| **World** | Gravity, slow motion or fast forward, remove invisible walls and kill zones, show kill volumes and soft ceilings as a wireframe overlay (Reach), explosion size, time stop, save and load checkpoints on demand |
| **Vehicles** | Faster vehicles, indestructible vehicles, rapid-fire vehicle guns, infinite boost, can't be hijacked, drive Pelicans and Phantoms |
| **Spawning** | Spawn enemies, allies, squads, weapons and vehicles from the current map, where you're aiming |
| **Fun / Chaos / Skulls** | Launch enemies when you hit them, giant and tiny enemies, double jump, poison touch, perfect camo, make it rain, a grenade dropped under every AI, a bombing run (Halo 3) and other joke options, plus skull toggles |
| **Co-op** | Splitscreen for 2 to 4 local players in any campaign, a controller each |
| **Killstreaks** | Pin any option to a quick-action list, then use the D-pad in game: Down arms it, Left disarms (see below) |
| **Menu Settings** | Menu colours, size, font and position, and button bindings |

## Killstreaks

Tick **Pin to killstreaks** on an option to add it to your quick-action list. Turn on **Killstreak mode**, then use the D-pad without opening the menu: D-pad Down arms the list and picks actions, D-pad Left disarms. While it's on, D-pad Down no longer reaches the game.

## Local co-op

The **Co-op** page turns on splitscreen for every campaign title (2 to 4 local players, a controller each, optional keyboard and mouse for player 1). Restart the mission or load a checkpoint after changing it. The co-op settings also show on MCC's own menus, so do that before starting a Halo: CE mission.

Co-op is ported from [AlphaRing](https://github.com/WinterSquire/AlphaRing) by WinterSquire; see the **Credits** page in the menu.

## Which games and how well tested?

| Game | State |
|---|---|
| Halo 3, Halo: Reach, Halo: CE, ODST, Halo 4 | Core cheats, vehicles and spawning confirmed working in game |
| Halo 2 | Core cheats, vehicles and spawning confirmed; some newer Fun/Chaos features are still being tested |
| Newer additions (FOV slider, explosion size, grenade drop, reticle dot, boundary view, bombing run) | Rolling out game by game; boundary view is Reach only and the bombing run is Halo 3 only, and both are still being tested |

This is a hobby project that's still being worked on, so expect rough edges. Bug reports are welcome.

---

# Technical details

Everything below is for developers and the curious.

## How it works

The mod loads as a `version.dll` proxy: MCC loads it from its own folder, and it forwards every export to the real one in System32, so there's no injector and nothing running alongside the game. Once loaded it hooks MCC's title DLLs (`halo1.dll`, `halo2.dll`, ...), draws an ImGui menu through a D3D11 `Present` hook, blocks game input while the menu is open, and edits game memory (tags, object fields, code patches) per title. Everything is version specific, which is why it only supports MCC 1.3528.0.0.

The mod only touches the MCC process. It writes `HaloModMenu.log` (the previous session's log is kept as `HaloModMenu.prev.log`) and `HaloModMenu.ini` next to the DLL, and makes no network connections.

## Why antivirus flags it

Defender's detection so far is a machine-learning one (`Program:Win32/Contebrew.A!ml`), a generic guess and not a match on known malware. The causes:

- A proxy `version.dll` is the same pattern as DLL side-loading, which ReShade and ASI loaders use and so does malware.
- It patches game memory and installs hooks (MinHook), as every trainer does.
- It's unsigned and new, so it has no download reputation yet.

If you're a maintainer, a false-positive report can be sent to Microsoft's Security Intelligence submission page.

## Verifying a release

Releases are built by GitHub Actions ([workflow](.github/workflows/build.yml)) from the tagged source, not on a personal machine. Each release lists the SHA-256 of `version.dll` and carries a signed build attestation, so you can confirm the file came from this repo's source:

```
gh attestation verify version.dll --repo nrkorte/Halo-MCC-All-Campaign-Mod-Menu
```

## Build it yourself

Requires Visual Studio 2022 with the "Desktop development with C++" workload and the "C++ CMake tools for Windows" component.

```
build.bat                                  -> build\Release\version.dll
powershell -File install.ps1               (copies it into the game folder)
powershell -File uninstall.ps1             (removes it; -RemoveSettings also deletes the ini and logs)
```

`install.ps1` finds MCC through Steam's library list; if it can't, pass `-GameDir "<MCC>\mcc\binaries\win64"`. It works from a source checkout or from an unzipped release.

**Desktop toggle:** `tools\ToggleHaloModMenu\build.bat` builds `Toggle-HaloModMenu.exe` and copies it to the Desktop. Each run flips the mod:
- **Off:** renames `version.dll` to `version.dll.disabled`.
- **On:** copies the latest `build\Release\version.dll`.

It finds MCC through Steam's library list, and it won't touch a `version.dll` that isn't HaloModMenu.

## Behaviour notes

- After the menu closes, input stays blocked until you let go of every button, so the B press doesn't reach the game.
- The corner lists the cheats that are on (this and the startup banner can be hidden in Menu Settings).
- **Reset:** on the main page, "Reset all" resets everything for the current game; inside a category, "Reset page" resets just that page.
- Cheat settings are saved per game in `HaloModMenu.ini` (a section per game; the menu's own settings are in `[Mod Menu]`). Delete the file to start from defaults.
- The open, select, back and close buttons can be rebound in Menu Settings (the open shortcut can be 1 to 4 buttons or keys pressed together).

`NOTES.md` is the detailed handoff: per-title differences, what was tested, known issues and the reverse-engineering notes.

## Core cheats

The table describes the Halo 3 versions; `NOTES.md` covers how each title differs.

| Cheat | How it works |
|---|---|
| God mode | Sets the player unit's "cannot take damage" flag |
| Infinite ammo | Keeps the player's reserve rounds full, using the weapon tag's magazine sizes |
| Bottomless clip | Keeps the loaded rounds full. Energy weapons: resets battery drain |
| Infinite grenades | Keeps each grenade type at the globals tag's maximum |
| One-hit kill | Enemy AI bipeds get no shield and 1% health. Allies (player and human teams) and scripted-invulnerable characters are skipped |
| Speed | Scales the walk, run and acceleration values in the globals tag's Player Information block |
| Jump height | Scales the player biped tag's jump velocity |
| Fly | Turns on MCC's Acrophobia skull (built-in flight) |
| Noclip | Holds you in place with the engine's position call each tick. Hold RB (or G) to fly where you're looking, through anything. Shift for fast |
| Gravity | Scales the world gravity constant (affects everything) |
| Game speed | Scales game time (slow-mo or fast-forward) |
| Remove boundaries *(experimental)* | Hides the scenario's kill trigger volumes and makes soft ceilings ignore everything |
| Damage to enemies | Scales health and shield lost by enemy AI each tick. Calls the engine's `unit_kill` when a scaled hit is fatal |
| Aimbot | Maxes your weapons' built-in autoaim angle, range and deviation, so shots bend onto targets |
| Aimbot strength | 0% = bending only. Higher pulls your view, while you're firing, onto the enemy nearest your crosshair; 100% snaps to any enemy on screen (walls aren't checked) |
| Ammo type | Your held weapon's barrels fire the chosen projectile (every weapon of that type does, until switched off) |
| Give weapon | Creates the chosen weapon with the engine's own functions and puts it in place of your held one |
| Enemies can't see you | Sets the AI-disregard flag on your unit, and applies the Cheat Engine table's perception patch |
| AI can't shoot | Code patch from the Cheat Engine table that makes AI skip firing. Also turns off AI grenades |
| AI can't hear | Code patch that gives AI a hearing range of zero, so gunfire and footsteps don't alert them (applies to every sound) |
| Full-auto | Semi-auto weapons keep firing while the trigger is held. Your weapons only |
| Rapid fire | Fires every game tick, ignoring the weapon's rate of fire. Your weapons only |
| No spread | Keeps bullet spread at its minimum. Your weapons only |
| No recoil | Code patch that skips the view kick after a shot (recoil only ever applies to players) |
| No overheat | Keeps plasma weapon heat at zero. Your weapons only |
| Infinite equipment | Your equipment stays in your hands after you use it (usable again once its effect ends) |
| No equipment despawn | Equipment you deploy (bubble shield, regenerator, ...) doesn't expire. Enemies' equipment is unaffected |
| Third person | Switches your camera to the game's third-person following camera; cutscene and death cameras are left alone |
| Camera distance | Moves the third-person camera back by a slider amount |
| No HUD | Hides the HUD, the same as the game's chud_show script command |

Every cheat restores the original values when switched off. Tag edits are applied again after a map or checkpoint reload.

Cheats that call engine functions (noclip, damage, give weapon) run on the game's main thread. The mod reaches that thread through hooks on each title's main loop or tick, or through MCC's input polling (`get_key_state`).

The code patches are found by byte pattern (`core/patch.cpp`), so they survive small updates. Several come from a Cheat Engine table made for the Windows Store build; the patterns also match the Steam build.

## Finding offsets

Offsets in each title's `offsets.h` note the DLL address (RVA) where they were confirmed. Most came from disassembling the engine's own HaloScript functions:
- **Function table:** the engine keeps a table of every script function (name and evaluator at `+0x20`). For Halo 3 it's at RVA `0x7F6AD0`, with 1,379 entries in the order of Assembly's `H3MCC_U14_Scripting.xml`. The other titles are similar (Halo 4's is built at runtime and is dumped from the running game).
- **Globals table:** RVA `0x7D36A0` in Halo 3, 1,246 entries.

The retail `cheat_*` HaloScript globals have no storage (their address field is 0), so they can't be used, except in Halo: CE. Scripts for this are in `tools/re/`.

## Layout

| Folder | What's in it |
|---|---|
| `src/core/` | `VERSION.dll` proxy (forwards every export to System32), patching and logging |
| `src/mcc/` | Hooks into MCC itself, shared by all titles: title DLL load/unload, input blocking through the game manager's `get_key_state`, and local co-op |
| `src/games/` | One class per title (`game.h`). `games.cpp` lists all six and routes to whichever is running. `blam.h` holds engine structures shared by Halo 3 and later |
| `src/games/halo3/` | Halo 3: `halo3.cpp` has the hooks, `cheats.cpp` the cheat logic, `tags.cpp` tag lookup, and `offsets.h` the offsets |
| `src/games/halo1/`, `halo2/`, `odst/`, `reach/`, `halo4/` | The other titles, laid out the same way (`odst/` is a port of `halo3/`, `halo4/` of `reach/`) |
| `src/ui/` | D3D11 `Present` hook with the ImGui menu (`overlay.cpp`), XInput controller reading (`gamepad.cpp`), saved settings and menu configuration |
| `tools/re/` | Reverse-engineering scripts per title (disassembly, script-table readers, live memory helpers in `tools/re/live/`). Needs your own memory dumps; `python mkimg.py` builds them from a running game |
| `tools/ToggleHaloModMenu/` | The desktop on/off toggle |

**Adding a title:**
1. Create `src/games/<title>/` with a `Game` subclass and its offsets.
2. Add it to `games.cpp`, and add the `.cpp` files to `CMakeLists.txt`.

Offsets for the main loop and a few other hooks came from AlphaRing's 1.3528.0.0 tables. After a game update everything must be found again.

## Credits

- Local co-op and several engine offsets: [AlphaRing](https://github.com/WinterSquire/AlphaRing) by WinterSquire.
- Code patches: a community Cheat Engine table for MCC.
- Tag layouts: the Assembly plugin files for each title.

Third-party code, vendored in `third_party/`: Dear ImGui v1.91.9, MinHook (master, 2026-09-08) and miniz.
