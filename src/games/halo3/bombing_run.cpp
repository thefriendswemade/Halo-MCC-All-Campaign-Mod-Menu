// Halo 3: the Chaos page's bombing run. A button starts a countdown ("take cover"), then flying
// vehicles spawn behind the spot ahead of player 1 and fly over it in a line, side by side,
// dropping grenades. They aren't piloted: each tick they are moved along the path with the
// engine's set-position and set-velocity calls. Everything is deleted when the run ends, or
// after kMaxLifeSeconds if something goes wrong.
// Halo3Bombing::Tick runs on the game thread once per game tick inside SEH.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "internal.h"
#include "offsets.h"
#include "tags.h"

using namespace Halo3Cheats;

namespace {
    // --- Tuning (world units; 1 = about 3 m) -----------------------------------
    constexpr float kCountdownSeconds = 5.0f;
    constexpr float kFlightSpeed = 4.0f;       // units per second along the path: a slow rollover
    constexpr float kAhead = 60.0f;            // the run's centre: this far ahead of the player when it's called
    constexpr float kHalfRun = 35.0f;          // aircraft start and end this far either side of the centre
    constexpr float kAltitude = 18.0f;         // above the player
    constexpr float kBombHalf = 25.0f;         // grenades drop while within this of the centre
    constexpr float kBombsPerSecond = 50.0f;   // across the whole formation, however many aircraft
    constexpr float kMinDropInterval = 0.04f;  // seconds between one aircraft's grenades, at the least
    constexpr float kScatter = 1.5f;           // random sideways speed, so the grenades spread into a carpet
    constexpr float kFallSpeed = 4.0f;         // downward speed they start with
    constexpr float kMaxLifeSeconds = 60.0f;   // hard stop: anything still around is deleted
    constexpr float kMinSpacing = 4.0f;        // between neighbours in a row
    constexpr float kSpacingPerRadius = 2.0f;  // ... or this many bounding radii, if larger
    constexpr float kRowGap = 0.9f;            // distance between rows, in spacings
    constexpr float kDropClearance = 1.5f;     // grenades start this far under the hull
    constexpr int kMaxAircraft = 12;

    using SetPositionFn = void(__fastcall*)(uint32_t object, const float* position, const float* forward,
                                            const float* up, const void* location, bool);
    using SetVelocityFn = void(__fastcall*)(uint32_t object, const float* linear, const float* angular);

    template <typename Fn>
    Fn Engine(int rva) { return (Fn)(Halo3Internal::Module() + rva); }

    enum class Phase { Idle, Countdown, Flying };

    struct Aircraft {
        uint32_t datum = 0xFFFFFFFF;
        float offset = 0.0f;     // sideways from the path's centre line
        float back = 0.0f;       // behind the formation's centre along the path (negative: ahead of it)
        float nextDrop = -1.0f;  // run time of its next grenade (-1: not in the drop zone yet)
    };

    struct Run {
        Phase phase = Phase::Idle;
        float timer = 0.0f;       // countdown left / seconds flown
        int shown = 0;            // last countdown number put on screen
        float centre[3] = {};
        float dir[2] = {1.0f, 0.0f};
        float yaw = 0.0f;
        int count = 1;
        uint32_t vehicle = 0xFFFFFFFF;
        float radius = 0.0f;
        uint32_t projectile = 0xFFFFFFFF;
        float spacing = kMinSpacing;
        float tail = 0.0f;        // how far behind the centre the rearmost aircraft is
        float dropInterval = 0.2f;
        int ordnance = 0;
        int dropped = 0, failed = 0;  // grenades made / refused, for the log
        std::vector<Aircraft> craft;
    } g_run;

    // matg's grenade slots, which are the four ordnance types the Chaos page offers.
    constexpr const char* kOrdnanceShort[] = {"Marine", "Covenant", "Brute", "White phosphorus"};

    uint32_t g_serial = 0;
    bool g_haveSerial = false;
    ULONGLONG g_lastTick = 0;

    float Seconds() {
        ULONGLONG now = GetTickCount64();
        float dt = g_lastTick ? (float)(now - g_lastTick) / 1000.0f : 0.0f;
        g_lastTick = now;
        return dt > 0.1f ? 0.1f : dt;
    }

