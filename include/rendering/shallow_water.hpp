#pragma once

// Waves coming into shallow water: the finite-depth dispersion that slows and
// shortens them, the shoaling that raises them, the refraction that turns
// their crests to lie along the beach, and the depth that makes them break.
//
// This is the C++ twin of the shallow-water part of
// assets/shaders/water_waves.glsli (evaluatePhysicalWavesShore and the sw*
// helpers); the tests check these figures against the textbook ones. Change
// one, change both.
//
// Units are the world's: yards and seconds (wave_spectrum.hpp's kGravity).
//
// How the phase is kept whole. A wave's phase is the sum of its wavenumber
// along the way it came; in water of varying depth the wavenumber varies, and
// taking it as k(h) x position at each point on its own tears the crests apart
// wherever the depth changes, more the further from the world's origin. So
// the waves keep two parts. The deep-water part keeps the deep phase
// k0 dir . x - omega t, which is whole everywhere; only its height changes
// (shoaling, breaking, the fade on the beach). The shore part runs along the
// slope of the bed, toward the beach, with a phase taken from the bed's depth
// alone (shorePhase): the phase a long wave builds up coming in over a beach
// of one reference slope. A function of the depth is whole wherever the depth
// is, and its crests are the depth's contours, so they lie along the beach;
// the waves slow and shorten as the depth goes. Where the real slope differs
// from the reference the shore waves come out longer (a gentler beach) or
// shorter (a steeper one) than they would be: their wavelength is only right
// on a beach of the reference slope. The deep part hands its height over to
// the shore part as the water shallows (refractionBlend), by Snell's law:
// sin(theta) / c is kept, so the share of the turn done is 1 - c / c0.

#include "rendering/wave_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::shallow_water {

using wave_spectrum::kGravity;

/// McCowan's breaker index: a wave breaks when its height reaches this
/// fraction of the water's depth.
inline constexpr float kBreakerGamma = 0.78f;
/// The most shoaling raises a wave (Green's law has it grow without bound as
/// the depth goes to nothing; breaking holds it long before).
inline constexpr float kMaxShoaling = 2.5f;
/// Over the last this many yards of depth the waves die away to nothing at
/// the waterline, where the swash takes over.
inline constexpr float kFadeDepth = 0.5f;
/// How sharply the height limit closes in: the larger, the closer the height
/// follows itself up to the limit before it is held.
inline constexpr float kLimitSharpness = 6.0f;
/// The significant wave height (4 sigma) of waves with sum(a^2) = 1.
inline constexpr float kSignificantHeightFactor = 2.8284271f;
/// The bed slope the shore waves' phase is built for (rise over run).
inline constexpr float kReferenceSlope = 0.15f;
/// A little depth added under the shore phase, so the shore waves' length does
/// not shrink to nothing at the waterline (where they have died away anyway).
inline constexpr float kShorePhaseDepth = 0.15f;
/// Past this k h the shore part is left out (and its phase stops growing):
/// the wave hardly feels the bed there.
inline constexpr float kShoreReachKh = 3.0f;
/// Past this k h for the longest wave the bed is not felt at all: the shader
/// takes the deep waves as they are there, at a deep wave's cost.
inline constexpr float kDeepKh = 5.0f;
/// The bed slopes under which there is no telling which way the shore lies.
inline constexpr float kFlatSlope = 0.01f;
inline constexpr float kSlopedSlope = 0.04f;
/// A water vertex with no ground under it in its tile is taken as this deep.
inline constexpr float kNoBedDepth = 50.0f;
/// The furthest above the water the ground is counted (dry land), yards.
inline constexpr float kMaxDryHeight = 10.0f;
/// The breaker index over which the waves steepen and lean shoreward, and
/// over which they are breaking.
inline constexpr float kSteepenFrom = 0.5f;
inline constexpr float kBreakingAt = 1.0f;
/// The most the steepness budget (sum of Q k a) is raised to as the waves
/// break, from wave_spectrum::kTotalSteepness. Under 1, so the surface never
/// folds.
inline constexpr float kBreakingSteepness = 0.95f;
/// The most a breaking wave's crest leans forward, as a share of its Q.
inline constexpr float kMaxLean = 0.6f;
/// A shore wave shorter than this, yards, stays in the deep part: its crests,
/// laid by the depth's contours, would come closer than the pixels on a
/// steep bank (water_waves.glsli's SW_MIN_SHORE_WAVELENGTH).
inline constexpr float kMinShoreWavelength = 1.0f;
/// The longest shoreward step the smoothed shore direction is taken over, in
/// grid cells either side.
inline constexpr int kSmoothCells = 2;

