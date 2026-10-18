// Boundary view: makes the invisible things "Remove boundaries" removes
// visible. Kill volumes are drawn as wireframes from the scenario's trigger
// volumes. Soft ceilings (the invisible walls and ceilings that stop you
// jumping or flying out of the map) are explicit triangles in the BSPs'
// structure design tags (scnr +0x5C -> sddt +0x40, found 2026-10-18), drawn
// as tinted triangles. Plain level collision lives in the BSPs' Havok data
// and isn't covered.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cheat_util.h"
#include "cheats.h"
#include "core/log.h"
#include "tags.h"

using namespace ReachCheats;

namespace {
    constexpr int kObserverSlot = 11;           // TLS slot of the observer (see NOTES: camera struct +0x248)
    constexpr int kObserverPosition = 0x248;    // the drawn camera
    constexpr int kObserverForward = 0x270;
    constexpr int kObserverUp = 0x27C;
    constexpr int kObserverFov = 0x288;         // horizontal, radians
    constexpr int kObserverAspect = 0x2A0;      // the aspect the FOV is defined at (4:3)
    constexpr int kTriggerVolumeSize = 0x7C;
    constexpr int kTriggerVolumesOffset = 0x280;
    constexpr int kKillTriggerSize = 4;
    constexpr size_t kMaxCells = 6000;          // pieces per frame after subdividing
    constexpr int kRaysPerTick = 160;           // visibility tests per game tick
    constexpr float kRayBackOff = 0.25f;        // stop the ray this short of the piece so a surface it lies on doesn't hide it

    using CollisionTestFn = bool(__fastcall*)(uint64_t flags, bool, const float* point, const float* vector,
                                              int64_t ignore1, int64_t ignore2, int64_t ignore3, void* result);

    // Visibility of pieces of boundary, found by the game thread. The render
    // thread lists the pieces it wants to know about (`g_pending`: a key and a
    // point on the piece) and reads the answers (`g_known`) next frame. Unknown
    // pieces count as hidden, so a piece appears once it's been tested.
    struct Known { bool visible; ULONGLONG touched; };
    struct Sample { uint64_t key; float p[3]; };
    std::mutex g_visLock;
    std::unordered_map<uint64_t, Known> g_known;
    std::vector<Sample> g_pending;
    size_t g_cursor = 0;
    char* g_visScenario = nullptr;

    void Cross(const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    }

    void AddLine(Games::BoundaryView& v, const float* a, const float* b, uint32_t color) {
        Games::BoundaryView::Line l;
        for (int k = 0; k < 3; ++k) { l.a[k] = a[k]; l.b[k] = b[k]; }
        l.color = color;
        v.lines.push_back(l);
    }

    float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Sub3(const float* x, const float* y, float* o) { for (int k = 0; k < 3; ++k) o[k] = x[k] - y[k]; }

    // The point of segment a-b nearest p.
    void ClosestOnSegment(const float* p, const float* a, const float* b, float* out) {
        float d[3], w[3];
        Sub3(b, a, d);
        Sub3(p, a, w);
        float len = Dot3(d, d), t = len > 0 ? Dot3(w, d) / len : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        for (int k = 0; k < 3; ++k) out[k] = a[k] + d[k] * t;
    }

    // The point of triangle a-b-c nearest p: the plane projection if it falls inside, else the nearest edge point.
    void ClosestOnTriangle(const float* p, const float* a, const float* b, const float* c, float* out) {
        float ab[3], ac[3], ap[3], n[3];
        Sub3(b, a, ab); Sub3(c, a, ac); Sub3(p, a, ap);
        Cross(ab, ac, n);
        float nn = Dot3(n, n);
        if (nn > 1e-12f) {
            float k = Dot3(ap, n) / nn;
            float pt[3] = {ap[0] - n[0] * k, ap[1] - n[1] * k, ap[2] - n[2] * k};  // relative to a
            float bc[3], ca[3], bp[3], cp[3], e0[3], e1[3], e2[3];
            Sub3(c, b, bc); Sub3(a, c, ca);
            Sub3(pt, ab, bp); Sub3(pt, ac, cp);
            Cross(ab, pt, e0); Cross(bc, bp, e1); Cross(ca, cp, e2);
            if (Dot3(e0, n) >= 0 && Dot3(e1, n) >= 0 && Dot3(e2, n) >= 0) {
                for (int m = 0; m < 3; ++m) out[m] = a[m] + pt[m];
                return;
            }
        }
        float q[3][3], best = 1e30f;
        ClosestOnSegment(p, a, b, q[0]);
        ClosestOnSegment(p, b, c, q[1]);
        ClosestOnSegment(p, c, a, q[2]);
        for (auto& r : q) {
            float d[3];
            Sub3(r, p, d);
            float dd = Dot3(d, d);
            if (dd < best) { best = dd; for (int m = 0; m < 3; ++m) out[m] = r[m]; }
        }
    }