    // The projectile of matg's grenade slot `type` (0 frag, 1 plasma, 2 spike, 3 firebomb), or -1.
    uint32_t GrenadeProjectile(int type) {
        char* matg = Halo3Tags::GetGlobal(HALO3_TAG_MATG);
        if (type < 0 || type >= HALO3_UNIT_GRENADE_TYPES || type >= Halo3Tags::BlockCount(matg, HALO3_MATG_GRENADES))
            return 0xFFFFFFFF;
        char* def = Halo3Tags::BlockElement(matg, HALO3_MATG_GRENADES, HALO3_MATG_GRENADE_DEF_SIZE, type);
        if (!def) return 0xFFFFFFFF;
        uint32_t projectile = Field<uint32_t>(def, HALO3_MATG_GRENADE_PROJECTILE + HALO3_TAGREF_DATUM);
        return projectile != 0xFFFFFFFF && Halo3Tags::Get(projectile) ? projectile : 0xFFFFFFFF;
    }

    bool Flies(char* tag) {
        return Halo3Tags::BlockCount(tag, HALO3_VEHI_PLANE) > 0 || Halo3Tags::BlockCount(tag, HALO3_VEHI_FIGHTER) > 0 ||
               Halo3Tags::BlockCount(tag, HALO3_VEHI_VTOL) > 0;
    }

