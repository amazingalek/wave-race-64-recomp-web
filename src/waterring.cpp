// See include/wr64/waterring.h for what this is and why it samples where it does.

#include "wr64/waterring_sample.h"
#include "wr64/water.h"
#include "wr64/drawdistance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

// The game's water height at a world XZ. Recompiled; declaring it is all it
// takes to call it.
extern "C" void func_8004D30C(uint8_t* rdram, recomp_context* ctx);

namespace wr64::waterring {
namespace {

// Where the ring stops: the same integer the course's own scenery cull reads, so
// the sea reaches as far as the things standing in it.
//
// The field is read through the game's own pointer, so it is the copy of the
// view drawn last; the Draw Distance setting is taken into account below.
constexpr uint32_t kCoursePointer = 0x001C0C80;
constexpr uint32_t kCullField = 0xA4;

// gCameraPerspective, and the index of the one in use -- the addresses the
// render-distance census reads.
constexpr uint32_t kCameraIndex = 0x00223930;
constexpr uint32_t kCameraBase = 0x00227C80;
constexpr uint32_t kCameraStride = 0x10C;
constexpr uint32_t kCameraX = 0x4C;
constexpr uint32_t kCameraZ = 0x54;

// `D_80192494`, the radius in lattice cells of the window the game simulates
// water in. Read by the integrator (`func_8004EAA4`) and by the mesh builder
// (`func_80050204`) alike, which is what makes the drawn patch and the
// simulated window the same thing. 16 on the opening and rider select, 12 in a
// one-player race, 10 in two players -- see GAME-INTERNALS.md, *The simulated
// window is a hexagon around the camera*.
constexpr uint32_t kWindowRadius = 0x00192494;
constexpr float kCell = 64.0f;   // world units a lattice cell

// The game's own drawn reach, from GAME-INTERNALS.md. The ring must carry full
// amplitude at least this far or it meets the patch's live waves with damped
// ones, which is a seam of its own.
constexpr float kSeamRadius = 922.0f;

// How far past the live window the field is still worth sampling.
//
// The window travels with the camera, so cells just outside it were being
// stepped moments ago and still hold a plausible surface; cells far outside hold
// whatever a generator line or a passing craft last stamped, and nothing has
// touched them since. There is no sharp edge between the two, so this is a
// span, not a boundary -- and it is the one number here chosen by eye rather
// than measured. WR64_WATER_RING_WAVE_FULL and _FLAT override both ends.
constexpr float kStaleSpan = 1400.0f;

// The reach of the game's own patch, from GAME-INTERNALS.md. This is only where
// the circles are laid out from: the rewriter moves the inner edge onto the
// patch's own outline in each direction the patch covers, because the ring is
// drawn translucent *after* the patch, and any overlap shows as a second layer
// of water with a hard edge (issue #19). See Walker::fit_ring_to_patch.
constexpr float kInnerRadius = 860.0f;

// Never less than this much ring, and never more. The lower bound keeps a course
// with a small cull from producing a degenerate ring; the upper is the far
// plane, past which nothing is drawn at all.
constexpr float kMinOuter = 2000.0f;
constexpr float kMaxOuter = 16192.0f;

// How far out the ring still carries waves.
//
// The first version of this damped by distance on the theory that the ring
// samples the field too coarsely to carry waves -- 48 sectors is 112 units
// between neighbours at 860 and 1,466 at 11,213, against a 64-unit lattice. That
// is true, and it is not the main thing.
//
// **The main thing is that there is no water out there to sample.** The game
// simulates only the hexagon of `D_80192494` cells around the camera -- 768
// units in a one-player race -- and nothing outside it is ever stepped. What the
// field holds further out is a generator line, a wake some craft left behind, or
// zero, none of which has moved since it was written. Measured on a captured
// field: 84% of entries have a height, 27.7% have ever been integrated.
//
// So a ring vertex past the window is not reading a distant swell. It is reading
// a frozen snapshot, and because the vertex slides with the camera, what the
// player sees is the ring's own sampling pattern crawling over dead data. No
// amount of tessellation or interpolation improves that; the geometry has to
// stop carrying detail it is not entitled to.
//
// Hence: full amplitude while the data is live, tapering to flat across the span
// where it is merely recent, and flat where it is dead. Flat far sea is also the
// cartridge's own picture -- what fills the screen past the patch is the sky's
// haze band, a plane at y = 0.
//
// A second problem solves itself here. The array aliases at 14,190 units (three
// world cells share every entry) while the ring reaches 16,192, so its outer
// circles were reading the water under the boat. Once sampling stops at the
// taper's end, around 2,300 units, nothing reaches the alias at all.
//
// WR64_WATER_RING_WAVES_FAR=1 samples the field undamped again, for the
// comparison. WR64_WATER_RING_WAVE_FULL and _FLAT override both radii.

// Where the field stops being live, for a window of `cells` radius. Full
// amplitude covers the game's own drawn reach as well, so the ring never meets
// the patch's live waves with damped ones.
float waves_full_to(int cells) {
    return std::max(kSeamRadius, float(cells) * kCell);
}

// How many circles a ring lays out, 2 to kCircles.
//
// WR64_WATER_RING_BANDS lowers it without a rebuild, which is the A/B for the
// band count: 9 is what the ring had before, and the spacing is geometric from
// the inner radius to `outer`, so fewer circles means deeper bands over the same
// reach rather than a shorter ring.
int circle_count() {
    static const int count = [] {
        const char* value = std::getenv("WR64_WATER_RING_BANDS");
        if (value == nullptr || value[0] == 0) return kCircles;
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed < 2) return 2;
        if (parsed > kCircles) return kCircles;
        return int(parsed);
    }();
    return count;
}

float env_float(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == 0) return fallback;
    const float parsed = std::strtof(value, nullptr);
    return std::isfinite(parsed) && parsed > 0.0f ? parsed : fallback;
}

