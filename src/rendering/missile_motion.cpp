#include "rendering/missile_motion.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace wowee::rendering::missile_motion {

namespace {

constexpr const char* kInputs[] = {"progress",      "time",         "missileIndex",  "missileCount",
                                   "distanceToFirePos", "distanceToImpactPos", "startDistance", "totalDistance",
                                   "rand1",         "rand2",        "rand3",         "spellID"};
constexpr const char* kOutputs[] = {"transAngle", "transMag",   "transRight", "transFront", "transUp",  "modelYaw",
                                    "modelPitch", "modelRoll",  "speedAbs",   "speedScalar", "speedOffset", "scale"};

// The globals a script may call: the client runs them in the interface's
// state, whose bare math names are compat.lua's and whose trigonometry is in
// degrees.
constexpr const char* kPrelude =
    "local m = math\n"
    "abs, ceil, floor, max, min, mod, sqrt, random = m.abs, m.ceil, m.floor, m.max, m.min, m.fmod, m.sqrt, m.random\n"
    "exp, log, log10, frexp, ldexp, deg, rad = m.exp, m.log, m.log10, m.frexp, m.ldexp, m.deg, m.rad\n"
    "local toRad, toDeg = m.pi / 180, 180 / m.pi\n"
    "cos = function(x) return m.cos(x * toRad) end\n"
    "sin = function(x) return m.sin(x * toRad) end\n"
    "tan = function(x) return m.tan(x * toRad) end\n"
    "acos = function(x) return m.acos(x) * toDeg end\n"
    "asin = function(x) return m.asin(x) * toDeg end\n"
    "atan = function(x) return m.atan(x) * toDeg end\n"
    "atan2 = function(y, x) return m.atan2(y, x) * toDeg end\n";

double finiteOr0(double v) { return std::isfinite(v) ? v : 0.0; }

}  // namespace

std::string wrapScript(const std::string& body) {
    std::string s = "return function(";
    bool first = true;
    for (const char* name : kInputs) {
        if (!first) s += ", ";
        s += name;
        first = false;
    }
    s += ")\n";
    for (const char* name : kOutputs) {
        s += "local ";
        s += name;
        s += " = 0.0\n";
    }
    s += body;
    s += "\nreturn ";
    first = true;
    for (const char* name : kOutputs) {
        if (!first) s += ", ";
        s += name;
        first = false;
    }
    s += "\nend\n";
    return s;
}

glm::vec3 drawnPosition(const glm::vec3& path, const glm::vec3& firePos, const glm::vec3& target,
                        const Outputs& out) {
    float fx = target.x - firePos.x;
    float fy = target.y - firePos.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 2.3841858e-07f) {
        const float inv = 1.0f / std::sqrt(len2);
        fx *= inv;
        fy *= inv;
    }
    glm::vec3 p = path;
    if (out.transMag != 0.0) {
        const double a = out.transAngle * 0.017453292;
        const float s = static_cast<float>(std::sin(a) * out.transMag);
        p.x += s * -fy;
        p.y += s * fx;
        p.z += static_cast<float>(std::cos(a) * out.transMag);
    }
    const auto right = static_cast<float>(out.transRight);
    p.x += right * -fy;
    p.y += right * fx;
    const auto front = static_cast<float>(out.transFront);
    p.x += front * fx;
    p.y += front * fy;
    p.z += static_cast<float>(out.transUp);
    return p;
}

glm::mat4 modelTurn(const Outputs& out) {
    constexpr float kRad = 0.017453292f;
    glm::mat4 m(1.0f);
    if (out.modelYaw != 0.0) m = glm::rotate(m, -static_cast<float>(out.modelYaw) * kRad, glm::vec3(0, 0, 1));
    if (out.modelPitch != 0.0) m = glm::rotate(m, static_cast<float>(out.modelPitch) * kRad, glm::vec3(0, 1, 0));
    if (out.modelRoll != 0.0) m = glm::rotate(m, -static_cast<float>(out.modelRoll) * kRad, glm::vec3(1, 0, 0));
    if (out.scale > 0.0) m = glm::scale(m, glm::vec3(std::max(static_cast<float>(out.scale), 0.01f)));
    return m;
}

float speedAfter(float speed, float baseSpeed, const Outputs& out) {
    if (out.speedAbs > 0.0) speed = static_cast<float>(out.speedAbs);
    if (out.speedScalar > 0.0) speed = static_cast<float>(out.speedScalar) * baseSpeed;
    if (out.speedOffset != 0.0) {
        const float s = static_cast<float>(out.speedOffset) + baseSpeed;
        speed = s > 0.01f ? s : 0.01f;
    }
    return speed;
}

Scripts::Scripts() {
    L_ = luaL_newstate();
    if (!L_) return;
    for (lua_CFunction open : {luaopen_base, luaopen_math}) {
        lua_pushcfunction(L_, open);
        lua_call(L_, 0, 0);
    }
    lua_settop(L_, 0);
    if (luaL_loadstring(L_, kPrelude) == 0) lua_pcall(L_, 0, 0, 0);
    lua_settop(L_, 0);
}

Scripts::~Scripts() {
    if (L_) lua_close(L_);
}

bool Scripts::compile(uint32_t id, const std::string& body) {
    if (!L_) return false;
    const std::string chunk = wrapScript(body);
    if (luaL_loadbuffer(L_, chunk.data(), chunk.size(), "SpellMissileMotion") != 0) {
        lua_pop(L_, 1);
        return false;
    }
    if (lua_pcall(L_, 0, 1, 0) != 0 || !lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    if (auto it = refs_.find(id); it != refs_.end()) luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    refs_[id] = luaL_ref(L_, LUA_REGISTRYINDEX);
    return true;
}

std::optional<Outputs> Scripts::run(uint32_t id, const Inputs& in) {
    const auto it = refs_.find(id);
    if (!L_ || it == refs_.end()) return std::nullopt;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
    for (double v : {in.progress, in.time, in.missileIndex, in.missileCount, in.distanceToFirePos,
                     in.distanceToImpactPos, in.startDistance, in.totalDistance, in.rand1, in.rand2, in.rand3,
                     in.spellID})
        lua_pushnumber(L_, v);
    if (lua_pcall(L_, 12, 12, 0) != 0) {
        lua_pop(L_, 1);
        return std::nullopt;
    }
    double v[12];
    for (int i = 0; i < 12; ++i) v[i] = finiteOr0(lua_tonumber(L_, i - 12));
    lua_pop(L_, 12);
    Outputs o;
    o.transAngle = v[0];
    o.transMag = v[1];
    o.transRight = v[2];
    o.transFront = v[3];
    o.transUp = v[4];
    o.modelYaw = v[5];
    o.modelPitch = v[6];
    o.modelRoll = v[7];
    o.speedAbs = v[8];
    o.speedScalar = v[9];
    o.speedOffset = v[10];
    o.scale = v[11];
    return o;
}

}  // namespace wowee::rendering::missile_motion
