#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Games {
    // A second word of cheats a title can support (the first is full).
    enum Feature2 : uint64_t {
        kDoubleJump = 1ull << 0,  // Player page: jump again in mid-air (double or infinite)
        kAiScale    = 1ull << 1,  // Fun page: giant / tiny AI
        kRandomSwap = 1ull << 2,  // Chaos page: every AI becomes a random character
        kSwordFly   = 1ull << 3,  // Combat page (Halo 2): the sword's reticle and lunge reach any distance
        kInfiniteSprint = 1ull << 5,  // Player page (Halo 4): sprint never runs out
        kFov        = 1ull << 6,  // Player page: field of view slider
        kGrenadeDrop = 1ull << 7,  // Chaos page: "We all feel down sometimes", a grenade under every AI
        kExplosionSize = 1ull << 9,  // Weapons page: explosion size slider
        kBombingRun = 1ull << 10,  // Chaos page: flying vehicles fly over side by side dropping grenades
        kDot        = 1ull << 4,  // Player page: a permanent reticle dot (offset to the shot in third person)
        kBoundaryView = 1ull << 11,  // World page: draw the invisible boundaries (kill volumes, soft ceilings)
    };

    // Cheats a title can support. The menu shows every one and greys out
    // the ones the running title doesn't list in Game::Features().
    enum Feature : uint64_t {
        kGodMode          = 1ull << 0,
        kInfiniteAmmo     = 1ull << 1,
        kBottomlessClip   = 1ull << 2,
        kInfiniteGrenades = 1ull << 3,
        kOneHitKill       = 1ull << 4,
        kSpeed            = 1ull << 5,
        kJump             = 1ull << 6,
        kGravity          = 1ull << 7,
        kGameSpeed        = 1ull << 8,
        kFly              = 1ull << 9,
        kNoclip           = 1ull << 10,
        kNoBoundaries     = 1ull << 11,
        kDamage           = 1ull << 12,
        kGiveWeapon       = 1ull << 13,
        kAmmoType         = 1ull << 14,
        kAimbot           = 1ull << 15,
        kAiBlind          = 1ull << 16,
        kAiCantShoot      = 1ull << 17,
        kFullAuto         = 1ull << 18,
        kRapidFire        = 1ull << 19,
        kNoSpread         = 1ull << 20,
        kNoRecoil         = 1ull << 21,
        kNoOverheat       = 1ull << 22,
        kInfiniteEquipment = 1ull << 23,
        kNoEquipmentDespawn = 1ull << 24,
        kAiDeaf           = 1ull << 25,
        kThirdPerson      = 1ull << 26,
        kNoHud            = 1ull << 27,
        kCameraDistance   = 1ull << 28,
        kEnvySkull        = 1ull << 29,  // Halo 2: active camo on the flashlight button
        kVehicleSpeed     = 1ull << 30,
        kVehicleRapidFire = 1ull << 31,
        kVehicleGodMode   = 1ull << 32,
        kVehicleNoHijack  = 1ull << 33,  // Reach: enemies can't board your vehicle
        kVehicleNoEmp     = 1ull << 34,  // Reach: EMP (overcharged plasma pistol) doesn't shut it down
        kVehicleInfiniteBoost = 1ull << 35,
        kVehicleNoRecoil  = 1ull << 36,  // the guns of the vehicle you're in
        kVehicleNoEject   = 1ull << 37,  // not thrown out when the vehicle flips
        kCheckpoints      = 1ull << 38,  // save / load a checkpoint
        kSpawning         = 1ull << 39,  // spawn the selected tag where you're aiming
        kPilotAiVehicles  = 1ull << 40,  // Reach: drive vehicles only AI may (Pelican, Phantom)
        kUpgradeVehicle   = 1ull << 41,  // Reach: swap your Falcon/Banshee for a Pelican/Phantom
        kTakePilotSeat    = 1ull << 42,  // Reach: a key that puts you in the pilot seat of an AI-only aircraft
        kAiStayDead       = 1ull << 43,  // Reach: the mission's invulnerable AI (Spartans) can die and aren't revived
        kSkulls           = 1ull << 44,  // Skulls page: force skulls on (Status::skullNames)
        kChaosGun         = 1ull << 45,  // every shot is a random projectile
        kRain             = 1ull << 46,  // random things drop from the sky around you
        kRecoilPush       = 1ull << 47,  // each shot pushes the shooter back (rocket jumps)
        kTeleport         = 1ull << 48,  // a key that teleports you where you're aiming
        kUpgradeAi        = 1ull << 49,  // the mission's AI move up a rank (grunt > grunt major ...)
        kRandomTeams      = 1ull << 50,  // the mission's AI are put on random teams
        kSquadSpawning    = 1ull << 52,  // Squads page: spawn several AI together
        kNoFriendlyFire   = 1ull << 55,  // you and your allies can't hurt each other
        kNoBetrayal       = 1ull << 56,  // living allies never turn against you
        kPoisonTouch      = 1ull << 58,  // AI page: AI that touch you die, friend or foe
        kCheckpointLock   = 1ull << 59,  // Game State page: the game can't save new checkpoints
        kPerfectCamo      = 1ull << 61,  // Equipment page: active camo ignores how fast you move
        kLaunchOnHit      = 1ull << 63,  // Fun page: every hit sends the enemy flying
        kTimeStop         = 1ull << 62,  // Fun page: every AI freezes while you keep moving
        kCheckpointHistory = 1ull << 60,  // Game State page: go back to any checkpoint saved this mission
        kGiveEquipment    = 1ull << 57,  // Equipment page: swap your armor ability for a chosen one
        kNuke             = 1ull << 53,  // AI page: kill every AI in the level
        kDespawnAi        = 1ull << 54,  // Spawning page: delete every AI the mod spawned
        kPlayerTeam       = 1ull << 51,  // the player switches sides (Covenant, or everyone's enemy)
    };

    // A tag the menu can pick from (a weapon to give, a projectile to fire).
    struct TagChoice {
        uint32_t datum;
        std::string name;
    };

    // What the user has switched on in the menu. Applies to whichever title
    // is running; titles ignore what they don't support.
    struct Settings {
        bool godMode = false;
        bool infiniteAmmo = false;
        bool bottomlessClip = false;
        bool infiniteGrenades = false;
        bool oneHitKill = false;
        bool fly = false;
        bool noclip = false;
        bool noBoundaries = false;
        bool showBoundaries = false;     // draw kill volumes as wireframes
        bool showSoftCeilings = false;   // draw soft ceilings (invisible walls and ceilings) as tinted triangles
        bool softCeilingsAll = false;    // ... including the ones that don't affect you (or are removed)
        bool boundaryHide = true;        // hide boundaries behind walls and objects (a ray test per piece, see Reach boundary_view.cpp)
        float boundaryOpacity = 0.2f;    // fill opacity of the tinted boundaries
        float boundaryRange = 10.0f;     // only boundaries this near the camera (world units); 0 = all
        bool aimbot = false;
        bool aiBlind = false;       // enemies can't see you
        bool aiCantShoot = false;
        bool fullAuto = false;      // weapon mods: the player's weapons only
        bool rapidFire = false;
        bool noSpread = false;
        bool noRecoil = false;
        bool noOverheat = false;
        bool infiniteEquipment = false;  // equipment: the player's only
        bool noEquipmentDespawn = false;
        float fov = 0.0f;            // field of view in degrees; 0 = leave the game's own setting
        uint32_t grenadeSerial = 0;      // bumped by "Drop a grenade under every AI"
        bool grenadeEnemiesOnly = false; // ... only the ones that aren't on your side
        bool grenadeAuto = false;        // ... and under each new AI as it appears
        uint32_t bombingRunSerial = 0;   // bumped by "Call in a bombing run"
        int bombingRunCount = 4;         // aircraft flying abreast
        int bombingRunType = 0;          // 0 marines (frag), 1 covenant (plasma), 2 brutes (spike), 3 white phosphorus (firebomb)
        float explosionScale = 1.0f; // explosion size (blast radius) multiplier; 1 = the game's own
        bool infiniteSprint = false; // Halo 4: sprint has no time limit
        bool dot = false;            // permanent reticle dot, drawn by the mod
        float dotColor[3] = {1.0f, 0.15f, 0.15f};
        float dotOffsetX = -88.0f;   // third person: where shots land, pixels from the centre at 1080p (x right)
        float dotOffsetY = -6.0f;    // (y down)
        float dotSize = 3.0f;        // radius in pixels at 1080p
        bool swordFly = false;       // Halo 2: holding the sword, targets are found at any range
        bool doubleJump = false;     // jump again in mid-air, `airJumps` times
        bool infiniteJump = false;   // ... with no limit
        float aiScaleEnemy = 1.0f;     // size of enemy AI (1 = normal)
        float aiScaleFriendly = 1.0f;  // size of AI on your side
        float airJumps = 1.0f;       // extra jumps per trip off the ground (1 = double jump)
        bool perfectCamo = false;   // Reach: active camo stays fully effective while moving
        bool aiDeaf = false;        // AI can't hear
        bool aiStayDead = false;    // the mission's AI can die for good (no script immortality, no revives)
        bool thirdPerson = false;
        bool noHud = false;
        bool envySkull = false;
        bool vehicleRapidFire = false;  // the weapons of the vehicle/turret you're in
        bool vehicleGodMode = false;    // the vehicle/turret you're in, and what it's mounted on
        bool vehicleNoHijack = false;
        bool vehicleNoEmp = false;
        bool vehicleInfiniteBoost = false;
        bool vehicleNoRecoil = false;
        bool vehicleNoEject = false;
        bool pilotAiVehicles = false;
        uint32_t ammoType = 0xFFFFFFFF;  // projectile tag datum; none = weapon's own
        uint32_t giveWeapon = 0xFFFFFFFF;  // weapon tag datum to give
        uint32_t giveWeaponSerial = 0;     // bumped each time "Give" is pressed
        uint32_t giveEquipment = 0xFFFFFFFF;  // equipment tag datum to give
        uint32_t giveEquipmentSerial = 0;
        uint32_t saveCheckpointSerial = 0;  // bumped by "Save checkpoint"
        uint32_t loadCheckpointSerial = 0;  // bumped by "Load checkpoint"
        bool lockCheckpoints = false;       // the game's own checkpoint saves are ignored (the Save button still works)
        uint32_t restoreCheckpoint = 0;     // history entry id to go back to
        uint32_t restoreCheckpointSerial = 0;
        bool spawnEnabled = false;          // the spawn key works
        uint32_t spawnTag = 0xFFFFFFFF;     // tag datum the spawn key creates
        uint32_t spawnSerial = 0;           // bumped each time the spawn key is pressed
        uint32_t upgradeVehicleSerial = 0;  // bumped by "Upgrade vehicle"
        uint32_t takeSeatSerial = 0;        // bumped by the take-pilot-seat key
        bool spawnRespawn = false;          // spawned AI come back after dying (the game revives them)
        bool spawnRandomGuns = true;        // Reach: spawned Elites, Brutes, Grunts and marines get a random gun
        int spawnTeam = 0;                  // spawned AI: 0 = their own team, 1 = ally, 2 = enemy
        int spawnSquad = -1;                // Squads page: index into kSquads the spawn key makes instead of spawnTag; -1 = none
        float cameraDistance = 0.5f;  // third-person camera: extra distance back, world units (0 = game default)
        float aimbotStrength = 0.0f;  // percent; above 0 the aimbot also turns your view
        float damage = 1.0f;        // damage enemies take
        float speed = 1.0f;      // multipliers; 1 = normal
        float jump = 1.0f;
        float gravity = 1.0f;
        float gameSpeed = 1.0f;
        float vehicleSpeed = 1.0f;  // the vehicle you're driving (its tag: same-type vehicles too)
        float vehicleGrip = 1.0f;   // how hard it holds a turn (on top of what speed scales)
        // Skulls page: bit n forces skull n on (the title's own numbering, see
        // Status::skullNames). Skulls not forced keep whatever MCC set.
        uint64_t skulls = 0;
        // Chaos page
        bool chaosGun = false;       // the players' shots are a random projectile each
        bool rain = false;           // things drop from the sky around player 1
        int rainKind = 0;            // 0 = everything, 1 = vehicles, 2 = objects, 3 = AI, 4 = weapons, 5 = the spawn selection
        float rainInterval = 2.0f;   // seconds between drops
        float rainMax = 20.0f;       // at most this many dropped things at once (oldest removed)
        float rainRadius = 15.0f;    // metres: drops land between 30% of this and this far from player 1
        // Random AI teams (Chaos page): the button re-rolls every AI in range;
        // auto gives each AI one roll as it shows up.
        uint32_t randomTeamsSerial = 0;  // bumped by "Randomize AI teams now"
        bool randomTeamsAuto = false;
        bool randomTeamsSpartans = false;  // Spartans too
        float randomTeamsRange = 0.0f;     // metres from player 1, 0 = everywhere
        float randomTeamsFactions = 3.0f;  // how many teams they're spread over (2-6; humans are one)
        // AI page: Nuke kills every AI; Spawning page: despawn deletes the AI the mod spawned.
        uint32_t nukeSerial = 0;
        uint32_t despawnSerial = 0;
        bool nukeSpareAllies = false;  // the nuke leaves your side's AI alone
        bool noFriendlyFire = false;   // no damage between you and your allies, either way
        bool noBetrayal = false;       // allied AI never turn hostile, however much you hurt them
        bool timeStop = false;         // every AI (and its weapons) freezes in place; you keep moving
        bool poisonTouch = false;      // any AI that touches you dies, friendly or enemy
        // Your team (Player page): 0 = normal, 1 = Covenant, 2 = rogue (everyone's enemy).
        int playerTeam = 0;
        // Upgrade AI (AI page): the button moves every AI in range up a rank;
        // auto upgrades each AI once as it shows up.
        uint32_t upgradeAiSerial = 0;    // bumped by "Upgrade AI now"
        uint32_t randomSwapSerial = 0;   // bumped by "Swap all AI now"
        bool randomSwapAuto = false;     // each new AI becomes a random character
        bool randomSwapAllies = false;   // ... allies too
        bool upgradeAiAuto = false;
        bool upgradeAiAllies = false;    // Marines/troopers too
        float upgradeAiRange = 0.0f;     // metres from player 1, 0 = everywhere
        // Fun page
        bool recoilPush = false;     // each shot pushes the shooter (or their vehicle) away from where they aim
        bool launchOnHit = false;    // enemies whose health or shield just dropped are thrown upward
        float launchStrength = 12.0f;  // upward speed given, world units per second (gravity is about 4)
        float recoilStrength = 3.0f; // world units per second added per shot
        bool teleportEnabled = false;  // the teleport key works
        uint32_t teleportSerial = 0;   // bumped each time the teleport key is pressed
    };

    constexpr int kRainKindCount = 6;

    // Where the reticle dot goes in third person, by title (pixels from the
    // centre at 1080p). Tuned in Reach; Halo 4 shoots from the centre in
    // third person, so it has none.
    inline void DefaultDotOffset(const char* title, float& x, float& y) {
        bool centred = title && (strcmp(title, "Halo 4") == 0 || strcmp(title, "Halo: CE") == 0 || strcmp(title, "Halo 2") == 0);
        x = centred ? 0.0f : -88.0f;
        y = centred ? 0.0f : -6.0f;
    }

    // Player controls the cheats react to, already cleared while the menu is open.
    struct FrameInput {
        bool noclipForward = false;  // fly where you're looking
        bool fast = false;
        bool firing = false;         // right trigger / left mouse (aimbot pull)
        bool exitHeld = false;       // X / E, the game's default vehicle exit (held)
        bool jumpHeld = false;       // A / Space, the game's default jump (held)
        uint32_t jumpPresses = 0;    // counts every new press, so a tap between two ticks isn't missed
    };

    // Boundary view: what a title hands the overlay to draw in world space.
    // Rebuilt by the title every frame it's on; valid until the next Tick.
    struct BoundaryView {
        struct Line { float a[3], b[3]; uint32_t color; };  // color: 0xAABBGGRR as ImGui packs it
        struct Tri { float v[3][3]; uint32_t color; bool outline; float dist; };  // dist: nearest point to the camera   // filled (opacity from the settings); color as for Line
        bool valid = false;          // camera below is real
        float pos[3] = {}, fwd[3] = {}, up[3] = {};
        float tanHalfV = 0.5f;       // tan(half the vertical field of view)
        float opacity = 0.2f;        // fill alpha, 0-1
        float fadeRange = 120.0f;    // fills fade out toward this distance
        struct { bool valid = false; float p[3] = {}; float dist = 0; } nearest;  // the closest boundary point to the camera
        std::vector<Line> lines;
        std::vector<Tri> tris;
    };

    // What the menu shows about the running title.
    // A checkpoint kept in the history (this mission).
    struct CheckpointEntry {
        uint32_t id = 0;
        int64_t when = 0;   // unix seconds it was saved
        std::string label;  // clock time, "14:05:09"
    };

    struct Status {
        const char* title = nullptr;  // nullptr when no title is running
        bool supported = false;       // we have offsets for this title
        uint64_t features = 0;        // Feature bits
        uint64_t features2 = 0;       // Feature2 bits
        bool inGame = false;          // gameplay state found (not a loading screen)
        int playerCount = 0;          // local player units found
        float health = 0.0f;          // player 1, normalized
        float shield = 0.0f;
        // Pickable tags in the loaded map. Owned by the game; valid until the next Tick.
        const std::vector<TagChoice>* weapons = nullptr;
        const std::vector<CheckpointEntry>* checkpoints = nullptr;  // newest last; valid until the next Tick
        const std::vector<TagChoice>* equipment = nullptr;  // armor abilities and other equipment tags
        const std::vector<TagChoice>* projectiles = nullptr;
        // Spawning: what can be spawned, by kind (nullptr = not available yet
        // in this title; weapons use `weapons`), and the latest result.
        const std::vector<TagChoice>* spawnVehicles = nullptr;
        const std::vector<TagChoice>* spawnObjects = nullptr;
        const std::vector<TagChoice>* spawnAi = nullptr;
        uint32_t spawnDone = 0;       // spawn requests handled so far (changes when one finishes)
        char spawnMessage[96] = "";   // what happened to the latest ("Spawned ...", "Nothing in range")
        // Skulls page: names by bit (nullptr = no skull at that bit), and
        // which are on in the game right now (forced or set by MCC).
        const char* const* skullNames = nullptr;
        int skullCount = 0;
        uint64_t skullsActive = 0;
        // Squads page: bit n set if kSquads[n] has at least one AI this map can make.
        uint64_t squadsAvailable[4] = {};
        // Upgrade AI: whether this map has any rank to move enemies / allies up to (true unless a title says otherwise).
        bool upgradeEnemiesApplicable = true;
        bool upgradeAlliesApplicable = true;
        // Field of view slider (Feature2::kFov): the title's range and its own default, in degrees.
        float fovMin = 0.0f, fovMax = 0.0f, fovDefault = 0.0f;
        const BoundaryView* boundaryView = nullptr;  // nullptr = nothing to draw
    };

    // One MCC title. Subclass it to add support; the base class is a title
    // we know about but can't mod yet.
    //
    // MCC keeps several titles' DLLs loaded at once, so a loaded title isn't
    // necessarily the one being played. Only the running title gets Tick().
    class Game {
    public:
        // titleId is MCC's number for the title (see MCC::Running).
        Game(const char* title, const wchar_t* dll, int titleId)
            : m_title(title), m_dll(dll), m_titleId(titleId) {}
        virtual ~Game() = default;

        const char* Title() const { return m_title; }
        const wchar_t* Dll() const { return m_dll; }
        int TitleId() const { return m_titleId; }

        // Bookkeeping done by games.cpp around Load/Unload.
        HMODULE module = nullptr;  // nullptr when not loaded
        bool supported = false;    // Load() succeeded

        // Called after the title's DLL loads. Return false if unsupported.
        virtual bool Load(HMODULE) { return false; }
        // Called before the DLL unloads. Remove any hooks into it here.
        virtual void Unload() {}

        virtual uint64_t Features() const { return 0; }
        virtual uint64_t Features2() const { return 0; }

        // Called every frame (render thread) while this is the running title.
        // Apply settings and fill in the status fields you know.
        virtual void Tick(const Settings&, const FrameInput&, Status&) {}

        // Called once per game tick on the title's main thread (from MCC's
        // input polling). The only safe place to call engine functions.
        virtual void MainThreadTick(const Settings&, const FrameInput&) {}

    private:
        const char* m_title;
        const wchar_t* m_dll;
        int m_titleId;
    };
}
