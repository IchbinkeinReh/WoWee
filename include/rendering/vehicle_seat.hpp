#pragma once

// A passenger in its vehicle's seat, as 0x0074a7f0 draws one in state 3:
// its model hung on the vehicle model's attachment for the seat (0x00831630)
// by a matrix 0x007490f0 builds, playing the seat's ride animation
// (0x00747b20 from 0x007385c0), hidden where the seat says so (0x00730f30).

#include <cmath>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering::vehicle_seat {

/// VehicleSeat AttachmentID (+8) to the vehicle model's attachment
/// (DAT_00a2d3f0, 22 entries; 0x007493b0, 0x0074a7f0). -1 past the table.
[[nodiscard]] inline int32_t seatModelAttachment(int32_t seatAttachmentId) {
    static constexpr int32_t kTable[22] = {20, 34, 19, 21, 22, 17, 23, 24, 25, 15, 16,
                                           37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 0};
    if (seatAttachmentId < 0 || seatAttachmentId >= 22) return -1;
    return kTable[seatAttachmentId];
}

/// What 0x007490f0 builds the passenger's matrix from.
struct PassengerPose {
    float facing = 0.0f;           ///< the passenger's facing on the vehicle (+0xaa0)
    float seatYaw = 0.0f;          ///< VehicleSeat PassengerYaw (+0x74)
    float seatPitch = 0.0f;        ///< PassengerPitch (+0x78)
    float seatRoll = 0.0f;         ///< PassengerRoll (+0x7c)
    glm::vec3 seatOffset{0.0f};    ///< AttachmentOffset (+0x0c)
    float passengerScale = 1.0f;   ///< the passenger's model scale (vtable +0x7c)
    /// The passenger model's PassengerAttachmentID (+0x80) attachment, where
    /// the model has it (0x00748400: its position, negated into +0xcc).
    std::optional<glm::vec3> passengerAttachment;
};

/// The passenger's matrix in its frame (0x007490f0): turned by its facing
/// less the seat's yaw, then the seat's roll and pitch where they are not 0,
/// sized to the passenger's scale over the frame's (the length of the
/// frame's first axis, so the vehicle's own size and its bone's do not
/// reach the passenger), placed so the passenger's attachment lands on the
/// seat's offset. `frameScale` is that length.
[[nodiscard]] inline glm::mat4 passengerLocal(const PassengerPose& pose, float frameScale) {
    glm::mat4 m(1.0f);
    // Row-vector v * Ry * Rx * Rz there, column-vector Rz * Rx * Ry here.
    m = glm::rotate(m, pose.facing - pose.seatYaw, glm::vec3(0.0f, 0.0f, 1.0f));  // 0x004c3380
    if (pose.seatRoll != 0.0f) m = glm::rotate(m, pose.seatRoll, glm::vec3(1.0f, 0.0f, 0.0f));    // 0x004c3300
    if (pose.seatPitch != 0.0f) m = glm::rotate(m, pose.seatPitch, glm::vec3(0.0f, 1.0f, 0.0f));  // 0x004c3340
    const float k = frameScale > 0.0001f ? pose.passengerScale / frameScale : pose.passengerScale;
    m = glm::scale(m, glm::vec3(k));  // 0x004c1bf0
    glm::vec3 t = pose.seatOffset;
    if (pose.passengerAttachment) t += glm::mat3(m) * -*pose.passengerAttachment;  // 0x004c2300
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

/// The passenger's world matrix: hung on `frame`, the vehicle model's seat
/// attachment in the world (0x00831410), or - where the model lacks it - the
/// vehicle model's own matrix (0x007490f0's other path, which comes to the
/// same: the seat offset by the vehicle's size, the passenger at its own).
[[nodiscard]] inline glm::mat4 passengerWorld(const glm::mat4& frame, const PassengerPose& pose) {
    return frame * passengerLocal(pose, glm::length(glm::vec3(frame[0])));
}

/// A passenger on its way into or out of a seat (0x0074a7f0, states 1, 2,
/// 4 and 5): drawn at a place on the straight way from where it began to
/// its end, lifted by the arc's height, facing as it turns - upright, its
/// own size (0x0082dd80 with the normal straight up).
struct Flight {
    glm::vec3 from{0.0f};           ///< where it began (+0x84, or +0x90 on its transport)
    std::optional<glm::vec3> to;    ///< where it ends; none for its seat (0x007493b0)
    float progress = 0.0f;          ///< +0x48
    float height = 0.0f;            ///< the arc's
    float facing = 0.0f;            ///< +0xbc, or +0xb4 while it waits
};

/// The flight's matrix, `target` its end this frame.
[[nodiscard]] inline glm::mat4 flightWorld(const Flight& f, const glm::vec3& target, float scale) {
    const float p = f.progress;
    glm::vec3 at = f.from * (1.0f - p) + target * p;
    at.z += f.height;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), at);
    m = glm::rotate(m, f.facing, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, glm::vec3(scale));
}

/// The ride animation a seated passenger plays (0x00747b20 state 3): with
/// seat Flags 0x2, RideAnimStart once where it has one, then RideAnimLoop.
/// Nothing (0x1fa) without the flag or either animation: the passenger then
/// animates as itself.
struct RideAnimation {
    std::optional<uint32_t> start;
    std::optional<uint32_t> loop;
};
[[nodiscard]] inline std::optional<RideAnimation> rideAnimation(uint32_t seatFlags, int32_t rideAnimStart,
                                                                int32_t rideAnimLoop) {
    constexpr uint32_t kHasRideAnim = 0x2;
    constexpr int32_t kNoAnimation = 0x1fa;
    if ((seatFlags & kHasRideAnim) == 0) return std::nullopt;
    RideAnimation out;
    if (rideAnimStart >= 0 && rideAnimStart != kNoAnimation) out.start = static_cast<uint32_t>(rideAnimStart);
    if (rideAnimLoop >= 0 && rideAnimLoop != kNoAnimation) out.loop = static_cast<uint32_t>(rideAnimLoop);
    if (!out.start && !out.loop) return std::nullopt;
    return out;
}

}  // namespace wowee::rendering::vehicle_seat