// 1 where the field is live, 0 where it is dead, smoothstepped across the span
// between so neither join shows as a crease. `full` comes from the window the
// game is actually simulating this frame, not from a constant.
float wave_scale(float radius, float full) {
    static const bool undamped = std::getenv("WR64_WATER_RING_WAVES_FAR") != nullptr;
    if (undamped) return 1.0f;
    // Read once. This is called once a circle, so reading the environment here
    // rather than in a static cost 26 getenv() calls a frame -- which the cost
    // counter caught as the sampling loop getting *dearer* while sampling 144
    // fewer vertices.
    static const float full_override = env_float("WR64_WATER_RING_WAVE_FULL", 0.0f);
    static const float flat_override = env_float("WR64_WATER_RING_WAVE_FLAT", 0.0f);
    if (full_override > 0.0f) full = full_override;
    const float flat = flat_override > 0.0f ? flat_override : full + kStaleSpan;
    if (radius <= full) return 1.0f;
    if (radius >= flat || flat <= full) return 0.0f;
    const float t = (radius - full) / (flat - full);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

std::mutex g_lock;
struct Pending {
    uint32_t list;
    Ring ring;
};
std::deque<Pending> g_pending;

uint32_t read_word(const uint8_t* rdram, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, rdram + (offset & 0x007FFFFFu), sizeof value);
    return value;
}

float read_float(const uint8_t* rdram, uint32_t offset) {
    float value;
    std::memcpy(&value, rdram + (offset & 0x007FFFFFu), sizeof value);
    return value;
}

// The game's own answer for the height at (x, z).
//
// The context is *copied* rather than borrowed, and f_odd repointed after the
// copy -- both for the reasons src/waterfield.cpp sets out at length: a borrowed
// context would let the callee write into the live frame, and f_odd points into
// the context it belongs to, so a struct copy leaves it aimed at the original.
float height_at(uint8_t* rdram, const recomp_context* ctx, float x, float z) {
    recomp_context probe = *ctx;
    probe.f_odd = probe.mips3_float_mode ? &probe.f1.u32l : &probe.f0.u32h;
    probe.f12.fl = x;
    probe.f14.fl = z;
    func_8004D30C(rdram, &probe);
    return probe.f0.fl;
}

