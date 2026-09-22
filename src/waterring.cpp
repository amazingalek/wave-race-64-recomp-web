// See include/wr64/waterring.h for what this is and why it samples where it does.

#include "wr64/waterring_sample.h"
#include "wr64/water.h"
#include "wr64/drawdistance.h"

#include <algorithm>
#include <atomic>
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

// Rebuilds the window's shape from `D_80192494`: the (du, dv) pairs in
// D_80160430 and the field offsets and vertex x/z in D_80160A98. The game calls
// it only from func_8004F9E0, when a scene starts -- so changing the window
// afterwards means calling it again or the lists still describe the old size.
extern "C" void func_8004E614(uint8_t* rdram, recomp_context* ctx);

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
// Between the two the game keeps the height. Only reported, never used to place
// anything -- it is what turns "the sea reaches 16,192 units" into "the sea
// reaches N pixels below the horizon", which is the number that decides whether
// reaching further is worth anything.
constexpr uint32_t kCameraY = 0x50;

// `D_80192494`, the radius in lattice cells of the window the game simulates
// water in. Read by the integrator (`func_8004EAA4`) and by the mesh builder
// (`func_80050204`) alike, which is what makes the drawn patch and the
// simulated window the same thing. 16 on the opening and rider select, 12 in a
// one-player race, 10 in two players -- see GAME-INTERNALS.md, *The simulated
// window is a hexagon around the camera*.
constexpr uint32_t kWindowRadius = 0x00192494;
constexpr float kCell = 64.0f;   // world units a lattice cell

// Two values `func_8004F9E0` *computes* from N rather than reading from the
// table, and so the only two that a port setting N behind its back has to
// compute for itself. Missing them is not subtle: everything that walks the
// window counts `D_8019249C` entries, so the lists get rebuilt for 817 cells
// while the walkers still stop at 469.
constexpr uint32_t kWindowCount = 0x0019249C;   // D_8019249C = 3N(N+1)+1
constexpr uint32_t kWindowUnits = 0x00192498;   // D_80192498 = N << 6

// gPlayers. Two players halves the window by themselves, and player one's
// vertex arena holds 497 Vtx against the 823 a 16-cell hexagon needs, so the
// wider window is one player only.
constexpr uint32_t kPlayers = 0x000DAB28;

// The window a one-player race runs at, and the one it is widened to. Twelve is
// the game's own; sixteen is the game's own too, from the opening screen, and is
// the largest it ships a mesh for.
constexpr int kRaceCells = 12;
constexpr int kWideCells = 16;

// `D_800D4B00`, the frame counter. It is incremented once a frame and it is
// what the game's own wave generators take their phase from
// (`func_8006E3A8`: `D_800D4B00 % period`), so a swell driven by it shares a
// clock with the cartridge's waves rather than running on one of its own.
constexpr uint32_t kFrameCounter = 0x000D4B00;

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

// How many field samples a ring vertex is averaged from.
//
// The jitter that survives inside the live window is spatial, not temporal.
// Each vertex's own height is continuous -- the field's query interpolates
// linearly inside a cell, and the vertex slides smoothly -- but the ring's
// neighbours are 112 units apart at the inner circle and 300 by the edge of the
// window, against a 64-unit lattice. So adjacent vertices land on unrelated
// parts of the surface, the mesh between them is noise rather than water, and
// that noise re-randomises as the ring slides.
//
// Point-sampling a signal finer than the sample spacing is the definition of
// aliasing, and the answer is to filter over the footprint rather than to
// sample harder. Each vertex is averaged over the quad it represents -- a box
// of one chord across and one band deep -- which is the same footprint filter
// WaterPS.hlsl applies to its ripple octaves, done in world space here because
// this is geometry rather than shading.
//
// Note what this is *not*: `WATER.md` has named "anchoring the ring's vertices
// to world lattice points" as the fix since 1.0.1. Snapped positions move in
// 64-unit steps, and the ring is paired by slot, so RT64 would interpolate
// those steps -- the same surge across the frames in between that the game's
// own water showed until it was given world-XZ pairing (PORTING.md §7). The
// ring cannot have that pairing: it is keyed to the water's matrix id and the
// ring carries its own. Filtering fixes the aliasing without touching what a
// slot means.

