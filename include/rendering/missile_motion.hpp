#pragma once

// A spell missile's motion script (SpellMissileMotion.dbc), as the client
// runs it (Missile_C.cpp and its script cache, 0x007587d0, 0x00700350,
// 0x00700550, 0x006feb20).
//
// Each row's ScriptBody is Lua. The client wraps it as
//
//   return function(progress, time, missileIndex, ...)
//   local transAngle = 0.0
//   ...
//   <body>
//   return transAngle, transMag, ...
//   end
//
// compiles it once, and every frame calls it with the missile's twelve
// inputs; the twelve outputs move where the missile is drawn off its path,
// turn and size its model, and change its speed. WoW's trigonometric globals
// take degrees, as the interface's do.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

struct lua_State;

namespace wowee::rendering::missile_motion {

/// The inputs, in the order the client names them (0x00ada7c0).
struct Inputs {
    double progress = 0.0;
    double time = 0.0;
    double missileIndex = 0.0;
    double missileCount = 1.0;
    double distanceToFirePos = 0.0;
    double distanceToImpactPos = 0.0;
    double startDistance = 0.0;
    double totalDistance = 0.0;
    double rand1 = 0.0, rand2 = 0.0, rand3 = 0.0;
    double spellID = 0.0;
};

/// The outputs (0x00ada7f4), each 0 where the script left it not finite.
struct Outputs {
    double transAngle = 0.0, transMag = 0.0, transRight = 0.0, transFront = 0.0, transUp = 0.0;
    double modelYaw = 0.0, modelPitch = 0.0, modelRoll = 0.0;
    double speedAbs = 0.0, speedScalar = 0.0, speedOffset = 0.0, scale = 0.0;
};

/// The chunk 0x007587d0 builds around a script body.
[[nodiscard]] std::string wrapScript(const std::string& body);

/// Where the missile is drawn (0x00700550): `path` moved by the outputs in
/// the frame of the way from `firePos` to `target` along the ground -
/// transAngle/transMag round that way (0 up), transRight across it,
/// transFront along it, transUp straight up. Positions in the world's
/// axes (x, y on the ground, z up).
[[nodiscard]] glm::vec3 drawnPosition(const glm::vec3& path, const glm::vec3& firePos, const glm::vec3& target,
                                      const Outputs& out);

/// The model's turn and size from the outputs (0x006feb20), to compose
/// after its placement: modelYaw, modelPitch and modelRoll in degrees about
/// its own up, side and forward axes - yaw, then pitch, then roll - and
/// scale where above 0, held to 0.01. In this client's render axes, whose
/// X and Y are the world's swapped, so a model's side axis is the client's
/// reversed and yaw and roll turn the other way.
[[nodiscard]] glm::mat4 modelTurn(const Outputs& out);

/// The speed after the outputs (0x00700350): speedAbs, else speedScalar
/// times the base speed, and speedOffset onto the base speed held to 0.01.
[[nodiscard]] float speedAfter(float speed, float baseSpeed, const Outputs& out);

/// The scripts, compiled once each, in a Lua state of their own with WoW's
/// math globals.
class Scripts {
public:
    Scripts();
    ~Scripts();
    Scripts(const Scripts&) = delete;
    Scripts& operator=(const Scripts&) = delete;

    /// Compiles a row's script; false where it does not compile, as the
    /// client keeps no motion then (0x007587d0 answers -1).
    bool compile(uint32_t id, const std::string& body);
    [[nodiscard]] bool has(uint32_t id) const { return refs_.count(id) != 0; }
    /// One call; nullopt where it raises, which ends the missile's motion
    /// (0x00701230 sets +0x17c to -1).
    std::optional<Outputs> run(uint32_t id, const Inputs& in);

private:
    lua_State* L_ = nullptr;
    std::unordered_map<uint32_t, int> refs_;
};

}  // namespace wowee::rendering::missile_motion