int16_t clamp_short(float value) {
    if (!std::isfinite(value)) return 0;
    if (value > 32000.0f) return 32000;
    if (value < -32000.0f) return -32000;
    return static_cast<int16_t>(value);
}

}  // namespace

bool enabled() {
    static const bool off = [] {
        const char* value = std::getenv("WR64_NO_WATER_RING");
        return value != nullptr && value[0] != 0 && std::strcmp(value, "0") != 0;
    }();
    return !off;
}

void publish(uint8_t* rdram, recomp_context* ctx, uint32_t display_list) {
    if (!enabled()) return;

    // Drawn when *either* setting has been moved off Original, and not before.
    //
    // With the modern renderer on, the ring is the sea that renderer would
    // otherwise stop shading at 922 units. With the renderer at Original it is
    // still worth having -- the game's own water simply reaches further, drawn
    // with the game's own translucency and texel -- but only once the player has
    // asked for a longer view, because with both settings at Original the port
    // promises a frame the game itself would have produced, and geometry the
    // cartridge never submitted would break that promise.
    const bool modern = water::quality() != water::Quality::Original;
    const bool further = drawdistance::reach() != 0;
    if (!modern && !further) return;

    const uint32_t course = read_word(rdram, kCoursePointer) & 0x00FFFFFFu;
    if (course == 0 || course + kCullField + 4 > 0x00800000u) return;
    const int32_t cull = static_cast<int32_t>(read_word(rdram, course + kCullField));

    // How far the game is simulating water this frame. A value outside the range
    // the parameter block can hold means the field is not set up yet -- before a
    // race, between courses -- and the ring falls back to the game's own drawn
    // reach rather than trusting it.
    const int32_t cells = static_cast<int32_t>(read_word(rdram, kWindowRadius));
    const bool window_valid = cells >= 2 && cells <= 64;
    const float full_to = window_valid ? waves_full_to(cells) : kSeamRadius;

    const uint32_t index = read_word(rdram, kCameraIndex) & 3;
    const uint32_t camera = kCameraBase + index * kCameraStride;
    const float cx = read_float(rdram, camera + kCameraX);
    const float cz = read_float(rdram, camera + kCameraZ);
    if (!std::isfinite(cx) || !std::isfinite(cz)) return;

    // As far as the course is drawn, or as far as the player asked for,
    // whichever is further. Reading the field alone is not enough: at Original
    // the setting writes nothing, and a raised value is only written once a
    // course has settled (see drawdistance.cpp).
    float outer = static_cast<float>(std::max(cull, drawdistance::reach()));
    outer = std::clamp(outer, kMinOuter, kMaxOuter);
    if (outer <= kInnerRadius * 1.2f) return;

    Ring ring{};
    ring.inner = kInnerRadius;
    ring.outer = outer;
    ring.cx = cx;
    ring.cz = cz;

    // Radii grow geometrically, so each band covers more ground than the one
    // inside it for the same number of vertices. That is where the cost belongs:
    // a band at 5,000 units is a few pixels tall on the screen.
    const int circles = circle_count();
    ring.circles = circles;
    // Circles the ring does not use are put past the far plane rather than left
    // at zero, so anything that walks `radius` looking for the first one outside
    // a distance stops at the end of the ring instead of inside it.
    for (int c = circles; c < kCircles; ++c) ring.radius[c] = kMaxOuter * 2.0f;

    // WR64_WATER_RING_COST=1 times the loop below and nothing else -- the work,
    // not the harness. The game's own height query is what this loop costs; the
    // trigonometry beside it is noise by comparison, which is why the damped
    // bands, which skip the query, are counted separately.
    // How far the ring's centre moves between publishes.
    //
    // This is the number every stability argument about the ring rests on, and
    // the port's own documents disagree about it by about sevenfold: one says
    // the camera "moves a few units a frame", another measures the water
    // lattice stepping in multiples of 32 -- 64, 96, 128 -- in 74% of frames.
    // A slot's height changes by roughly this distance times the field's slope,
    // so it decides how much amplitude the ring can carry before it reads as
    // shimmer. Measured here rather than assumed.
    static const bool cost = std::getenv("WR64_WATER_RING_COST") != nullptr;
    static float prev_cx = 0.0f, prev_cz = 0.0f;
    static bool have_prev = false;
    static double delta_total = 0.0;
    static float delta_max = 0.0f;
    float delta = 0.0f;
    if (have_prev) {
        delta = std::sqrt((cx - prev_cx) * (cx - prev_cx) + (cz - prev_cz) * (cz - prev_cz));
        // A course or view change teleports the camera; that is not a frame's
        // motion. Racing measures 48-57 units a frame with peaks at 63-65, so
        // anything past a few hundred is a jump, not a lap.
        if (delta < 300.0f) {
            delta_total += delta;
            delta_max = std::max(delta_max, delta);
        }
    }
    prev_cx = cx;
    prev_cz = cz;
    have_prev = true;

    const auto started = cost ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
    int sampled = 0, flat = 0;

    const float ratio = std::pow(outer / kInnerRadius, 1.0f / float(circles - 1));
    float radius = kInnerRadius;
    for (int c = 0; c < circles; ++c) {
        ring.radius[c] = radius;
        // One circle is one radius, so the wave damping is decided once a band
        // rather than once a vertex.
        const float waves = wave_scale(radius, full_to);
        for (int s = 0; s < kSectors; ++s) {
            const float angle = (6.2831853f * float(s)) / float(kSectors);
            const float x = cx + radius * std::cos(angle);
            const float z = cz + radius * std::sin(angle);
            Vertex& v = ring.vertices[c * kSectors + s];
            v.x = clamp_short(x);
            v.z = clamp_short(z);
            // Past the damping the field is not sampled at all: the height is
            // water level, and the game's own query is the expensive part of
            // this loop.
            if (waves > 0.0f) {
                v.y = clamp_short(height_at(rdram, ctx, x, z) * waves);
                ++sampled;
            } else {
                v.y = 0;
                ++flat;
            }
        }
        radius *= ratio;
    }

    if (cost) {
        const double micros = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - started).count();
        static double total = 0;
        static int frames = 0;
        total += micros;
        if (++frames % 300 == 0) {
            std::fprintf(stderr,
                         "[water] ring cost: %d circles to %.0f units, %d sampled +"
                         " %d flat vertices, %.3f ms a ring (mean of %d);"
                         " camera %.1f units a frame (mean), %.1f peak;"
                         " live window %d cells = %.0f units%s, waves full to %.0f\n",
                         circles, outer, sampled, flat, total / 300.0 / 1000.0, 300,
                         delta_total / 300.0, delta_max,
                         cells, float(cells) * kCell,
                         window_valid ? "" : " (REJECTED)", full_to);
            std::fflush(stderr);
            total = 0;
            delta_total = 0.0;
            delta_max = 0.0f;
        }
    }

    std::lock_guard<std::mutex> guard(g_lock);
    g_pending.push_back({ display_list & 0x7FFFFFu, ring });
    // The same bound water.cpp keeps: a snapshot nobody claimed is a frame the
    // renderer never drew, and holding more than a handful means holding stale
    // ones.
    while (g_pending.size() > 16) g_pending.pop_front();
}

const Ring* claim(uint32_t display_list) {
    if (!enabled()) return nullptr;
    static Ring held;
    const uint32_t key = display_list & 0x7FFFFFu;
    std::lock_guard<std::mutex> guard(g_lock);
    const auto match = std::find_if(g_pending.begin(), g_pending.end(),
                                    [&](const Pending& p) { return p.list == key; });
    if (match == g_pending.end()) return nullptr;
    held = match->ring;
    g_pending.erase(g_pending.begin(), match + 1);
    return &held;
}

}  // namespace wr64::waterring