inline float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// The wavenumber of the wave of angular frequency `omega` in water `h` yards
/// deep, solving omega^2 = g k tanh(k h) exactly (Newton's method). Deep
/// water gives k = omega^2 / g, shallow water k = omega / sqrt(g h).
inline float wavenumberExact(float omega, float h) {
    const float k0 = omega * omega / kGravity;
    if (h <= 0.0f) return k0 * 1.0e4f;
    // Start from the shallow-water value where it is near, the deep one where
    // that is; Newton's method on f(k) = g k tanh(k h) - omega^2 then closes
    // in from above in either case.
    double k = std::max(static_cast<double>(k0), omega / std::sqrt(kGravity * static_cast<double>(h)));
    for (int i = 0; i < 50; ++i) {
        const double th = std::tanh(k * h);
        const double f = kGravity * k * th - static_cast<double>(omega) * omega;
        const double df = kGravity * (th + k * h * (1.0 - th * th));
        const double step = f / df;
        k -= step;
        if (std::abs(step) < 1e-12 * k) break;
    }
    return static_cast<float>(k);
}

/// The same, as the shader works it: Fenton and McKee's explicit
/// approximation (within about 1.5%), then one Newton step, which brings it
/// within a few parts in ten thousand.
inline float wavenumber(float omega, float h) {
    const float k0 = omega * omega / kGravity;
    const float hh = std::max(h, 1.0e-3f);
    const float x = k0 * hh;
    float k = k0 / std::pow(std::tanh(std::pow(x, 0.75f)), 2.0f / 3.0f);
    const float th = std::tanh(k * hh);
    const float f = kGravity * k * th - omega * omega;
    const float df = kGravity * (th + k * hh * (1.0f - th * th));
    k -= f / df;
    return k;
}

/// Phase speed and group speed at depth `h`, from the wavenumber `k` there.
inline float phaseSpeed(float omega, float k) { return omega / k; }
inline float groupSpeed(float omega, float k, float h) {
    const float kh2 = 2.0f * k * std::max(h, 0.0f);
    // n = 1/2 (1 + 2kh / sinh 2kh): 1/2 in deep water, 1 in shallow.
    const float n = kh2 > 20.0f ? 0.5f : 0.5f * (1.0f + (kh2 > 1.0e-4f ? kh2 / std::sinh(kh2) : 1.0f));
    return n * omega / k;
}

/// Shoaling: as the group slows the energy it carries bunches up and the wave
/// grows, Ks = sqrt(cg_deep / cg). 1 in deep water (a little under it in
/// between, where the group outruns its deep self), growing as the water
/// shallows, held at kMaxShoaling.
inline float shoalingCoefficient(float omega, float k, float h) {
    if (h <= 0.0f) return kMaxShoaling;
    const float cgDeep = 0.5f * kGravity / omega;
    return std::min(std::sqrt(cgDeep / groupSpeed(omega, k, h)), kMaxShoaling);
}

/// The highest wave water `h` yards deep holds (McCowan).
inline float depthLimitedHeight(float h) { return kBreakerGamma * std::max(h, 0.0f); }

/// The breaker index: a wave height over the most the depth holds. 1 and
/// over, the waves break.
inline float breakerIndex(float waveHeight, float h) {
    return waveHeight / (kBreakerGamma * std::max(h, 1.0e-3f));
}

/// What the waves' heights are scaled by so that their height never passes
/// the depth's limit: about 1 well under it, closing on limit / height over
/// it, smoothly (b / (1 + b^p)^(1/p) is the height kept, as a share of the
/// limit).
inline float limitScale(float breaker) {
    if (breaker <= 0.0f) return 1.0f;
    return 1.0f / std::pow(1.0f + std::pow(breaker, kLimitSharpness), 1.0f / kLimitSharpness);
}

/// The waves die away over the last kFadeDepth of depth, to nothing at the
/// waterline and over dry land.
inline float shallowFade(float h) { return smoothstep(0.0f, kFadeDepth, h); }

/// The surf's breaker index (water_waves.glsli's GerstnerResult.breaking,
/// which water.frag's surfFoam whitens the water by): the waves' significant
/// height as shoaled, before shallowFade, over the depth's limit. The fade is
/// how a wave that has broken dies at the waterline; counted in, it took the
/// index to nothing there faster than the depth, and a lake's waves never
/// broke at any shore.
inline float surfBreakerIndex(float shoaledSignificantHeight, float h) {
    return breakerIndex(shoaledSignificantHeight, h);
}

