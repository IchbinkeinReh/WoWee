#pragma once

/// How the 3.3.5a client makes its weather, as data and arithmetic
/// (MapWeather.cpp, 0x00783a40..0x0078d6b0).
///
/// SMSG_WEATHER names a Weather.dbc row (0x00520000's handler, case 0x2f4);
/// the row's EffectType picks rain (1), snow (2) or sand (3), its colour tints
/// the effect and its texture replaces the default one (0x007846a0). Each
/// effect lives in a box around the camera - rain 130 x 130 x 75 yards, snow
/// 90 x 90 x 60, sand 40 x 40 x 25 (0x0078d170) - and spawns particles at a
/// rate set by the intensity and the weatherDensity setting. A particle is
/// given a start and an end time when it is made: it falls from the top of the
/// box until it reaches the ground it was traced against, and the vertex
/// program places it from those two times alone (rain, patter, snowpoint and
/// sand in Shaders\Vertex). The ground is a cache of heights on a 1/0.96 yard
/// grid around the camera (0x00784ab0), filled as the traces ask.
///
/// Positions here are the client's own (x north, y west, z up). Nothing here
/// touches a device, so it can be checked on its own.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>

namespace wowee::rendering::client_weather {

enum class Effect : uint8_t { None = 0, Rain = 1, Snow = 2, Sand = 3 };

/// The weatherDensity setting (0-3, "2" by default) as the multiplier it
/// stands for (0x00784040).
constexpr int kDefaultDensityLevel = 2;
inline float densityScale(int level) {
    switch (std::clamp(level, 0, 3)) {
        case 0: return 0.1f;
        case 1: return 0.33f;
        case 2: return 0.66f;
        default: return 1.0f;
    }
}

/// The effects run on what the intensity has past a quarter, stretched back
/// to 0..1 (0x00784850).
inline float effectStrength(float intensity) {
    return std::max(0.0f, (intensity - 0.25f) * 1.3333334f);
}

/// The intensity on its way from `from` to `to`, `elapsed` seconds after it
/// set out: ten seconds for the whole range (0x00784850).
inline float easedIntensity(float from, float to, float elapsed) {
    const float t = std::clamp(elapsed / (std::abs(to - from + 0.001f) * 10.0f), 0.0f, 1.0f);
    return from + (to - from) * t;
}

/// The box each effect fills, centred on the camera (0x0078d170 builds them
/// with these; 0x0078ca90 and its siblings halve them).
inline glm::vec3 boxSize(Effect e) {
    switch (e) {
        case Effect::Rain: return {130.0f, 130.0f, 75.0f};
        case Effect::Snow: return {90.0f, 90.0f, 60.0f};
        case Effect::Sand: return {40.0f, 40.0f, 25.0f};
        default: return glm::vec3(0.0f);
    }
}

/// Particles made a second at full strength and density, on the vertex
/// program path useWeatherShaders takes by default (0x007840b0, 0x007844f0,
/// 0x00784580).
inline float fullRate(Effect e) {
    switch (e) {
        case Effect::Rain: return 35000.0f;
        case Effect::Snow: return 14000.0f;
        case Effect::Sand: return 32000.0f;
        default: return 0.0f;
    }
}
inline float spawnRate(Effect e, float density, float strength) {
    return density * fullRate(e) * strength;
}

/// Particles go out in packets of 0x1800, one vertex buffer each
/// (Packet<Drop,0x1800>, 0x00786920). A packet is closed when full or when it
/// has been filling for the full rate's 6144th of a second (+0xb4).
constexpr uint32_t kPacketSize = 0x1800;
inline float packetWindow(Effect e) { return fullRate(e) * 0.00016276042f; }

/// How many to make this frame (0x00787ce0): the rate over the frame, the
/// frame taken as no longer than a sixtieth; nothing unless that is more than
/// one, and then that many rounded.
inline int spawnCount(float dt, float rate) {
    const float n = std::min(dt, 0.016667f) * rate;
    if (!(n > 1.0f)) return 0;
    return static_cast<int>(std::lround(n - 0.5f));
}

/// A particle: where it is at `start`, how it moves, and when it stops. A
/// dead one has start == end (rain, sand) or ends a quarter second before it
/// starts (snow) and is never drawn.
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float start = 0.0f;
    float end = 0.0f;
};

