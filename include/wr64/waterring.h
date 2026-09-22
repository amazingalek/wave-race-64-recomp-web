#pragma once

// Water past the edge of the game's own patch.
//
// The game animates a 500-vertex patch that reaches **922 units** around the
// camera, and that is all the water there is: a whole-frame trace found nothing
// else within two units of sea level except the craft. What fills the screen
// from the patch's edge down to the bottom is the lowest of the sky's three
// bands -- a seven-vertex plane at y = 0, painted to look like distant sea. See
// docs/GAME-INTERNALS.md, *The water*.
//
// So there is no far surface to shade, and making the sea reach further means
// putting geometry where the game draws none. That is what this builds: a ring
// of quads from the patch's edge out to the distance the rest of the course is
// drawn at, standing on the **same wave field the game's own patch stands on**,
// so the swell outside agrees with the swell inside instead of being invented.
//
// It carries the game's own water attributes rather than inventing those either:
// tc = (1024, 1024) and rgba = FFFFFFB0, read out of a trace of the game's own
// patch. The alpha is the part that matters -- the water is drawn translucent,
// and an opaque ring reads as a solid band laid over the sea. That is what lets
// this work with the water renderer at **Original** as well as with the modern
// one: at Original the ring is simply the game's water reaching further, drawn
// with the game's own texel and translucency.
//
// **Why the heights are sampled here and not in the renderer.** The field lives
// in RDRAM and the game thread writes it. The display-list rewriter runs on the
// graphics thread, so reading the field there would be a torn read of a moving
// structure. This follows the pattern water.cpp already uses: sample on the game
// thread at task submission, publish keyed by display list, and let the rewriter
// claim it. What is sampled is only the ring's own vertices -- a few hundred
// points -- not the field's 192 KiB.
//
// The height comes from **the game's own query**, `func_8004D30C(x, z)`, rather
// than from a second implementation of the lattice arithmetic in C++. A decode
// exists and is verified (tools/decode_water_field.py), but a copy is a thing
// that can drift; the game's own answer cannot.

#include <cstdint>

// This half is included by the display-list rewriter, which must not see
// recomp.h: that header defines MEM_W and friends as macros. The sampling half,
// which needs a recomp_context to call the game's own height query, is declared
// in wr64/waterring_sample.h instead. recomp_context is a typedef of an
// anonymous struct and cannot be forward declared, so the split is the only way
// to keep the macros out of here -- wr64/waterfield.h has the same constraint.

namespace wr64::waterring {

// The ring's shape. Forty-eight sectors is 7.5 degrees each, about 110 units
// between neighbours at the inner radius -- still coarser than the game's own
// 32-unit grid, but close enough at the seam that the two read as one surface.
// Twenty-four was tried first and the join showed.
constexpr int kSectors = 48;

// Radii, including the inner edge: bands of quads between circles, spaced so
// they grow with distance, because a band at 5,000 units is a few pixels tall
// and the cost belongs further in. The outermost fades out, so the ring meets
// the sky's painted sea rather than ending at it.
//
// This is the **most** circles a ring can have, and sizes the arrays below.
// Nine was the original, which over the far plane's 16,192 units put the outer
// bands 3,400 and 5,000 units deep; thirteen keeps the same geometric spacing
// but lands more of the circles where the sea is actually drawn. The count a
// frame really uses is `Ring::circles`, so it can be turned back down without
// rebuilding -- see `WR64_WATER_RING_BANDS`.
constexpr int kCircles = 13;

constexpr int kVertexCount = kSectors * kCircles;

// How much finer than a world unit the ring's heights are carried.
//
// A Vtx holds s16 coordinates, so a vertex standing in world space has its
// height rounded to a whole unit. The game's own patch can live with that: its
// vertices sit *at* lattice points, so they do not resample as the camera moves
// and the rounding is a fixed error. The ring's vertices slide continuously,
// which turns the same rounding into a **1-unit staircase walked at 20 Hz** --
// and against a far swell whose whole amplitude is about four units, that is a
// quarter of the wave. It is the jitter that survived both the distance taper
// and a footprint filter, neither of which can help: filtering lowers the
// signal's variance and leaves the step size exactly where it was.
//
// So the heights are carried pre-multiplied by this, and the ring's own model
// matrix -- which it already had, and which was an identity -- scales Y back
// down by the same factor. Sixteen gives a sixteenth of a world unit, and the
// worst height the field can hold (+/-128 units, plus the course's water level)
// stays far inside an s16.
constexpr int kHeightScale = 16;

// One vertex, exactly as a Vtx holds it: x and z in world units, y in
// sixteenths of one (see kHeightScale). The game's own water is drawn under an
// identity model matrix with world-space vertices and this is drawn under a
// matrix that differs from identity only in Y, so the two stand in the same
// world -- and 16,000 units fits an int16.
struct Vertex {
    int16_t x, y, z;
};

struct Ring {
    Vertex vertices[kVertexCount];
    // The inner radius the ring starts at, so the rewriter can say in a log what
    // it drew without recomputing it.
    float inner;
    float outer;
    // The centre the circles are drawn around (the camera) and each circle's
    // radius, so the rewriter can move the inner edge onto the patch's own
    // outline without guessing where the other circles are.
    float cx, cz;
    float radius[kCircles];
    // How many of those circles this ring actually uses, 2 to kCircles. The
    // emitter draws `circles - 1` bands and fades the last one, so a ring is
    // described entirely by this and the radii above; nothing downstream reads
    // kCircles to decide what was laid out.
    int circles;
};

// Claimed by the display-list rewriter on the graphics thread, by the same list
// identity water::begin_frame uses. Null when there is nothing to draw.
const Ring* claim(uint32_t display_list);

// WR64_NO_WATER_RING=1 switches it off, as CONTRIBUTING.md asks of anything that
// could make a machine worse.
bool enabled();

}  // namespace wr64::waterring