/// How much surf the waters' size makes (water.frag's surfFoam `sea`), from
/// the open water's significant height, yards: next to none for a still
/// pond's ripples, a thin rim for a lake's chop, all of it from a lake in a
/// gale and the sea's swell up.
inline constexpr float kSurfSeaFrom = 0.01f;
inline constexpr float kSurfSeaFull = 0.16f;
inline float surfSeaShare(float seaHeight) { return smoothstep(kSurfSeaFrom, kSurfSeaFull, seaHeight); }
/// The swash's rim - the line where the run-up turns - keeps this much of the
/// surf however still the water: it laps at every shore (surfFoam).
inline constexpr float kSurfRimFloor = 0.6f;
inline float surfRimShare(float seaHeight) { return std::max(surfSeaShare(seaHeight), kSurfRimFloor); }

// --- The swash: the waterline running up the beach and back ---
//
// The waves die away over the last kFadeDepth of depth; what is left at the
// waterline is the swash, the water's level there rising as the waves come
// in and falling as they draw back, which carries the waterline up the beach
// and down again. The level near the shore (all of it out to swashFullDepth,
// fading out over kSwashFadeOver more, the dry land under the sheet with it)
// is raised by
//   eta = rms * (kSwashGroupShare * groups + kSwashDirectShare * direct),
// each term scaled to an rms of 1 for waves that all run in:
//  - groups: the run-up follows the wave groups (surf beat). A group of
//    higher waves pushes the water further up the beach, the lull between
//    groups lets it fall back: over neighbouring waves i, i+1 of a rung's
//    spectrum (ordered by frequency) a_i a_i+1 cos(phi_i - phi_i+1), their
//    beat, at the slow difference of their frequencies (some 3 s on a lake
//    in a light wind, 8 s at sea, 20 s in a storm). The groups come in a
//    little sooner or later along the shore (up to kSwashGroupDelay seconds,
//    a noise field kSwashDelayCell yards across: wave_surface::swashDelay),
//    so a lake's whole shore does not rise and fall as one.
//  - direct: each wave's own run-up - a long wave rides up and down the
//    beach with every crest, short ones, broken well out, hardly do
//    (saturation): a_i sat(omega_i) sin(phi_i).
// phi_i is the wave's phase at the waterline as its shore part comes in
// (shorePhase at depth 0, - omega t), a_i its height times how much it runs
// toward the shore. The rms is kSwashGain of the open water's significant
// height times the share of the waves' energy that runs in (a lee shore
// laps less), between kSwashMinRms (a still lake still laps a few
// hundredths of a yard) and kSwashMaxRms (a storm on the open sea).
// water_waves.glsli's sw*Swash* are the shader's twin: change one, change
// both. Not in the original client.
inline constexpr float kSwashGain = 0.5f;
inline constexpr float kSwashMinRms = 0.025f;
inline constexpr float kSwashMaxRms = 0.3f;
/// The depth to which the level moves whole: at least this, and three rms.
inline constexpr float kSwashFullFrom = 0.5f;
inline constexpr float kSwashFadeOver = 2.0f;
/// The waves' own run-up is held down for those quicker than this, rad/s.
inline constexpr float kSwashSaturation = 1.5f;
inline constexpr float kSwashGroupShare = 0.8f;
inline constexpr float kSwashDirectShare = 0.8f;
inline constexpr float kSwashGroupDelay = 4.0f;
inline constexpr float kSwashDelayCell = 60.0f;
/// The hand-made waves' swash takes each wave's phase over the world at
/// this share of its wavenumber (their beats' patterns some 30 yards long,
/// which the liquid grid's 4-yard cells carry).
inline constexpr float kSwashSpatialSpread = 0.35f;

/// The run-up's rms, yards, for the open water's significant height and the
/// share of its energy that runs toward this shore (`exposure`, 0-1).
inline float swashRms(float seaHeight, float exposure) {
    return std::clamp(kSwashGain * seaHeight * exposure, kSwashMinRms, kSwashMaxRms);
}
inline float swashFullDepth(float rms) { return std::max(kSwashFullFrom, 3.0f * rms); }
/// How much of the swash's level a point `h` yards deep (negative: dry land
/// under the sheet) is raised by: all of it near the shore, none in deep
/// water.
inline float swashWeight(float h, float rms) {
    const float full = swashFullDepth(rms);
    return 1.0f - smoothstep(full, full + kSwashFadeOver, h);
}
/// The deepest water the swash moves at all.
inline float swashReach(float rms) { return swashFullDepth(rms) + kSwashFadeOver; }
inline float swashSaturation(float omega) {
    const float x = omega / kSwashSaturation;
    return 1.0f / (1.0f + x * x);
}
/// How much a wave runs toward the shore: `along` the dot of its heading and
/// the way to the shore (a unit vector), `sloped` (0-1) how surely the shore
/// lies that way - on a flat bed, where there is no telling, every wave half.
/// A wave running away from it still laps at it a little (kSwashLeeShare),
/// turned and bent round: a lee shore laps, less.
inline constexpr float kSwashLeeShare = 0.15f;
inline float swashToward(float along, float sloped) {
    const float t = smoothstep(-0.2f, 0.4f, along);
    return std::max(kSwashLeeShare, 0.5f + (t - 0.5f) * std::clamp(sloped, 0.0f, 1.0f));
}