/// A rain drop's splash: where and when it lands, shown for a quarter second
/// (0x00785640).
struct Splash {
    glm::vec3 position{0.0f};
    float start = 0.0f;
    float end = 0.0f;
};
constexpr float kSplashLife = 0.25f;

/// The client's sine and cosine (0x006f7a60): a cubic, not the library's.
inline float clientCosPi(float u) {
    const float whole = std::floor(u);
    const float f = u - whole;
    const float c = 1.0f - (6.0f - f * 4.0f) * f * f;
    return (static_cast<long long>(whole) & 1) ? -c : c;
}

/// A 3x3 the way the client lays one out and applies it (0x004c2210):
/// out.x = m[0]x + m[1]y + m[2]z, and so on down.
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    glm::vec3 apply(const glm::vec3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
};

/// The rotation by `angle` about a unit axis as 0x004c3460 writes it.
inline Mat3 axisRotation(float angle, glm::vec3 a) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1.0f - c;
    Mat3 r;
    r.m[0] = a.x * a.x * k + c;
    r.m[1] = a.y * a.x * k + s * a.z;
    r.m[2] = a.z * a.x * k - a.y * s;
    r.m[3] = a.y * a.x * k - s * a.z;
    r.m[4] = a.y * a.y * k + c;
    r.m[5] = a.z * a.y * k + a.x * s;
    r.m[6] = a.z * a.x * k + a.y * s;
    r.m[7] = a.z * a.y * k - a.x * s;
    r.m[8] = a.z * a.z * k + c;
    return r;
}

/// The turn about z by `angle` as 0x004c3290 writes it.
inline Mat3 yawRotation(float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    Mat3 r;
    r.m[0] = c;  r.m[1] = s;
    r.m[3] = -s; r.m[4] = c;
    return r;
}

/// The player's movement as the effects use it (0x0078d170): the direction
/// the player is going, or facing when going nowhere, and how fast.
struct Wind {
    float angle = 0.0f;  ///< radians, the client's own (0 = +x, toward +y)
    float speed = 0.0f;  ///< yards a second
};

/// Rain and snow lean their box over by the player's speed, up to 65 degrees
/// at 18 yards a second, about the line across the way the player is going
/// (0x00785140, 0x00785880).
inline Mat3 leanForWind(const Wind& wind) {
    constexpr float kInvPi = 0.31830987f;
    const float u = (wind.angle - 1.5707964f) * kInvPi;
    const float sinA = clientCosPi(u - 0.5f);
    const float cosA = clientCosPi(u);
    const float lean = std::clamp(wind.speed * 0.055555556f, 0.0f, 1.0f) * 1.134464f;
    return axisRotation(lean, glm::normalize(glm::vec3(cosA, sinA, 0.0f)));
}

/// The ground under the weather (0x00784ab0): heights on a grid of
/// 1/0.96-yard cells, filled as they are asked for, over the 5 x 5 blocks of
/// 33.33 yards around the camera's. A cell's height is the first surface
/// within 200 yards above or below the height it was first asked at - the
/// terrain, the liquid on it or anything standing there (0x007ade10) - or 200
/// yards under it with none.
class HeightCache {
public:
    /// Highest surface at (x, y) between z + 200 and z - 200, or z - 200.
    using Query = std::function<float(float x, float y, float z)>;

    static constexpr int kBlocks = 5;
    static constexpr int kCellsPerBlock = 32;
    static constexpr int kCells = kBlocks * kCellsPerBlock;
    static constexpr float kBlockSize = 33.333332f;
    static constexpr float kCellSize = 1.0416666f;