    float DistSq(const float* a, const float* b) { float d[3]; Sub3(a, b, d); return Dot3(d, d); }

    void AddTri(Games::BoundaryView& v, const float* a, const float* b, const float* c, uint32_t color, bool outline) {
        Games::BoundaryView::Tri t;
        for (int k = 0; k < 3; ++k) { t.v[0][k] = a[k]; t.v[1][k] = b[k]; t.v[2][k] = c[k]; }
        t.color = color;
        t.outline = outline;
        t.dist = 0;
        v.tris.push_back(t);
    }

    // A trigger volume as wireframe and tinted faces: a box from its corner along forward,
    // left (up x forward) and up; a sector as its footprint polygon between
    // the z bounds.
    void DrawVolume(Games::BoundaryView& v, char* vol, uint32_t color) {
        int type = Field<int16_t>(vol, 0xC);
        if (type == 0) {
            const float* fwd = &Field<float>(vol, 0x10);
            const float* up = &Field<float>(vol, 0x1C);
            const float* pos = &Field<float>(vol, 0x28);
            const float* ext = &Field<float>(vol, 0x34);
            float left[3];
            Cross(up, fwd, left);
            float c[8][3];
            for (int i = 0; i < 8; ++i)
                for (int k = 0; k < 3; ++k)
                    c[i][k] = pos[k] + ((i & 1) ? fwd[k] * ext[0] : 0) + ((i & 2) ? left[k] * ext[1] : 0) +
                              ((i & 4) ? up[k] * ext[2] : 0);
            static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                             {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (auto& e : edges) AddLine(v, c[e[0]], c[e[1]], color);
            // Six faces as two triangles each (corner bits: 1 = forward, 2 = left, 4 = up).
            static const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
            for (auto& f : faces) {
                AddTri(v, c[f[0]], c[f[1]], c[f[2]], color, false);
                AddTri(v, c[f[0]], c[f[2]], c[f[3]], color, false);
            }
        } else {
            int n = ReachTags::BlockCount(vol, 0x44);
            if (n < 3 || n > 64) return;
            float z0 = Field<float>(vol, 0x6C), z1 = Field<float>(vol, 0x70);
            for (int i = 0; i < n; ++i) {
                char* a = ReachTags::BlockElement(vol, 0x44, 0x14, i);
                char* b = ReachTags::BlockElement(vol, 0x44, 0x14, (i + 1) % n);
                if (!a || !b) return;
                float a0[3] = {Field<float>(a, 0), Field<float>(a, 4), z0}, a1[3] = {Field<float>(a, 0), Field<float>(a, 4), z1};
                float b0[3] = {Field<float>(b, 0), Field<float>(b, 4), z0}, b1[3] = {Field<float>(b, 0), Field<float>(b, 4), z1};
                AddLine(v, a0, b0, color);
                AddLine(v, a1, b1, color);
                AddLine(v, a0, a1, color);
                AddTri(v, a0, b0, b1, color, false);  // the side, as two triangles
                AddTri(v, a0, b1, a1, color, false);
            }
            // Top and bottom: a fan from the first point (right for convex footprints).
            for (int i = 1; i + 1 < n; ++i) {
                char* p0 = ReachTags::BlockElement(vol, 0x44, 0x14, 0);
                char* p1 = ReachTags::BlockElement(vol, 0x44, 0x14, i);
                char* p2 = ReachTags::BlockElement(vol, 0x44, 0x14, i + 1);
                if (!p0 || !p1 || !p2) break;
                for (int level = 0; level < 2; ++level) {
                    float z = level ? z1 : z0;
                    float q0[3] = {Field<float>(p0, 0), Field<float>(p0, 4), z};
                    float q1[3] = {Field<float>(p1, 0), Field<float>(p1, 4), z};
                    float q2[3] = {Field<float>(p2, 0), Field<float>(p2, 4), z};
                    AddTri(v, q0, q1, q2, color, false);
                }
            }
        }
    }

    // Soft ceilings: the triangles of every structure design tag (a scnr
    // block of tag references, +0x5C). Each is matched by name to the
    // scenario's own soft ceiling entry, whose flags say what it affects
    // (+0 flags bit 0 = ignores bipeds). Those that stop the player are drawn in
    // colour by type (acceleration orange, soft kill red, slip surface
    // magenta); the rest, only when `all`, in grey-blue.
    void SoftCeilings(Games::BoundaryView& v, char* scnr, bool all) {
        int designs = ReachTags::BlockCount(scnr, 0x5C);
        int entries = ReachTags::BlockCount(scnr, REACH_SCNR_SOFT_CEILINGS);
        for (int d = 0; d < designs && d < 16; ++d) {
            char* ref = ReachTags::BlockElement(scnr, 0x5C, 0x20, d);
            char* design = ref ? ReachTags::Get(Field<uint32_t>(ref, 0xC)) : nullptr;
            if (!design) continue;
            int ceilings = ReachTags::BlockCount(design, 0x40);
            for (int c = 0; c < ceilings && c < 256; ++c) {
                char* ceiling = ReachTags::BlockElement(design, 0x40, 0x14, c);
                if (!ceiling) continue;
                uint32_t name = Field<uint32_t>(ceiling, 0);
                int type = Field<int16_t>(ceiling, 4);
                bool hitsPlayer = true;
                for (int e = 0; e < entries && e < 64; ++e) {
                    char* entry = ReachTags::BlockElement(scnr, REACH_SCNR_SOFT_CEILINGS, REACH_SCNR_SOFT_CEILING_SIZE, e);
                    if (!entry || Field<uint32_t>(entry, 4) != name) continue;
                    // +0 = the flags; +2 (runtime flags) reads 1 on the ceilings that do stop the player, so it isn't used.
                    hitsPlayer = !(Field<uint16_t>(entry, 0) & 1);
                    break;
                }
                if (!hitsPlayer && !all) continue;
                uint32_t color = !hitsPlayer ? 0xFFC0A080u : type == 1 ? 0xFF4040FFu : type == 2 ? 0xFFFF40FFu : 0xFF30A0FFu;
                int tris = ReachTags::BlockCount(ceiling, 8);
                for (int t = 0; t < tris && t < 4096; ++t) {
                    char* tri = ReachTags::BlockElement(ceiling, 8, 0x44, t);
                    if (!tri) continue;
                    Games::BoundaryView::Tri out;
                    for (int m = 0; m < 3; ++m)
                        for (int k = 0; k < 3; ++k) out.v[m][k] = Field<float>(tri, 0x20 + 12 * m + 4 * k);
                    out.color = color;
                    out.outline = false;  // the edges below are drawn as lines, which can be hidden piece by piece
                    out.dist = 0;
                    for (int m = 0; m < 3; ++m) AddLine(v, out.v[m], out.v[(m + 1) % 3], (color & 0x00FFFFFFu) | (150u << 24));
                    v.tris.push_back(out);
                }
            }
        }
    }

    uint64_t PieceKey(const float* c, int kind) {
        auto q = [](float v) { return (uint64_t)((int64_t)floorf(v * 8.0f) & 0xFFFFF); };  // 1/8 unit
        return q(c[0]) | (q(c[1]) << 20) | (q(c[2]) << 40) | ((uint64_t)kind << 60);
    }

    // The piece size wanted at a distance: small near the camera, coarse far away.
    float PieceSize(float dist) { return dist < 15 ? 2.5f : dist < 40 ? 5.0f : dist < 100 ? 12.0f : 30.0f; }

    // Replaces the view's triangles and lines with pieces (halved until small enough for their distance)
    // that the game thread has found visible. Every piece is also listed for testing.
    void HideOccluded(Games::BoundaryView& view, char* scnr, float range) {
        std::lock_guard<std::mutex> lock(g_visLock);
        ULONGLONG now = GetTickCount64();
        if (scnr != g_visScenario) { g_visScenario = scnr; g_known.clear(); }
        g_pending.clear();
        auto known = [&](uint64_t key, const float* c) {
            g_pending.push_back({key, {c[0], c[1], c[2]}});
            auto it = g_known.find(key);
            if (it == g_known.end()) return false;
            it->second.touched = now;
            return it->second.visible;
        };

        std::vector<Games::BoundaryView::Tri> in;
        in.swap(view.tris);
        std::vector<Games::BoundaryView::Tri> stack;
        for (const auto& t0 : in) {
            stack.push_back(t0);
            while (!stack.empty()) {
                Games::BoundaryView::Tri t = stack.back();
                stack.pop_back();
                float q[3];
                ClosestOnTriangle(view.pos, t.v[0], t.v[1], t.v[2], q);
                float d = sqrtf(DistSq(q, view.pos));
                if (range > 0.0f && d > range) continue;
                int e = 0;  // longest edge
                float longest = 0;
                for (int m = 0; m < 3; ++m) {
                    float l = DistSq(t.v[m], t.v[(m + 1) % 3]);
                    if (l > longest) { longest = l; e = m; }
                }
                if (sqrtf(longest) > PieceSize(d) && view.tris.size() + stack.size() < kMaxCells) {
                    float mid[3];
                    for (int k = 0; k < 3; ++k) mid[k] = (t.v[e][k] + t.v[(e + 1) % 3][k]) * 0.5f;
                    Games::BoundaryView::Tri a = t, b = t;
                    for (int k = 0; k < 3; ++k) { a.v[(e + 1) % 3][k] = mid[k]; b.v[e][k] = mid[k]; }
                    stack.push_back(a);
                    stack.push_back(b);
                    continue;
                }
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = (t.v[0][k] + t.v[1][k] + t.v[2][k]) / 3.0f;
                t.dist = d;
                if (known(PieceKey(c, 0), c)) view.tris.push_back(t);
            }
        }

        std::vector<Games::BoundaryView::Line> lines;
        lines.swap(view.lines);
        std::vector<Games::BoundaryView::Line> pieces;
        for (const auto& l0 : lines) {
            pieces.push_back(l0);
            while (!pieces.empty()) {
                Games::BoundaryView::Line l = pieces.back();
                pieces.pop_back();
                float q[3];
                ClosestOnSegment(view.pos, l.a, l.b, q);
                float d = sqrtf(DistSq(q, view.pos));
                if (range > 0.0f && d > range) continue;
                float len = sqrtf(DistSq(l.a, l.b));
                if (len > PieceSize(d) && view.lines.size() + pieces.size() < kMaxCells) {
                    float mid[3];
                    for (int k = 0; k < 3; ++k) mid[k] = (l.a[k] + l.b[k]) * 0.5f;
                    Games::BoundaryView::Line a = l, b = l;
                    for (int k = 0; k < 3; ++k) { a.b[k] = mid[k]; b.a[k] = mid[k]; }
                    pieces.push_back(a);
                    pieces.push_back(b);
                    continue;
                }
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = (l.a[k] + l.b[k]) * 0.5f;
                if (known(PieceKey(c, 1), c)) view.lines.push_back(l);
            }
        }

        // Forget answers nobody has asked about for a while.
        static ULONGLONG s_pruned = 0;
        if (now - s_pruned > 2000) {
            s_pruned = now;
            for (auto it = g_known.begin(); it != g_known.end();)
                it = now - it->second.touched > 5000 ? g_known.erase(it) : std::next(it);
        }
    }
}

bool ReachCheats::ReadObserver(void** state, float* pos, float* fwd, float* up, float* tanHalfV) {
    char* o = (char*)state[kObserverSlot];
    if (!o) return false;
    for (int k = 0; k < 3; ++k) {
        pos[k] = Field<float>(o, kObserverPosition + 4 * k);
        fwd[k] = Field<float>(o, kObserverForward + 4 * k);
        up[k] = Field<float>(o, kObserverUp + 4 * k);
    }
    float fov = Field<float>(o, kObserverFov), aspect = Field<float>(o, kObserverAspect);
    if (!(fov > 0.2f && fov < 3.0f)) return false;
    if (!(aspect > 0.5f && aspect < 4.0f)) aspect = 4.0f / 3.0f;
    *tanHalfV = tanf(fov * 0.5f) / aspect;  // the FOV is horizontal at 4:3
    float l = fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2];
    return l > 0.9f && l < 1.1f;
}