float env_float(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == 0) return fallback;
    const float parsed = std::strtof(value, nullptr);
    return std::isfinite(parsed) && parsed > 0.0f ? parsed : fallback;
}

// A box the width of the footprint attenuates what it cannot resolve; it does
// not remove it. Widening the filter past the footprint suppresses more of the
// residue at the cost of softening what the ring *can* carry, so the width is a
// multiplier rather than a constant: WR64_WATER_RING_FILTER=1.5 filters over one
// and a half footprints, =1 is the plain footprint, =2 is as soft as is worth
// trying. It costs nothing to widen -- the taps move, they do not multiply.
float filter_width() {
    static const float width = [] {
        const float w = env_float("WR64_WATER_RING_FILTER", 1.0f);
        return w < 0.25f ? 0.25f : (w > 4.0f ? 4.0f : w);
    }();
    return width;
}

// WR64_WATER_RING_TAPS=1 point-samples again, for the comparison.
int sample_taps() {
    static const int taps = [] {
        const char* value = std::getenv("WR64_WATER_RING_TAPS");
        if (value == nullptr || value[0] == 0) return 4;
        const long parsed = std::strtol(value, nullptr, 10);
        return parsed == 1 ? 1 : 4;
    }();
    return taps;
}

// ---- the far swell -------------------------------------------------------
//
// Past the simulated window there is no water to sample, so the sea there was
// left flat. Flat is honest and it is what the cartridge shows, but it is also
// the thing a player notices: the sea stops having weather halfway to the
// horizon.
//
// So it is synthesised instead of sampled. Three crossing sine components,
// evaluated at the vertex's **world** position -- not the camera's -- so the
// surface stands still while the ring slides through it, which is the property
// the sampled field could not have out there. Their phase comes from the game's
// own frame counter, so they keep time with the cartridge's waves.
//
// Amplitudes come from the field's own statistics rather than taste: over a
// captured Dolphin Park field the height is 3 world units at the median and 9 at
// the 90th percentile, so a little under four units of total swell is the right
// order. That is also small enough to sidestep the row-folding limit that made
// thirteen circles jitter -- a band folds over the one behind it when the
// amplitude approaches `h (k-1)/(k+1)`, about 12 units at this camera height.
//
// **Each component fades out by its own footprint.** The ring samples at
// `chord = 0.1308 r`, so a component of wavelength L is resolved while
// `chord < L/6` and aliases after; fading it out there is the same trick
// WaterPS.hlsl already uses on its ripple octaves. The shortest component is
// gone by ~2,300 units and the longest by ~7,600, past which the sea is a strip
// a few pixels tall and flat is the correct answer.
//
// WR64_WATER_RING_NO_SWELL=1 leaves the far sea flat again, for the comparison.
struct Swell {
    float amplitude;    // world units
    float wavelength;   // world units
    float dx, dz;       // unit direction of travel
    float period;       // frames for a full cycle
};

// The longest component reaches the far plane. A wave is resolved while the
// ring's chord is under half its length, and the chord at 16,192 units is 2,118,
// so a 13,000-unit swell is carried the whole way out while a 6,000-unit one has
// faded by 15,000 and the 1,800-unit chop is gone by 4,600. That spread is what
// puts weather on the sea all the way to the horizon instead of stopping it
// two-thirds of the way.
constexpr Swell kSwell[] = {
    { 2.2f, 13000.0f, 0.62f,  0.78f, 280.0f },
    { 2.0f,  6000.0f, 0.94f,  0.34f, 190.0f },
    { 1.2f,  3400.0f, -0.42f, 0.91f, 130.0f },
    { 0.8f,  1800.0f, 0.71f, -0.71f,  90.0f },
};