    void setQuery(Query q) { query_ = std::move(q); }

    /// Moves the window to the camera's block (0x00784f20). Cells still in it
    /// keep their heights.
    void recenter(const glm::vec2& camera) {
        const int bx = static_cast<int>(std::floor(camera.x * 0.03f));
        const int by = static_cast<int>(std::floor(camera.y * 0.03f));
        if (valid_ && bx == centreX_ && by == centreY_) return;
        valid_ = true;
        centreX_ = bx;
        centreY_ = by;
        for (auto it = cells_.begin(); it != cells_.end();) {
            if (!inWindow(cellX(it->first), cellY(it->first))) it = cells_.erase(it);
            else ++it;
        }
    }

    void clear() { cells_.clear(); valid_ = false; }

    /// The height of the cell a point is in, asked at the point itself
    /// (0x00784be0, 0x00783c10); the largest float outside the window.
    float heightAt(const glm::vec3& p) {
        const int cx = static_cast<int>(std::floor(p.x * 0.96f));
        const int cy = static_cast<int>(std::floor(p.y * 0.96f));
        if (!inWindow(cx, cy)) return std::numeric_limits<float>::max();
        return cell(cx, cy, p);
    }

    /// Walks the cells from `from` to `to` (0x00784c60) and stops at the first
    /// whose surface is above the line there: `hit` is that point on the
    /// surface. False with no surface crossed (`hit` is `to`) or on leaving
    /// the window (`hit` is `from`).
    bool trace(const glm::vec3& from, const glm::vec3& to, glm::vec3& hit) {
        int x = static_cast<int>(std::floor(from.x * 0.96f));
        int y = static_cast<int>(std::floor(from.y * 0.96f));
        const int x1 = static_cast<int>(std::floor(to.x * 0.96f));
        const int y1 = static_cast<int>(std::floor(to.y * 0.96f));
        const int dx = std::abs(x1 - x), dy = std::abs(y1 - y);
        const int sx = to.x < from.x ? -1 : 1, sy = to.y < from.y ? -1 : 1;
        const int steps = std::max(1, std::max(dx, dy));
        const float inv = 1.0f / static_cast<float>(steps);
        int err = dx - dy;
        hit = from;
        for (int i = 0; i <= steps; ++i) {
            if (!inWindow(x, y)) { hit = from; return false; }
            const glm::vec3 p = from + (to - from) * (static_cast<float>(i) * inv);
            const glm::vec3 centre((static_cast<float>(x) + 0.5f) * kCellSize,
                                   (static_cast<float>(y) + 0.5f) * kCellSize, p.z);
            const float h = cell(x, y, centre);
            if (h > p.z) { hit = {p.x, p.y, h}; return true; }
            const int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 < dx) { err += dx; y += sy; }
        }
        hit = to;
        return false;
    }

    size_t cachedCells() const { return cells_.size(); }

private:
    static int64_t key(int x, int y) {
        return (static_cast<int64_t>(x) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(y));
    }
    static int cellX(int64_t k) { return static_cast<int>(k >> 32); }
    static int cellY(int64_t k) { return static_cast<int>(static_cast<uint32_t>(k & 0xffffffff)); }

    bool inWindow(int cx, int cy) const {
        if (!valid_) return false;
        const int ox = (centreX_ - 2) * kCellsPerBlock, oy = (centreY_ - 2) * kCellsPerBlock;
        return cx >= ox && cy >= oy && cx < ox + kCells && cy < oy + kCells;
    }

    float cell(int cx, int cy, const glm::vec3& askAt) {
        const int64_t k = key(cx, cy);
        auto it = cells_.find(k);
        if (it != cells_.end()) return it->second;
        const float h = query_ ? query_(askAt.x, askAt.y, askAt.z) : askAt.z - 200.0f;
        cells_.emplace(k, h);
        return h;
    }

    Query query_;
    std::unordered_map<int64_t, float> cells_;
    int centreX_ = 0, centreY_ = 0;
    bool valid_ = false;
};