    // Flying vehicle tags in this map, smallest first (the small ones fly side by side best).
    std::vector<uint32_t> Flyers() {
        constexpr const char* kSkip[] = {"cinematic", "cin_", "damaged", "wreck", "prop", "filler", "turret", "gun"};
        struct Candidate { uint32_t datum; float radius; };
        std::vector<Candidate> found;
        for (auto& vehicle : Halo3Tags::ListGroup(HALO3_TAG_VEHI)) {
            char* tag = Halo3Tags::Get(vehicle.datum);
            if (!tag || !Flies(tag)) continue;
            std::string name = Halo3Tags::FullName(vehicle.datum);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
            bool skip = false;
            for (const char* word : kSkip) skip |= name.find(word) != std::string::npos;
            if (!skip) found.push_back({vehicle.datum, Field<float>(tag, HALO3_OBJE_BOUNDING_RADIUS)});
        }
        std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.radius < b.radius; });
        std::vector<uint32_t> out;
        for (auto& c : found) out.push_back(c.datum);
        return out;
    }

    void Delete(Run& run) {
        for (auto& a : run.craft)
            if (a.datum != 0xFFFFFFFF) Halo3Internal::DeleteObject(a.datum);
        run.craft.clear();
    }

    void Finish(Run& run, const char* why) {
        Delete(run);
        run.phase = Phase::Idle;
        LOG("Halo 3: bombing run over (%s): %d grenades dropped, %d refused", why, run.dropped, run.failed);
    }

    // Where aircraft `a` is when the formation has flown `along` units from the centre.
    void PositionOf(const Run& run, const Aircraft& a, float along, float* out) {
        float left[2] = {-run.dir[1], run.dir[0]};
        along -= a.back;
        out[0] = run.centre[0] + run.dir[0] * along + left[0] * a.offset;
        out[1] = run.centre[1] + run.dir[1] * along + left[1] * a.offset;
        out[2] = run.centre[2];
    }

    // A pyramid: one leader, then two behind him, then three behind them, and so on; each row is
    // centred, the last one partly filled. Sideways and back are in world units, the back measured
    // from the formation's middle so the whole group (not the leader) is centred on the run.
    std::vector<Aircraft> Formation(int count, float spacing) {
        std::vector<Aircraft> craft;
        for (int row = 0; (int)craft.size() < count; ++row) {
            int inRow = std::min(row + 1, count - (int)craft.size());
            for (int i = 0; i < inRow; ++i) {
                Aircraft a;
                a.offset = ((float)i - (float)(inRow - 1) * 0.5f) * spacing;
                a.back = (float)row * spacing * kRowGap;
                craft.push_back(a);
            }
        }
        float mean = 0.0f;
        for (auto& a : craft) mean += a.back;
        mean /= (float)craft.size();
        for (auto& a : craft) a.back -= mean;
        return craft;
    }

    // Makes the aircraft in formation at the start. False if no flyer in this map
    // could be made (not loaded in this part of the level, for example).
    bool Spawn(Run& run) {
        float spacing = 0.0f;
        for (uint32_t tag : Flyers()) {
            char* def = Halo3Tags::Get(tag);
            float radius = def ? Field<float>(def, HALO3_OBJE_BOUNDING_RADIUS) : 0.0f;
            spacing = std::max(kMinSpacing, radius * kSpacingPerRadius);
            run.spacing = spacing;
            run.craft.clear();
            run.tail = 0.0f;
            for (Aircraft a : Formation(run.count, spacing)) {
                float at[3];
                PositionOf(run, a, -kHalfRun, at);
                a.datum = Halo3Internal::NewObject(tag, at, run.yaw);
                if (a.datum == 0xFFFFFFFF) break;
                run.tail = std::max(run.tail, a.back);
                run.craft.push_back(a);
            }
            if ((int)run.craft.size() == run.count) {
                run.vehicle = tag;
                run.radius = radius;
                LOG("Halo 3: bombing run: %d x %s, spacing %.1f", run.count, Halo3Tags::FullName(tag).c_str(), spacing);
                return true;
            }
            Delete(run);  // this kind didn't work: try the next
        }
        return false;
    }

    void Show(const char* text) { Halo3Internal::Toast(text); }

    void Start(void** state, const Players& p, const Games::Settings& s) {
        if (g_run.phase != Phase::Idle) {
            Show("A bombing run is already on its way");
            return;
        }
        if (p.count == 0) {
            Show("Bombing run: no player");
            return;
        }
        if (Flyers().empty()) {
            Show("Bombing run: not available (no flying vehicle in this map)");
            return;
        }
        int type = std::max(0, std::min(s.bombingRunType, HALO3_UNIT_GRENADE_TYPES - 1));
        uint32_t projectile = GrenadeProjectile(type);
        if (projectile == 0xFFFFFFFF) {
            char text[96];
            sprintf_s(text, "Bombing run: not available (no %s grenade in this map)", kOrdnanceShort[type]);
            Show(text);
            return;
        }
        g_run.ordnance = type;
        auto control = (char*)state[HALO3_TLS_SLOT_PLAYER_CONTROL];
        if (!control) {
            Show("Bombing run: no player");
            return;
        }
        Run& run = g_run;
        run.yaw = Field<float>(control, HALO3_PLAYER_CONTROL_YAW);
        run.dir[0] = cosf(run.yaw);
        run.dir[1] = sinf(run.yaw);
        // The centre is where the player is looking (the spawn key's ray); if that hits nothing,
        // a spot kAhead in front of them.
        float aim[3], look[3], facing;
        if (Halo3Internal::AimPoint(state, p, aim, look, &facing)) {
            memcpy(run.centre, aim, sizeof(aim));
        } else {
            const float* at = &Field<float>(p.units[0], HALO3_OBJECT_POSITION);
            run.centre[0] = at[0] + run.dir[0] * kAhead;
            run.centre[1] = at[1] + run.dir[1] * kAhead;
            run.centre[2] = at[2];
            Show("Bombing run: nothing in sight to aim at, using the spot ahead of you");
        }
        run.centre[2] += kAltitude;
        run.count = std::max(1, std::min(s.bombingRunCount, kMaxAircraft));
        run.projectile = projectile;
        run.dropInterval = std::max(kMinDropInterval, (float)run.count / kBombsPerSecond);
        run.dropped = run.failed = 0;
        run.craft.clear();
        run.timer = kCountdownSeconds;
        run.shown = 0;
        run.phase = Phase::Countdown;
        LOG("Halo 3: bombing run called: centre %.1f %.1f %.1f, heading %.2f, %d aircraft", run.centre[0], run.centre[1],
            run.centre[2], run.yaw, run.count);
    }

    void Countdown(Run& run, float dt) {
        run.timer -= dt;
        int number = (int)ceilf(run.timer);
        if (number != run.shown && number > 0) {
            run.shown = number;
            char text[96];
            sprintf_s(text, "%s bombing run inbound: %d  -  take cover!", kOrdnanceShort[run.ordnance], number);
            Show(text);
        }
        if (run.timer > 0.0f) return;
        if (!Spawn(run)) {
            run.phase = Phase::Idle;
            Show("Bombing run: not available (the aircraft couldn't be made here)");
            return;
        }
        run.timer = 0.0f;
        run.phase = Phase::Flying;
        Show("Bombing run!");
    }

    float Random(float range) { return ((float)(rand() % 2001) / 1000.0f - 1.0f) * range; }  // -range .. +range

    // One grenade from under the hull, anywhere across the gap to the next aircraft, falling
    // almost straight down (a little random sideways speed spreads the carpet).
    void Drop(Run& run, const float* from) {
        float left[2] = {-run.dir[1], run.dir[0]};
        float side = Random(run.spacing * 0.5f);
        float at[3] = {from[0] + left[0] * side, from[1] + left[1] * side, from[2] - run.radius - kDropClearance};
        uint32_t bomb = Halo3Internal::NewObject(run.projectile, at, run.yaw);
        if (bomb == 0xFFFFFFFF) {
            if (++run.failed == 1) LOG("Halo 3: bombing run: the game refused to make a grenade");
            return;
        }
        ++run.dropped;
        float v[3] = {Random(kScatter), Random(kScatter), -kFallSpeed};
        Engine<SetVelocityFn>(OFFSET_HALO3_OBJECT_SET_VELOCITY)(bomb, v, nullptr);
    }

    void Fly(Run& run, Blam::DataArray* objects, float dt) {
        run.timer += dt;
        float along = -kHalfRun + kFlightSpeed * run.timer;
        bool done = along - run.tail > kHalfRun;  // the rearmost aircraft has passed the end
        if (done || run.timer > kMaxLifeSeconds) {
            Finish(run, done ? "finished" : "timed out");
            return;
        }
        const float velocity[3] = {run.dir[0] * kFlightSpeed, run.dir[1] * kFlightSpeed, 0.0f};
        const float still[3] = {};
        for (auto& a : run.craft) {
            if (a.datum == 0xFFFFFFFF) continue;
            if (!Blam::ObjectFromDatum(objects, a.datum)) {  // shot down, or gone
                a.datum = 0xFFFFFFFF;
                continue;
            }
            float at[3];
            PositionOf(run, a, along, at);
            // A null location is 64-bit null (a 32-bit 0 left garbage in the high half; see noclip).
            Engine<SetPositionFn>(OFFSET_HALO3_OBJECT_SET_POSITION)(a.datum, at, nullptr, nullptr, nullptr, false);
            Engine<SetVelocityFn>(OFFSET_HALO3_OBJECT_SET_VELOCITY)(a.datum, velocity, still);

            if (fabsf(along - a.back) > kBombHalf) continue;  // each aircraft drops over the same stretch
            if (a.nextDrop < 0.0f) a.nextDrop = run.timer + run.dropInterval * (float)(rand() % 100) / 100.0f;
            if (run.timer >= a.nextDrop) {
                a.nextDrop += run.dropInterval;
                Drop(run, at);
            }
        }
    }
}

void Halo3Bombing::Reset() {
    g_run = {};  // the aircraft went with the map
    g_haveSerial = false;
    g_lastTick = 0;
}

void Halo3Bombing::Tick(void** state, const Games::Settings& s, const Players& p, Blam::DataArray* objects) {
    if (!Halo3Internal::Module()) return;
    __try {
        if (!g_haveSerial) {  // don't run a button press from before this session
            g_serial = s.bombingRunSerial;
            g_haveSerial = true;
        }
        float dt = Seconds();
        if (s.bombingRunSerial != g_serial) {
            g_serial = s.bombingRunSerial;
            Start(state, p, s);
        }
        if (g_run.phase == Phase::Countdown) Countdown(g_run, dt);
        else if (g_run.phase == Phase::Flying) Fly(g_run, objects, dt);
    } __except (Log::Exception(GetExceptionInformation(), "Halo 3 bombing run")) {
        g_run.craft.clear();  // faulted: don't touch what may be stale
        g_run.phase = Phase::Idle;
    }
}
