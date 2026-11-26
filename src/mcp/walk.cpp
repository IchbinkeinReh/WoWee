// A walk by keys: W, the turn keys, and SPACE where something low is in the
// way, along the way walk_planner.hpp found.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <utility>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/protocol_constants.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/walk_planner.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"

namespace wowee::mcp {

namespace {

constexpr float kDegrees = 57.29578f;

float flat(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a) - glm::vec2(b));
}

class Walker {
public:
    Walker(ToolContext& context, std::function<std::optional<glm::vec3>()> goal, WalkOptions options,
           std::shared_ptr<json> told)
        : context_(context), goal_(std::move(goal)), options_(options), told_(std::move(told)),
          forward_(context, SDL_SCANCODE_W), left_(context, SDL_SCANCODE_A), right_(context, SDL_SCANCODE_D),
          up_(context, SDL_SCANCODE_SPACE), down_(context, SDL_SCANCODE_X), back_(context, SDL_SCANCODE_S) {}

    Script::Step step(Script& s) {
        if (!begun_) begin(s);
        auto* gh = context_.app.getGameHandler();
        auto* renderer = context_.app.getRenderer();
        auto* cc = renderer ? renderer->getCameraController() : nullptr;
        if (!gh || !cc || !inWorld(context_)) return over(s, "left the world", false);
        // A ghost walks (to its corpse, a spirit healer); only a body lying
        // dead ends the walk.
        if (gh->isPlayerDead() && !gh->isPlayerGhost()) return over(s, "the character died", false);
        if (gh->getCurrentMapId() != map_) return over(s, "the map changed", false);
        const auto goal = goal_();
        if (!goal) return over(s, "what was walked to is gone", false);
        const glm::vec3 me = playerAt(context_);
        travelled_ += flat(me, last_);
        last_ = me;
        const float away = flat(me, *goal);
        const float dz = goal->z - me.z;
        const bool floats = cc->isSwimming() || cc->isFlightAirborne();
        if (away <= options_.stopWithin && (std::fabs(dz) < 4.0f || floats)) return over(s, "arrived", true);
        if (s.now() - start_ > options_.timeout) return over(s, "time ran out", false);
        if (options_.stopInCombat && inCombat(*gh)) return over(s, "in combat", false);

        if (backUntil_ > s.now()) return Script::Step::Again;
        back_.set(false);
        // Afloat, straight at it: the ways planned are over the ground, and
        // what is looked for is the shore.
        if (floats) {
            points_.clear();
            replan_ = true;
            blank_ = 0;
            steer(me, *goal, true, true, dz);
            watch(s, me, *cc);
            return gaveUp_ ? over(s, "no way there was found", false) : Script::Step::Again;
        }
        plan(s, me, *goal);
        if (planning_.valid() && points_.empty()) {
            steer(me, *goal, false, false, dz);
            return Script::Step::Again;
        }
        if (gaveUp_) return over(s, "no way there was found", false);

        glm::vec3 next = *goal;
        while (!points_.empty() && passed(me, points_.front())) {
            leg_ = points_.front();
            points_.erase(points_.begin());
        }
        if (!points_.empty()) next = points_.front();
        else if (!reaches_) replan_ = true;  // the end of what was known
        steer(me, next, true, false, 0.0f);
        watch(s, me, *cc);
        return Script::Step::Again;
    }

private:
    void begin(Script& s) {
        begun_ = true;
        start_ = s.now();
        last_ = from_ = watchedAt_ = playerAt(context_);
        watchedSince_ = s.now();
        if (auto* gh = context_.app.getGameHandler()) map_ = gh->getCurrentMapId();
    }

    /// At a corner of the way, or just by it: what was looked at is the line
    /// to it and the line from it, not one that cuts it off.
    bool passed(const glm::vec3& me, const glm::vec3& point) const {
        const float away = flat(me, point);
        if (away < 0.6f) return true;
        const glm::vec2 along = glm::vec2(point) - glm::vec2(leg_);
        const glm::vec2 left = glm::vec2(point) - glm::vec2(me);
        return away < 1.5f && glm::dot(along, left) <= 0.0f;
    }