/// What a spawn needs from the frame.
struct SpawnContext {
    glm::vec3 camera{0.0f};          ///< the box's centre
    glm::vec3 playerVelocity{0.0f};  ///< smoothed (0x0078c500)
    Wind wind;
    float strength = 0.0f;           ///< effectStrength of the intensity
    /// On a taxi: drops end at the box's floor, unsplashed, without a trace.
    bool riding = false;
};

/// The client's random number in [0, 1).
using Random = std::function<float()>;

/// Rain and snow share everything after their velocity (0x00785140,
/// 0x00785880): the drop starts at the top of the box, which is leaned with
/// the wind and led by the player's movement, and ends where its fall meets
/// the ground - three half-boxes down at most, or two when riding.
inline Particle fall(const glm::vec3& local, const glm::vec3& velocity, const glm::vec3& box,
                     const SpawnContext& ctx, HeightCache& ground, float start, float deadEnd) {
    Particle p;
    p.velocity = velocity;
    const float half = -((box.z * 0.5f) / velocity.z);
    glm::vec3 pos = leanForWind(ctx.wind).apply(local - velocity * half);
    pos += ctx.playerVelocity * 1.75f + ctx.camera;
    p.position = pos;
    if (!(ground.heightAt(pos) < pos.z)) { p.start = 0.0f; p.end = deadEnd; return p; }
    const glm::vec3 floor = pos + velocity * half;
    const glm::vec3 end = floor + velocity * half * (ctx.riding ? 1.0f : 2.0f);
    glm::vec3 hit = end;
    if (!ctx.riding && !ground.trace(pos, end, hit)) { p.start = 0.0f; p.end = deadEnd; return p; }
    const float inv = 1.0f / glm::length(velocity);
    const float t = glm::length(pos - hit) * inv;
    p.position = hit - velocity * t;
    p.start = start;
    p.end = start + glm::length(p.position - hit) * inv;
    return p;
}

/// A rain drop (0x00785140).
inline Particle spawnRain(const Random& rnd, const SpawnContext& ctx, HeightCache& ground, float start) {
    const glm::vec3 box = boxSize(Effect::Rain);
    const float ry = rnd(), rx = rnd();
    const glm::vec3 local((rx - 0.5f) * box.x, (ry - 0.5f) * box.y, 0.0f);
    const float s = ctx.strength;
    const float angle = (s * 0.20943952f + 0.05235988f) * (rnd() - 0.5f) - 1.57f;
    const float speed = s * 9.49f + 0.01f + 2.0f * (rnd() - 0.5f) * s;
    const float down = -28.0f - s * 4.0f;
    const glm::vec3 v(std::sin(angle) * speed, std::cos(angle) * speed, rnd() * s * -2.0f + down);
    return fall(local, v, box, ctx, ground, start, 0.0f);
}

/// A snow flake (0x00785880): slower, and blowing every way when light.
inline Particle spawnSnow(const Random& rnd, const SpawnContext& ctx, HeightCache& ground, float start) {
    const glm::vec3 box = boxSize(Effect::Snow);
    const float ry = rnd(), rx = rnd();
    const glm::vec3 local((rx - 0.5f) * box.x, (ry - 0.5f) * box.y, 0.0f);
    const float s = ctx.strength;
    const float angle = (rnd() - 0.5f) * (6.2831855f - s * 5.9341197f) - 1.57f;
    const float speed = (rnd() - 0.5f) * s + s * 5.985f + 0.015f;
    const float down = -2.0f - s * 3.5f;
    const glm::vec3 v(std::sin(angle) * speed, std::cos(angle) * speed, rnd() * s * -1.0f + down);
    return fall(local, v, box, ctx, ground, start, -kSplashLife);
}