// How much of a component survives at a given sample spacing: full while the
// chord resolves it comfortably, nothing by the point it stops resolving it at
// all. Nyquist is a chord of half a wavelength, so dropping a component by a
// third of one is still on the safe side of it -- the earlier eighth-to-quarter
// window was conservative, and it was throwing away swell the ring could carry.
float footprint_fade(float chord, float wavelength) {
    const float full = wavelength / 6.0f;
    const float gone = wavelength / 3.0f;
    if (chord <= full) return 1.0f;
    if (chord >= gone) return 0.0f;
    const float t = (chord - full) / (gone - full);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

float far_swell(float x, float z, float frame, float chord) {
    float y = 0.0f;
    for (const Swell& s : kSwell) {
        const float fade = footprint_fade(chord, s.wavelength);
        if (fade <= 0.0f) continue;
        const float along = (x * s.dx + z * s.dz) / s.wavelength;
        const float phase = 6.2831853f * (along - frame / s.period);
        y += s.amplitude * std::sin(phase) * fade;
    }
    return y;
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

void write_word(uint8_t* rdram, uint32_t offset, uint32_t value) {
    std::memcpy(rdram + (offset & 0x007FFFFFu), &value, sizeof value);
}

// **Experiment.** How large a window the game simulates water in.
//
// `func_8004F9E0` writes this per scene as a literal: 16 cells on the opening
// and rider select, 12 in a one-player race, 10 in two players. Cells are 64
// world units, so a race simulates water 768 units around the camera and
// nothing outside it moves, carries a buoy, or has a wave in it at all.
//
// The game already runs a 16-cell window itself, on the opening screen, so that
// much is the cartridge's own configuration rather than an invented one. What
// stops it going further is not known yet: the static window-shape list, the
// mesh's vertex arena and the parameter sets all scale with it. Until that is
// worked out this only *narrows* or restores, and raising it past 16 is refused.
//
// **N does not travel alone.** `func_8004F9E0` copies a thirteen-word parameter
// set out of `D_800D4684` into `D_80192460`, choosing it per scene by the same
// branch that writes N, and the first of those words is the window's cell count.
// It then computes the cell count *again* from N into `D_8019249C`
// (`3N(N+1)+1`) and the radius into `D_80192498` (`N << 6`).
//
// So writing N alone leaves two cell counts disagreeing -- the copied 469 and
// the computed 817 -- and the water bobs and jitters, which is exactly what
// forcing 16 without the set did. The set has to move with it.
//
// That is also why only the three sizes the cartridge itself ships are allowed:
// a set exists for each, and there is no fourth to copy.
constexpr uint32_t kParamTable = 0x000D4684;   // D_800D4684, 3 sets of 13 words
constexpr uint32_t kParamLive = 0x00192460;    // D_80192460, the set in use
constexpr int kParamWords = 13;

struct WindowSetup { int cells; int set; };
constexpr WindowSetup kWindowSetups[] = {
    { 16, 0 },    // the opening and rider select
    { 12, 1 },    // a one-player race
    { 10, 2 },    // two players
};

// WR64_WATER_WINDOW=<cells> pins it to 10, 12 or 16 whatever the setting says.
// Unset leaves the choice to Draw Distance.
const WindowSetup* window_override() {
    static const WindowSetup* setup = [] () -> const WindowSetup* {
        const char* value = std::getenv("WR64_WATER_WINDOW");
        if (value == nullptr || value[0] == 0) return nullptr;
        const long parsed = std::strtol(value, nullptr, 10);
        for (const WindowSetup& s : kWindowSetups) {
            if (s.cells == parsed) return &s;
        }
        std::fprintf(stderr,
                     "[water] WR64_WATER_WINDOW=%ld ignored: the game ships a"
                     " parameter set for 10, 12 and 16 cells only\n", parsed);
        std::fflush(stderr);
        return nullptr;
    }();
    return setup;
}

const WindowSetup* setup_for(int cells) {
    for (const WindowSetup& s : kWindowSetups) {
        if (s.cells == cells) return &s;
    }
    return nullptr;
}

// Which window this frame should run, or null to leave the game's own alone.
//
// Draw Distance owns it. At Original the port promises the frame the cartridge
// would have produced, so the water simulates exactly as far as it always did;
// at Extended the same setting that pushes the scenery out pushes the waves out
// with it, which is the one a player would expect to move them.
//
// Only from a one-player race, and only to a size the game ships a mesh and a
// parameter set for. `held` remembers that the port did the widening, because
// once the value is written the game's own is no longer readable -- without it
// there is no way to tell a forced 16 from the opening screen's own.
const WindowSetup* wanted_window(const uint8_t* rdram, int current, int* held) {
    if (const WindowSetup* pinned = window_override()) return pinned;

    const bool one_player = read_word(rdram, kPlayers) == 1;
    const bool further = drawdistance::reach() != 0;

    if (*held != 0 && current != kWideCells) {
        *held = 0;           // a scene change put the game's own back
    }
    if (*held != 0) {
        if (one_player && further) return nullptr;      // already wide
        const WindowSetup* back = setup_for(*held);     // put it back
        *held = 0;
        return back;
    }
    if (one_player && further && current == kRaceCells) {
        *held = current;
        return setup_for(kWideCells);
    }
    return nullptr;
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

// Published for the display-list rewriter, which runs on the graphics thread
// and has no recomp_context to read RDRAM through. Written by publish() on the
// game thread each frame; an int being torn between the two is not a risk worth
// a lock here, and a stale value costs one frame of the wrong mesh.
std::atomic<int> g_window_cells{ 12 };

int window_cells() {
    return g_window_cells.load(std::memory_order_relaxed);
}

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
    // Applied whenever the game's own value is not the one asked for, which
    // covers both the first frame of a scene and the game putting its own back.
    //
    // Three things have to move together or the water tears: N itself, the
    // thirteen-word parameter set that goes with it (its first words are the
    // cell count and the six skirt vertices, all sized to N), and the window's
    // shape lists, which only func_8004E614 writes. The game does all three in
    // func_8004F9E0 and never revisits them, so the port does the same three.
    //
    // The mesh is the fourth, and the rewriter handles it: the triangles are
    // baked per size, so Walker::water_mesh_for hands over the game's own
    // 33-row list in place of its 13-row one. Without that this draws an N=16
    // arena through an N=12 list, which is what tore the water the first time.
    static int held_window = 0;
    if (const WindowSetup* wanted =
            wanted_window(rdram, int(read_word(rdram, kWindowRadius)), &held_window)) {
        if (int32_t(read_word(rdram, kWindowRadius)) != wanted->cells) {
            const int32_t n = wanted->cells;
            write_word(rdram, kWindowRadius, uint32_t(n));
            write_word(rdram, kWindowCount, uint32_t(3 * n * (n + 1) + 1));
            write_word(rdram, kWindowUnits, uint32_t(n << 6));
            const uint32_t from = kParamTable + uint32_t(wanted->set) * kParamWords * 4;
            for (int w = 0; w < kParamWords; ++w) {
                write_word(rdram, kParamLive + uint32_t(w) * 4,
                           read_word(rdram, from + uint32_t(w) * 4));
            }
            // The game's own rebuild, on the game's own thread, with a copied
            // context for the reasons height_at() gives.
            recomp_context rebuild = *ctx;
            rebuild.f_odd = rebuild.mips3_float_mode ? &rebuild.f1.u32l : &rebuild.f0.u32h;
            func_8004E614(rdram, &rebuild);
            std::fprintf(stderr,
                         "[water] simulated window forced to %d cells: %d units,"
                         " %u cells, shape lists rebuilt\n",
                         n, n << 6, read_word(rdram, kWindowCount));
            std::fflush(stderr);
        }
    }
    const int32_t cells = static_cast<int32_t>(read_word(rdram, kWindowRadius));
    const bool window_valid = cells >= 2 && cells <= 64;
    g_window_cells.store(window_valid ? cells : 12, std::memory_order_relaxed);
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

    // The game's frame counter drives the far swell's phase. It is an s32 that
    // only grows, and a float loses whole numbers past 2^24, so it is folded
    // into the swell's own cycle first -- the components' periods multiply out
    // to a cycle far shorter than that, and the fold is exact in integers.
    static const bool no_swell = std::getenv("WR64_WATER_RING_NO_SWELL") != nullptr;
    // 622440 is lcm(280, 190, 130, 90), the components' common period, so the
    // fold is invisible in the surface and keeps the counter well inside the
    // 2^24 where a float still holds whole numbers.
    const uint32_t frame = read_word(rdram, kFrameCounter) % 622440u;

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

    const int taps = sample_taps();
    const float width = filter_width();
    const float ratio = std::pow(outer / kInnerRadius, 1.0f / float(circles - 1));
    float radius = kInnerRadius;
    for (int c = 0; c < circles; ++c) {
        ring.radius[c] = radius;
        // One circle is one radius, so the wave damping is decided once a band
        // rather than once a vertex.
        const float waves = wave_scale(radius, full_to);
        // The synthesised swell takes over exactly as the sampled field lets
        // go, so the two never both carry the same wave. Its components are
        // weighed against this circle's sample spacing once, not per vertex.
        const float swell = no_swell ? 0.0f : 1.0f - waves;
        const float chord = 2.0f * radius * std::sin(3.14159265f / float(kSectors));
        // The quad this circle's vertices stand at the corner of: one chord
        // across, one band deep. Quarter-extents put the four taps at the
        // midpoints of its quadrants, which is a box filter over the footprint.
        const float half_t = chord * 0.25f * width;
        const float half_r = radius * (ratio - 1.0f) * 0.25f * width;
        for (int s = 0; s < kSectors; ++s) {
            const float angle = (6.2831853f * float(s)) / float(kSectors);
            const float ca = std::cos(angle), sa = std::sin(angle);
            const float x = cx + radius * ca;
            const float z = cz + radius * sa;
            Vertex& v = ring.vertices[c * kSectors + s];
            v.x = clamp_short(x);
            v.z = clamp_short(z);
            // Past the window the field is not sampled at all -- the game's own
            // query is what this loop costs, and out there it would answer with
            // a frozen snapshot anyway.
            float y = 0.0f;
            if (waves > 0.0f) {
                float height = 0.0f;
                if (taps == 1) {
                    height = height_at(rdram, ctx, x, z);
                    ++sampled;
                } else {
                    // Radial axis is (ca, sa), tangential is (-sa, ca).
                    for (int t = 0; t < 4; ++t) {
                        const float dr = (t & 1) ? half_r : -half_r;
                        const float dt = (t & 2) ? half_t : -half_t;
                        height += height_at(rdram, ctx,
                                            x + ca * dr - sa * dt,
                                            z + sa * dr + ca * dt);
                    }
                    height *= 0.25f;
                    sampled += 4;
                }
                y += height * waves;
            } else {
                ++flat;
            }
            if (swell > 0.0f) {
                y += far_swell(x, z, float(frame), chord) * swell;
            }
            // Carried in sixteenths of a world unit; the ring's own matrix
            // scales Y back down. See kHeightScale.
            v.y = clamp_short(y * float(kHeightScale));
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
                         " live window %d cells = %.0f units%s, waves full to %.0f,"
                         " camera %.0f above water\n",
                         circles, outer, sampled, flat, total / 300.0 / 1000.0, 300,
                         delta_total / 300.0, delta_max,
                         cells, float(cells) * kCell,
                         window_valid ? "" : " (REJECTED)", full_to,
                         read_float(rdram, camera + kCameraY));
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