    static bool inCombat(game::GameHandler& gh) {
        const auto self = gh.getEntityManager().getEntity(gh.getPlayerGuid());
        return self && self->getType() == game::ObjectType::PLAYER &&
               (static_cast<const game::Unit&>(*self).getUnitFlags() & game::UNIT_FLAG_IN_COMBAT) != 0;
    }

    /// The way worked out on another thread, over faces gathered here.
    void plan(Script& s, const glm::vec3& me, const glm::vec3& goal) {
        if (planning_.valid()) {
            if (planning_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const WalkPlan made = planning_.get();
            ++plans_;
            points_ = made.points;
            leg_ = me;
            reaches_ = made.reaches;
            // Nothing to walk and no nearer to it: twice over, and it is
            // given up.
            if (points_.empty() && !reaches_) {
                if (++blank_ >= 2) gaveUp_ = true;
            } else {
                blank_ = 0;
            }
            return;
        }
        const bool moved = flat(goal, plannedGoal_) > 4.0f;
        if (plans_ > 0 && !replan_ && !moved) return;
        if (s.now() - lastPlan_ < 0.8) return;
        lastPlan_ = s.now();
        replan_ = false;
        plannedGoal_ = goal;
        auto facets = facetsForWalk(context_, me, goal);
        WalkLimits limits;
        limits.avoid = avoid_;
        if (auto* renderer = context_.app.getRenderer()) {
            if (const auto* cc = renderer->getCameraController()) {
                limits.bodyHeight = std::max(limits.bodyHeight, cc->collisionHeight());
            }
        }
        planning_ = std::async(std::launch::async, [facets = std::move(facets), me, goal, limits]() mutable {
            return planWalk(FacetIndex(std::move(facets)), me, goal, limits);
        });
    }

    void steer(const glm::vec3& me, const glm::vec3& to, bool go, bool floats, float climb) {
        const float err = turnTo(me, playerFacing(context_), to) * kDegrees;
        const float frame = 180.0f * static_cast<float>(std::clamp(context_.frameSeconds, 0.005, 0.1));
        const float stopAt = std::max(2.0f, frame * 0.6f);
        const bool turning = left_.isDown() || right_.isDown();
        const bool turn = std::fabs(err) > (turning ? stopAt : stopAt + 4.0f);
        left_.set(turn && err > 0.0f);
        right_.set(turn && err < 0.0f);
        // Standing while it looks far away from where it goes; nearer the
        // place, less far.
        const float most = flat(me, to) < 5.0f ? 30.0f : 60.0f;
        forward_.set(go && std::fabs(err) < most);
        up_.set(floats && climb > 1.5f);
        down_.set(floats && climb < -1.5f);
    }

    /// Whether it gets on; what to do where it does not.
    void watch(Script& s, const glm::vec3& me, const rendering::CameraController& cc) {
        if (!forward_.isDown()) {
            watchedSince_ = s.now();
            watchedAt_ = me;
            return;
        }
        if (s.now() - watchedSince_ < 0.7) return;
        const float moved = flat(me, watchedAt_);
        watchedSince_ = s.now();
        watchedAt_ = me;
        if (moved > 1.2f || !cc.isGrounded()) {
            if (moved > 2.5f) stuckRun_ = 0;
            return;
        }
        ++stuck_;
        ++stuckRun_;
        if (stuckAt_.size() < 6) stuckAt_.push_back(me);
        if (stuckRun_ == 1) {
            // Something low, most often.
            ++jumps_;
            append(s, pressKey(context_, SDL_SCANCODE_SPACE, SDL_KMOD_NONE, 0.08));
        } else if (stuckRun_ >= 10) {
            gaveUp_ = true;
        } else {
            // Back a step, and looked at anew, with what is in front here
            // known to be in the way.
            const float facing = playerFacing(context_);
            avoid_.push_back(me + 0.9f * glm::vec3(std::cos(facing), std::sin(facing), 0.0f));
            forward_.set(false);
            back_.set(true);
            backUntil_ = s.now() + 0.5 + 0.1 * stuckRun_;
            replan_ = true;
            points_.clear();
        }
    }

    /// Steps put in front of this one, which goes on after them.
    void append(Script& s, Steps steps) {
        pending_ = std::move(steps);
        (void)s;
    }

public:
    /// What watch() wanted pressed before the next step.
    Steps takePending() { return std::exchange(pending_, {}); }

private:
    Script::Step over(Script& s, const char* why, bool arrived) {
        forward_.set(false);
        left_.set(false);
        right_.set(false);
        up_.set(false);
        down_.set(false);
        back_.set(false);
        const glm::vec3 me = playerAt(context_);
        json out = {{"arrived", arrived},
                    {"why", why},
                    {"at", vec3Json(me)},
                    {"from", vec3Json(from_)},
                    {"walked_yards", rounded(travelled_, 1)},
                    {"seconds", rounded(s.now() - start_, 1)},
                    {"plans", plans_}};
        if (const auto goal = goal_()) out["yards_left"] = rounded(glm::length(*goal - me), 1);
        if (stuck_ > 0) {
            out["stuck"] = stuck_;
            json at = json::array();
            for (const auto& p : stuckAt_) at.push_back(vec3Json(p));
            out["stuck_at"] = std::move(at);
        }
        if (jumps_ > 0) out["jumps"] = jumps_;
        // What held the character's keys as it ended, where anything did (T402).
        if (!arrived && !context_.app.movementHolds().empty()) out["held_by"] = context_.app.movementHolds();
        if (told_) *told_ = std::move(out);
        return Script::Step::Next;
    }

    ToolContext& context_;
    std::function<std::optional<glm::vec3>()> goal_;
    WalkOptions options_;
    std::shared_ptr<json> told_;
    HeldKey forward_, left_, right_, up_, down_, back_;
    bool begun_ = false;
    double start_ = 0.0;
    uint32_t map_ = 0;
    glm::vec3 from_{0.0f};
    glm::vec3 last_{0.0f};
    float travelled_ = 0.0f;
    std::future<WalkPlan> planning_;
    std::vector<glm::vec3> points_;
    glm::vec3 leg_{0.0f};             ///< where the way to the next point began
    std::vector<glm::vec3> avoid_;    ///< where it was stuck
    glm::vec3 plannedGoal_{0.0f};
    bool reaches_ = false;
    bool replan_ = false;
    bool gaveUp_ = false;
    int plans_ = 0;
    int blank_ = 0;
    double lastPlan_ = -10.0;
    double watchedSince_ = 0.0;
    glm::vec3 watchedAt_{0.0f};
    double backUntil_ = 0.0;
    int stuck_ = 0;
    int stuckRun_ = 0;
    std::vector<glm::vec3> stuckAt_;
    int jumps_ = 0;
    Steps pending_;
};

}  // namespace

Steps walkTo(ToolContext& context, std::function<std::optional<glm::vec3>()> goal, WalkOptions options,
             std::shared_ptr<json> told) {
    auto walker = std::make_shared<Walker>(context, std::move(goal), options, std::move(told));
    Steps out;
    out.push_back(wantKeysFree(context));
    // A step that asks itself again, with what it wants pressed put first.
    struct Loop {
        static Script::Fn make(std::shared_ptr<Walker> w) {
            return [w](Script& s) {
                const Script::Step what = w->step(s);
                if (what != Script::Step::Again) return what;
                Steps press = w->takePending();
                if (press.empty()) return what;
                press.push_back(make(w));
                s.first(std::move(press));
                return Script::Step::Next;
            };
        }
    };
    out.push_back(Loop::make(std::move(walker)));
    out.push_back(Script::frames(2));
    return out;
}

}  // namespace wowee::mcp