/// A grain of sand (0x00785ea0): blown in from the far side of the box,
/// turned to the way the player is going, for a little over three seconds or
/// until it meets the ground.
inline Particle spawnSand(const Random& rnd, const SpawnContext& ctx, HeightCache& ground, float start) {
    const glm::vec3 box = boxSize(Effect::Sand);
    const float rz = rnd(), ry = rnd(), rx = rnd();
    glm::vec3 pos(((rx) * 0.15f + 0.85f) * box.x, (ry - 0.5f) * box.y, (rz - 0.5f) * box.z);
    const float angle = (rnd() - 0.5f) * 0.34906587f - 1.57f;
    const float speed = (rnd() - 0.5f) * 0.6666667f + 18.666666f;
    glm::vec3 v(std::sin(angle) * speed, std::cos(angle) * speed,
                (rnd() - 0.5f) * 0.16666667f + 0.8333333f);
    const Mat3 turn = yawRotation(-ctx.wind.angle);
    v = turn.apply(v);
    pos = turn.apply(pos) + ctx.playerVelocity * 1.75f + ctx.camera;
    Particle p;
    p.position = pos;
    p.velocity = v;
    const float life = (rnd() - 0.5f) * 0.3f + 3.2f;
    if (ground.heightAt(pos) < pos.z) {
        glm::vec3 hit;
        ground.trace(pos, pos + v * life, hit);
        p.start = start;
        p.end = glm::length(pos - hit) / glm::length(v) + start;
    }
    return p;
}

/// When a packet's last particle is gone (0x00785640, 0x00785d60,
/// 0x00786210): a rain drop's tip falls on two yards past its end, a flake
/// fades a quarter second after landing, a splash lasts a quarter second.
inline float rainGone(const Particle& p) { return p.end - 2.0f / p.velocity.z; }
inline float snowGone(const Particle& p) { return std::max(p.end, 0.0f) + kSplashLife; }
inline float sandGone(const Particle& p) { return p.end; }

/// The splash a landed drop leaves (0x00785640): none for a dead drop or
/// when riding.
inline bool splashes(const Particle& p, bool riding) { return p.end != p.start && !riding; }
inline Splash splashOf(const Particle& p) {
    return {p.position + p.velocity * (p.end - p.start), p.end, p.end + kSplashLife};
}

/// The player's velocity as the weather sees it (0x0078c500): the moves of the
/// last 150 milliseconds or so over their time, plus one millisecond.
class VelocityWindow {
public:
    void add(const glm::vec3& delta, int ms) {
        if (ms <= 0) return;
        moves_[head_] = {delta, ms};
        head_ = (head_ + 1) % kSlots;
        count_ = std::min(count_ + 1, kSlots);
    }
    glm::vec3 velocity() const {
        glm::vec3 sum(0.0f);
        int ms = 0;
        for (int i = 0; i < count_; ++i) {
            const auto& m = moves_[(head_ - 1 - i + kSlots) % kSlots];
            sum += m.delta;
            ms += m.ms;
            if (ms > 0x95) break;
        }
        return sum * (1.0f / ((static_cast<float>(ms) + 1.0f) * 0.001f));
    }
    void clear() { count_ = 0; head_ = 0; }

private:
    struct Move { glm::vec3 delta{0.0f}; int ms = 0; };
    static constexpr int kSlots = 64;
    Move moves_[kSlots];
    int head_ = 0, count_ = 0;
};

/// The direction the weather leans to (0x0078d170): the way the player moves
/// when moving at a yard a second or more, else the way they face; and how
/// fast.
inline Wind windFor(const glm::vec3& velocity, float facing) {
    Wind w;
    const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    if (speed < 1.0f) {
        w.angle = facing;
    } else {
        w.angle = std::atan2(velocity.y, velocity.x);
        if (w.angle < 0.0f) w.angle += 6.2831855f;
    }
    w.speed = speed;
    return w;
}