void ReachCheats::BoundaryFill(void** state, char* scnr, int killCount, const Games::Settings& s,
                               Games::BoundaryView& view) {
    view.lines.clear();
    view.tris.clear();
    view.valid = ReadObserver(state, view.pos, view.fwd, view.up, &view.tanHalfV);
    if (!view.valid || !scnr) return;

    if (s.showBoundaries) {
        int volumes = ReachTags::BlockCount(scnr, kTriggerVolumesOffset);
        char* triggers = ReachTags::BlockData(scnr, REACH_SCNR_KILL_TRIGGERS);
        for (int i = 0; triggers && i < killCount && i < 64; ++i) {
            char* trigger = triggers + kKillTriggerSize * i;
            int index = Field<int16_t>(trigger, 0);
            if (index < 0 || index >= volumes) continue;
            char* vol = ReachTags::BlockElement(scnr, kTriggerVolumesOffset, kTriggerVolumeSize, index);
            if (!vol) continue;
            bool soft = ReachTags::StringName(Field<uint32_t>(vol, 0)).find("soft") != std::string::npos;
            DrawVolume(view, vol, soft ? 0xFF40D0FF : 0xFF4040FF);  // yellow-orange for soft kill, red for kill
        }
    }

    if (s.showSoftCeilings) SoftCeilings(view, scnr, s.softCeilingsAll);
    view.opacity = s.boundaryOpacity;

    // Each triangle's distance from the camera (it fades with it), the nearest point of all of them
    // (marked in the overlay with its distance, a depth cue), and the range cut.
    view.nearest.valid = false;
    float best = 1e30f, range2 = s.boundaryRange * s.boundaryRange;
    size_t n = 0;
    for (size_t i = 0; i < view.tris.size(); ++i) {
        auto& t = view.tris[i];
        float q[3];
        ClosestOnTriangle(view.pos, t.v[0], t.v[1], t.v[2], q);
        float d2 = DistSq(q, view.pos);
        if (s.boundaryRange > 0.0f && d2 > range2) continue;
        t.dist = sqrtf(d2);
        if (d2 < best) { best = d2; memcpy(view.nearest.p, q, sizeof(q)); view.nearest.dist = t.dist; view.nearest.valid = true; }
        view.tris[n++] = t;
    }
    view.tris.resize(n);
    if (s.boundaryRange > 0.0f) {
        n = 0;
        for (size_t i = 0; i < view.lines.size(); ++i) {
            float q[3];
            ClosestOnSegment(view.pos, view.lines[i].a, view.lines[i].b, q);
            if (DistSq(q, view.pos) <= range2) view.lines[n++] = view.lines[i];
        }
        view.lines.resize(n);
    }
    view.fadeRange = s.boundaryRange > 0.0f ? s.boundaryRange : 120.0f;

    if (s.boundaryHide) {
        HideOccluded(view, scnr, s.boundaryRange);
    } else {
        std::lock_guard<std::mutex> lock(g_visLock);
        g_pending.clear();
    }
}