/// The swash's level (yards over the flat surface, before swashWeight) and
/// how fast it is changing (yards a second).
struct SwashLevel {
    float level = 0.0f;
    float rate = 0.0f;
    float rms = kSwashMinRms;
};

/// The swash summed wave by wave (the shader's swSwashAdd and swSwashEnd):
/// add() each wave in the spectrum's order, `rung` telling the two rungs'
/// sets apart (waves are paired within one only).
struct SwashSum {
    float energy = 0.0f;  ///< sum a^2, every wave
    float inward = 0.0f;  ///< sum (a toward)^2
    float direct = 0.0f, directRate = 0.0f;
    float groups = 0.0f, groupsRate = 0.0f, groupsNorm = 0.0f;
    float lastA = 0.0f, lastSin = 0.0f, lastCos = 0.0f, lastOmega = 0.0f;
    int lastRung = -1;

    /// A wave of height `a` running `toward` (swashToward) the shore, of
    /// angular frequency `omega`, at `phase` (its shore phase at the
    /// waterline: - shorePhase(omega, 0) - omega t + its offset), in `rung`;
    /// `delay` the groups' arrival delay here, seconds.
    void add(float a, float toward, float omega, float phase, int rung, float delay) {
        const float at = a * toward;
        energy += a * a;
        inward += at * at;
        const float s = std::sin(phase);
        const float c = std::cos(phase);
        const float sat = swashSaturation(omega);
        direct += at * sat * s;
        directRate -= at * sat * omega * c;
        if (rung == lastRung) {
            // The beat psi = phi_last - phi (d psi / dt = omega - omega_last),
            // come in `delay` seconds later here.
            const float p = lastA * at;
            const float dw = omega - lastOmega;
            const float cb = lastCos * c + lastSin * s;
            const float sb = lastSin * c - lastCos * s;
            const float cs = std::cos(dw * delay);
            const float sn = std::sin(dw * delay);
            const float cpsi = cb * cs - sb * sn;
            const float spsi = sb * cs + cb * sn;
            groups += p * cpsi;
            groupsRate -= p * spsi * dw;
            groupsNorm += p * p;
        }
        lastA = at;
        lastSin = s;
        lastCos = c;
        lastOmega = omega;
        lastRung = rung;
    }
    /// The swash, for the open water's significant height `seaHeight`.
    SwashLevel end(float seaHeight) const {
        SwashLevel out;
        if (energy <= 1e-12f || inward <= 1e-12f) return out;
        out.rms = swashRms(seaHeight, std::sqrt(inward / energy));
        const float d = kSwashDirectShare / std::sqrt(0.5f * inward);
        const float g = groupsNorm > 1e-16f ? kSwashGroupShare / std::sqrt(0.5f * groupsNorm) : 0.0f;
        out.level = out.rms * (groups * g + direct * d);
        out.rate = out.rms * (groupsRate * g + directRate * d);
        return out;
    }
};

/// How much of a wave has turned to run straight up the beach (Snell:
/// sin(theta) / c held, so 1 - c / c0 = 1 - tanh(k h) of the turn is done),
/// left out past kShoreReachKh.
inline float refractionBlend(float kh) {
    return (1.0f - std::tanh(kh)) * (1.0f - smoothstep(0.66f * kShoreReachKh, kShoreReachKh, kh));
}

/// The depth past which the shore part of a wave of frequency `omega` is
/// left out (k h = kShoreReachKh by the shallow-water wavenumber).
inline float shoreReachDepth(float omega) {
    const float r = kShoreReachKh / omega;
    return kGravity * r * r;
}