/// The mist sheets every effect drifts through the box (0x0078c420 makes
/// them, 0x00786560 and 0x00786330 start one, 0x00786e10 moves and draws
/// them): 12-yard billboards of SnowMist01 (WeatherMistGrainy01 for sand) in
/// the fog's colour, a few dozen a second at most, each crossing the 44 x 44
/// x 25 box for about 2.7 seconds and climbing over the ground it meets.
struct MistSpec {
    const char* texture = "";
    float angle = -1.57f, angleSpread = 0.34906587f;  ///< +0x24, +0x28
    float speed = 0.0f, speedSpread = 0.0f;           ///< +0x34, +0x38
};
inline MistSpec mistSpec(Effect e) {
    switch (e) {
        case Effect::Rain: return {"textures\\Weather\\SnowMist01.blp", -1.57f, 0.34906587f, 5.0f, 1.2f};
        case Effect::Snow: return {"textures\\Weather\\SnowMist01.blp", -1.57f, 0.34906587f, 9.0f, 3.0f};
        case Effect::Sand: return {"textures\\Weather\\WeatherMistGrainy01.blp", -1.57f, 0.34906587f, 15.0f, 4.5f};
        default: return {};
    }
}
constexpr float kMistSize = 12.0f;
constexpr float kMistFade = 0.4f;
constexpr int kMistSlots = 0x80;
constexpr int kMistPathSteps = 64;
inline glm::vec3 mistBox() { return {44.0f, 44.0f, 25.0f}; }

/// Mist sheets made a second (0x007840b0, 0x007844f0, 0x00784580): rain's
/// and snow's only past half strength.
inline float mistRate(Effect e, float density, float strength) {
    switch (e) {
        case Effect::Rain: return std::max(strength - 0.5f, 0.0f) * 2.0f * density * 38.0f;
        case Effect::Snow: return std::max(strength - 0.5f, 0.0f) * 2.0f * density * 48.0f;
        case Effect::Sand: return density * 64.0f * strength;
        default: return 0.0f;
    }
}

struct Mist {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float rise = 0.0f;   ///< +0x128, gaining 5/3 each time the ground lifts it
    float start = 0.0f;  ///< 0 when the slot is free
    float end = 0.0f;
    int steps = 0;
    float ground[kMistPathSteps] = {};
    bool live() const { return start != 0.0f || end != 0.0f; }
};

/// A sheet starting at `start` (0x00786560, then 0x00786330): sent across the
/// box against the way the player is going, set on the ground, its path's
/// ground heights read ahead a cell at a time; cut short where the ground
/// climbs too steeply, and dropped if that is at once.
inline Mist spawnMist(const Random& rnd, const MistSpec& spec, const SpawnContext& ctx,
                      HeightCache& ground, float start) {
    Mist m;
    m.start = start;
    const float angle = (rnd() - 0.5f) * spec.angleSpread + spec.angle;
    const float speed = (rnd() - 0.5f) * spec.speedSpread + spec.speed;
    glm::vec3 v(std::sin(angle) * speed, std::cos(angle) * speed,
                (rnd() - 0.5f) * 0.033333335f + 0.33333334f);
    const Mat3 turn = yawRotation(-ctx.wind.angle);
    m.velocity = turn.apply(v);
    const glm::vec3 box = mistBox();
    const float rz = rnd(), ry = rnd(), rx = rnd();
    glm::vec3 pos = turn.apply({(rx - 0.5f) * box.x, (ry - 0.5f) * box.y, (rz - 0.5f) * box.z});
    pos += ctx.camera - m.velocity * 1.5f;
    const float h = std::max(ground.heightAt(pos), pos.z);
    pos.z = kMistSize * 0.5f + h;
    m.position = pos;
    m.rise = (rnd() - 0.5f) * 3.3333333f;
    const float life = (rnd() - 0.5f) * 0.3f + 2.7f;
    m.end = start + life;

    // 0x00786330: the ground along the way, a cell apart.
    const float flat = std::sqrt(m.velocity.x * m.velocity.x + m.velocity.y * m.velocity.y);
    if (flat <= 0.0f) { m.start = m.end = 0.0f; return m; }
    const glm::vec2 step = glm::vec2(m.velocity) / flat * HeightCache::kCellSize;
    int count = static_cast<int>(std::lround(flat * life / glm::length(step)));
    if (count > kMistPathSteps - 1) count = kMistPathSteps;
    m.steps = count;
    glm::vec3 at = pos;
    for (int i = 0; i < count; ++i) {
        m.ground[i] = ground.heightAt(at);
        at.x += step.x;
        at.y += step.y;
    }
    for (int i = 0; count != 3 && i < count - 3; ++i) {
        const float* g = m.ground + i;
        if (g[3] - g[0] > 1.0f || g[2] - g[0] > 0.75f || g[1] - g[0] > 0.5f) {
            const float cut = (static_cast<float>(i + 1) / static_cast<float>(count)) * life;
            m.steps = i;
            m.end = m.start + cut;
            if (i == 0) m.start = m.end = 0.0f;
            break;
        }
    }
    return m;
}