void ReachCheats::BoundaryOcclusion(void** state, const Games::Settings& s, uint32_t unitDatum, uint32_t vehicle) {
    if (!s.boundaryHide || (!s.showBoundaries && !s.showSoftCeilings)) return;
    HMODULE reach = GetModuleHandleW(L"haloreach.dll");
    float pos[3], fwd[3], up[3], tv;
    if (!reach || !ReadObserver(state, pos, fwd, up, &tv)) return;
    auto test = (CollisionTestFn)((char*)reach + OFFSET_REACH_COLLISION_TEST_VECTOR);

    std::lock_guard<std::mutex> lock(g_visLock);
    if (g_pending.empty()) return;
    alignas(16) static uint8_t result[REACH_COLLISION_RESULT_SIZE];
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kRaysPerTick; ++i) {
        if (g_cursor >= g_pending.size()) g_cursor = 0;
        const Sample& sample = g_pending[g_cursor++];
        float ray[3] = {sample.p[0] - pos[0], sample.p[1] - pos[1], sample.p[2] - pos[2]};
        float len = sqrtf(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
        bool visible = true;
        if (len > kRayBackOff * 2) {
            float k = (len - kRayBackOff) / len;
            for (float& r : ray) r *= k;
            memset(result, 0, 0x100);
            visible = !test(REACH_COLLISION_FLAGS_CAMERA, false, pos, ray, (int32_t)unitDatum, (int32_t)vehicle, -1, result);
        }
        g_known[sample.key] = {visible, now};
    }
}