/// The shore part's phase at depth `h`, before - omega t: the phase a long
/// wave (c = sqrt(g h)) builds up running in over a bed of kReferenceSlope,
/// counted from the waterline: the integral of omega / sqrt(g h) dh over the
/// slope. A function of the depth alone, so whole wherever the depth is.
inline float shorePhase(float omega, float h) {
    const float d = std::clamp(h, 0.0f, shoreReachDepth(omega)) + kShorePhaseDepth;
    return 2.0f * omega / kReferenceSlope * std::sqrt(d / kGravity);
}

/// The shore part's wavenumber along a bed of slope `slope`: the gradient of
/// shorePhase, d(phase)/dh times the slope.
inline float shoreWavenumber(float omega, float h, float slope) {
    if (h > shoreReachDepth(omega)) return 0.0f;
    return omega / std::sqrt(kGravity * (std::max(h, 0.0f) + kShorePhaseDepth)) * slope / kReferenceSlope;
}

/// The steepness the waves are given at a breaker index, from the sum of
/// their k a: Q shared by all, and the crests' forward lean (a share of the
/// height moved shoreward with the crest). Together, sum(k a) sqrt(Q^2 +
/// lean^2) stays under the budget, which rises from wave_spectrum's toward
/// kBreakingSteepness as the waves break - so the surface never folds.
struct Steepness {
    float q = 0.0f;
    float lean = 0.0f;
};
inline Steepness steepness(float kaSum, float breaker) {
    const float b = smoothstep(kSteepenFrom, kBreakingAt, breaker);
    const float budget = wave_spectrum::kTotalSteepness + (kBreakingSteepness - wave_spectrum::kTotalSteepness) * b;
    const float leanShare = kMaxLean * b;
    Steepness s;
    if (kaSum <= 0.0f) return s;
    s.q = std::min(1.0f, budget / (kaSum * std::sqrt(1.0f + leanShare * leanShare)));
    s.lean = leanShare * s.q;
    return s;
}

/// The shore field of a square grid of ground heights, `n` x `n`, `cell`
/// yards apart, row gy, column gx (index gy * n + gx), NaN where unknown: at
/// every node the ground's uphill slope, smoothed over kSmoothCells either
/// side, in yards per yard along +gx and +gy. Uphill under flat water is
/// toward the shallows.
struct Uphill {
    float gx = 0.0f;
    float gy = 0.0f;
};
inline std::vector<Uphill> uphillField(const std::vector<float>& z, int n, float cell) {
    std::vector<Uphill> raw(static_cast<size_t>(n) * n);
    std::vector<unsigned char> known(raw.size(), 0);
    auto at = [&](int x, int y) { return z[static_cast<size_t>(y) * n + x]; };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (!std::isfinite(at(x, y))) continue;
            // Central differences, one-sided at the edge or beside a gap.
            auto diff = [&](int x0, int y0, int x1, int y1) -> float {
                const bool a = x0 >= 0 && y0 >= 0 && x0 < n && y0 < n && std::isfinite(at(x0, y0));
                const bool b = x1 >= 0 && y1 >= 0 && x1 < n && y1 < n && std::isfinite(at(x1, y1));
                if (a && b) return (at(x1, y1) - at(x0, y0)) / (2.0f * cell);
                if (b) return (at(x1, y1) - at(x, y)) / cell;
                if (a) return (at(x, y) - at(x0, y0)) / cell;
                return 0.0f;
            };
            Uphill& u = raw[static_cast<size_t>(y) * n + x];
            u.gx = diff(x - 1, y, x + 1, y);
            u.gy = diff(x, y - 1, x, y + 1);
            known[static_cast<size_t>(y) * n + x] = 1;
        }
    }
    // A box average over the neighbourhood, so one bump in the bed does not
    // turn the waves about.
    std::vector<Uphill> out(raw.size());
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float sx = 0.0f, sy = 0.0f;
            int count = 0;
            for (int dy = -kSmoothCells; dy <= kSmoothCells; ++dy) {
                for (int dx = -kSmoothCells; dx <= kSmoothCells; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
                    const size_t i = static_cast<size_t>(yy) * n + xx;
                    if (!known[i]) continue;
                    sx += raw[i].gx;
                    sy += raw[i].gy;
                    ++count;
                }
            }
            if (count > 0) out[static_cast<size_t>(y) * n + x] = {sx / count, sy / count};
        }
    }
    return out;
}

/// How surely the shore lies uphill on a bed of `slope`: 0 on a flat bed,
/// where there is no telling, 1 on a sloping one. The shore direction handed
/// to the shader is the uphill unit vector times this.
inline float slopeWeight(float slope) { return smoothstep(kFlatSlope, kSlopedSlope, slope); }

}  // namespace wowee::rendering::shallow_water