/// One frame of a sheet from `before` to `now` (0x00786e10): it drifts,
/// accelerating up or down by its rise, and is pushed up - a quarter of its
/// size a frame at most - when the ground under its path is above it.
inline void stepMist(Mist& m, float before, float now) {
    const float dt = now - before;
    const float life = m.end - m.start;
    const float lift = dt * dt * m.rise * 0.5f;
    m.position += m.velocity * dt + glm::vec3(lift);
    if (m.steps <= 0 || life <= 0.0f) return;
    const float from = m.start < before ? (before - m.start) / life : 0.0f;
    const float to = std::min((now - m.start) / life, 1.0f);
    const float n = static_cast<float>(m.steps);
    const int a = std::clamp(static_cast<int>(std::lround(from * n)), 0, m.steps - 1);
    int b = static_cast<int>(std::lround(n * to));
    if (b == a) b = a + 1;
    b = std::clamp(b, 0, m.steps - 1);
    const float g = kMistSize * 0.5f + (n * from - static_cast<float>(a)) * (m.ground[b] - m.ground[a]) +
                    m.ground[a];
    if (m.position.z < g) {
        m.rise += 1.6666666f;
        m.position.z += std::min(g - m.position.z, kMistSize * 0.25f);
    }
}

/// How opaque a sheet is (0x00786e10): in over its first 0.4 seconds, out over
/// its last, and fading toward a corner as that corner nears the camera,
/// gone within six yards.
inline float mistAlpha(const Mist& m, float now, float cornerDistance) {
    const float age = now - m.start;
    const float in = std::clamp(1.0f - age / kMistFade, 0.0f, 1.0f);
    const float out = std::clamp((m.end - m.start - age) / kMistFade, 0.0f, 1.0f);
    const float near = std::clamp(1.5f - cornerDistance * 0.083333336f, 0.0f, 1.0f);
    return (1.0f - near) * out * (1.0f - in);
}

/// Weather.dbc's columns the client reads (0x007846a0): EffectType at +8, a
/// float at +0xc it hands the light, EffectColor at +0x10, EffectTexture at
/// +0x1c.
struct WeatherRow {
    Effect effect = Effect::None;
    glm::vec3 color{1.0f};
    std::string texture;
};

/// The texture an effect draws with when its row names none (0x00783b90).
/// Sand draws untextured points.
inline const char* defaultTexture(Effect e) {
    switch (e) {
        case Effect::Rain: return "textures\\Weather\\RainDrop01.blp";
        case Effect::Snow: return "textures\\Weather\\SnowFlake01.blp";
        default: return "";
    }
}
/// Rain's splashes (0x0078ca90).
constexpr const char* kSplashTexture = "textures\\Weather\\RainDropSplash01.blp";

}  // namespace wowee::rendering::client_weather
