#pragma once

// A GameHandler fed server packets the way the socket feeds it, with no server,
// window or renderer behind it.
//
// Most of what goes wrong between this client and a realm is a byte read from
// the wrong place: a flag tested against the wrong bit, a field index from the
// wrong expansion, a guid read packed where the server wrote it whole. None of
// that shows in a unit test of a helper, because the helper is right and the
// call site is not. Here the packet goes in through the same dispatch the
// realm's would, with one expansion active and its own opcode and update field
// tables and packet parsers, and the test reads back what the client concluded.
//
// The packets are built by hand, field by field as the server writes them;
// each test says which server function it copies.
//
// The socket is never connected; it only keeps what the client sends, for a
// test to read back.

#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include "core/application.hpp"
#include "core/app_clock.hpp"
#include "game/expansion_profile.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wowee {
namespace game {

struct GameHandlerTestAccess {
    // Queued and drained the way update() drains the socket's packets, frame
    // after frame, since update object blocks are applied after their packet
    // rather than inside it. Bounded, so work that never drains fails here
    // instead of hanging the run or leaving the test half-applied state.
    static void receive(GameHandler& gh, const network::Packet& packet) {
        gh.enqueueIncomingPacket(packet);
        auto pending = [&] {
            return !gh.pendingIncomingPackets_.empty() ||
                   gh.entityController_->hasPendingUpdateObjectWork();
        };
        for (int frame = 0; frame < 1000 && pending(); ++frame) gh.processQueuedIncomingPackets();
        REQUIRE_FALSE(pending());
    }
    // What one frame makes of what is there.
    static void queue(GameHandler& gh, const network::Packet& packet) { gh.enqueueIncomingPacket(packet); }
    static void frame(GameHandler& gh) { gh.processQueuedIncomingPackets(); }
    static size_t queued(const GameHandler& gh) {
        return gh.pendingIncomingPackets_.size() + (gh.entityController_->hasPendingUpdateObjectWork() ? 1 : 0);
    }
    static void enterWorld(GameHandler& gh) { gh.setState(WorldState::IN_WORLD); }
    static void setSocket(GameHandler& gh, std::unique_ptr<network::WorldSocket> socket) {
        gh.socket = std::move(socket);
    }
    static void setCharacter(GameHandler& gh, const std::string& account, const std::string& name, uint64_t guid) {
        gh.accountName = account;
        Character character{};
        character.guid = guid;
        character.name = name;
        gh.characters = {character};
        gh.activeCharacterGuid_ = guid;
    }
    // An Achievement.dbc row and an Achievement_Category.dbc row as the caches
    // hold them, for tests without the client's files.
    static void setAchievement(GameHandler& gh, uint32_t id, uint32_t category, uint32_t uiOrder) {
        gh.achievementCategoryCache_[id] = category;
        gh.achievementUiOrderCache_[id] = uiOrder;
    }
    // Achievement.dbc rows in the file's order, as the loader keeps them.
    static void setAchievementRows(GameHandler& gh, std::vector<AchievementRow> rows) {
        gh.achievementRows_ = std::move(rows);
        gh.achievementCategoriesLoaded_ = true;
    }
    static void setAchievementCategory(GameHandler& gh, uint32_t id, int32_t parent, bool statistic) {
        GameHandler::AchievementCategoryInfo info;
        info.parentId = parent;
        info.isStatistic = statistic;
        gh.achievementCategoryInfo_[id] = info;
    }
    static void setExpansions(core::Application& app, std::unique_ptr<ExpansionRegistry> registry) {
        app.expansionRegistry_ = std::move(registry);
    }
};

}  // namespace game
}  // namespace wowee

namespace replay {

using wowee::game::Opcode;
using wowee::game::UF;

// SMSG_UPDATE_OBJECT, one block per call, in WotLK's layout. Fields are keyed by name and
// resolved through the loaded update field table, so a test states which field
// it sets rather than which index that is this expansion.
class UpdateObject {
public:
    using Fields = std::map<UF, uint32_t>;

    // An object appearing, laid out as Object::BuildCreateUpdateBlockForPlayer
    // writes it, but with the movement part cut down to a stationary position
    // (UPDATEFLAG_STATIONARY_POSITION): where it stands does not matter to
    // anything these tests read. `self` is UPDATEFLAG_SELF, the mark on the
    // receiving player's own block, which adds nothing to the payload.
    UpdateObject& create(uint64_t guid, wowee::game::ObjectType type, const Fields& fields,
                         bool self = false) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
        body_.writePackedGuid(guid);
        body_.writeUInt8(static_cast<uint8_t>(type));
        body_.writeUInt16(static_cast<uint16_t>(kStationaryPosition | (self ? kSelf : 0)));
        for (int i = 0; i < 4; ++i) body_.writeFloat(0.0f);
        writeFields(fields);
        return *this;
    }

    // A game object that carries passengers (UPDATEFLAG_TRANSPORT, with its time).
    UpdateObject& transport(uint64_t guid, const Fields& fields) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
        body_.writePackedGuid(guid);
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::ObjectType::GAMEOBJECT));
        body_.writeUInt16(kStationaryPosition | 0x0002);
        for (int i = 0; i < 4; ++i) body_.writeFloat(0.0f);
        body_.writeUInt32(0);
        writeFields(fields);
        return *this;
    }

    // A unit of the WotLK realm's walking its path (UPDATEFLAG_LIVING, with
    // MOVEMENTFLAG_SPLINE_ENABLED), as Unit::BuildMovementUpdate and
    // PacketBuilder::WriteCreate write it.
    UpdateObject& walking(uint64_t guid, const Fields& fields, const glm::vec3& at,
                          const std::vector<glm::vec3>& points, uint32_t splineFlags = 0, uint8_t mode = 0,
                          const glm::vec3& end = glm::vec3(0.0f)) {
        Spline spline;
        spline.at = at;
        spline.points = points;
        spline.flags = splineFlags;
        spline.mode = mode;
        spline.end = end;
        return onSpline(guid, fields, spline);
    }

    // The same with all it says.
    struct Spline {
        wowee::game::UpdateType type = wowee::game::UpdateType::CREATE_OBJECT;
        wowee::game::ObjectType objectType = wowee::game::ObjectType::UNIT;
        uint32_t moveFlags = 0x08000001;
        uint64_t transport = 0;  // with ONTRANSPORT, and its place there
        glm::vec3 onTransport{0.0f};
        uint8_t seat = 0xff;
        glm::vec3 at{0.0f};
        float facing = 0.5f;
        uint32_t flags = 0;
        float finalAngle = 1.0f;
        uint64_t finalTarget = 0;
        uint32_t passed = 500, duration = 600000, id = 77;
        float factor = 1.0f, nextFactor = 1.0f;
        float acceleration = 0.0f;
        uint32_t arcStart = 0;
        std::vector<glm::vec3> points;
        uint8_t mode = 0;
        glm::vec3 end{0.0f};
    };
    UpdateObject& onSpline(uint64_t guid, const Fields& fields, const Spline& s) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(s.type));
        body_.writePackedGuid(guid);
        if (s.type != wowee::game::UpdateType::MOVEMENT) body_.writeUInt8(static_cast<uint8_t>(s.objectType));
        body_.writeUInt16(s.objectType == wowee::game::ObjectType::PLAYER ? 0x0061 : 0x0060);
        body_.writeUInt32(s.moveFlags);
        body_.writeUInt16(0);
        body_.writeUInt32(1000);
        for (float f : {s.at.x, s.at.y, s.at.z, s.facing}) body_.writeFloat(f);
        if (s.moveFlags & 0x200u) {
            body_.writePackedGuid(s.transport);
            for (float f : {s.onTransport.x, s.onTransport.y, s.onTransport.z, s.facing}) body_.writeFloat(f);
            body_.writeUInt32(0);
            body_.writeUInt8(s.seat);
        }
        body_.writeUInt32(0);
        for (int i = 0; i < 9; ++i) body_.writeFloat(7.0f);
        if (s.moveFlags & 0x08000000u) {
            body_.writeUInt32(s.flags);
            if (s.flags & 0x20000u) body_.writeFloat(s.finalAngle);
            else if (s.flags & 0x10000u) body_.writeUInt64(s.finalTarget != 0 ? s.finalTarget : guid);
            else if (s.flags & 0x8000u) for (int i = 0; i < 3; ++i) body_.writeFloat(1.0f);
            for (uint32_t v : {s.passed, s.duration, s.id}) body_.writeUInt32(v);
            for (float f : {s.factor, s.nextFactor, s.acceleration}) body_.writeFloat(f);
            body_.writeUInt32(s.arcStart);
            body_.writeUInt32(static_cast<uint32_t>(s.points.size()));
            for (const auto& p : s.points) for (float f : {p.x, p.y, p.z}) body_.writeFloat(f);
            body_.writeUInt8(s.mode);
            for (float f : {s.end.x, s.end.y, s.end.z}) body_.writeFloat(f);
        }
        if (s.type != wowee::game::UpdateType::MOVEMENT) writeFields(fields);
        return *this;
    }

    // Fields changing on something already there.
    UpdateObject& values(uint64_t guid, const Fields& fields) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
        body_.writePackedGuid(guid);
        writeFields(fields);
        return *this;
    }

    // The same by index, for the fields of an array past its first.
    UpdateObject& valuesAt(uint64_t guid, const std::map<uint16_t, uint32_t>& byIndex) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
        body_.writePackedGuid(guid);
        writeIndexed(byIndex);
        return *this;
    }

    [[nodiscard]] wowee::network::Packet packet() const {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        p.writeUInt32(blocks_);
        const auto& body = body_.getData();
        p.writeBytes(body.data(), body.size());
        return p;
    }

private:
    static constexpr uint16_t kSelf = 0x0001;
    static constexpr uint16_t kStationaryPosition = 0x0040;

    void writeFields(const Fields& fields) {
        std::map<uint16_t, uint32_t> byIndex;
        for (const auto& [field, value] : fields) {
            const uint16_t index = wowee::game::fieldIndex(field);
            REQUIRE(index != 0xFFFF);
            byIndex[index] = value;
        }
        writeIndexed(byIndex);
    }

    void writeIndexed(const std::map<uint16_t, uint32_t>& byIndex) {
        const uint8_t maskBlocks =
            byIndex.empty() ? 0 : static_cast<uint8_t>(byIndex.rbegin()->first / 32 + 1);
        body_.writeUInt8(maskBlocks);
        for (uint8_t b = 0; b < maskBlocks; ++b) {
            uint32_t mask = 0;
            for (const auto& entry : byIndex)
                if (entry.first / 32 == b) mask |= 1u << (entry.first % 32);
            body_.writeUInt32(mask);
        }
        for (const auto& entry : byIndex) body_.writeUInt32(entry.second);
    }

    uint32_t blocks_ = 0;
    wowee::network::Packet body_;
};

// Keeps what the client sends instead of writing it anywhere.
class SentPackets : public wowee::network::WorldSocket {
public:
    // Not connected, as far as the client can tell, unless a test says so.
    [[nodiscard]] bool isConnected() const override { return connected; }
    bool connected = false;
    void send(const wowee::network::Packet& packet) override { sent_.push_back(packet); }

    [[nodiscard]] const std::vector<wowee::network::Packet>& all() const { return sent_; }

    // Everything sent with this opcode, oldest first.
    [[nodiscard]] std::vector<wowee::network::Packet> of(Opcode opcode) const {
        std::vector<wowee::network::Packet> found;
        for (const auto& p : sent_)
            if (p.getOpcode() == wowee::game::wireOpcode(opcode)) found.push_back(p);
        return found;
    }

private:
    std::vector<wowee::network::Packet> sent_;
};

// A client in the world as one character, ready for packets. WotLK unless
// another expansion is named.
class Session {
public:
    static constexpr uint64_t kPlayerGuid = 0x0000000000000042ull;

    // The expansion is chosen and its tables loaded the way the client does it
    // at startup, from the profiles under Data/expansions in this repository.
    // What goes by the clock goes by it as the test moves it on (core::advanceTestClock), however
    // slow the machine.
    struct HeldClock {
        HeldClock() { wowee::core::holdTestClock(true); }
        ~HeldClock() { wowee::core::holdTestClock(false); }
        HeldClock(const HeldClock&) = delete;
        HeldClock& operator=(const HeldClock&) = delete;
    };

    explicit Session(const char* expansion = "wotlk") : handler_(services_) {
        auto registry = std::make_unique<wowee::game::ExpansionRegistry>();
        registry->initialize(std::string(WOWEE_SOURCE_DIR) + "/Data");
        REQUIRE(registry->setActive(expansion));
        const std::string dir = registry->getActive()->dataPath;
        wowee::game::GameHandlerTestAccess::setExpansions(app_, std::move(registry));
        REQUIRE(handler_.getOpcodeTable().loadFromJson(dir + "/opcodes.json"));
        REQUIRE(handler_.getUpdateFieldTable().loadFromJson(dir + "/update_fields.json"));
        handler_.setPacketParsers(wowee::game::createPacketParsers(expansion));
        handler_.setPlayerGuid(kPlayerGuid);
        auto socket = std::make_unique<SentPackets>();
        sent_ = socket.get();
        wowee::game::GameHandlerTestAccess::setSocket(handler_, std::move(socket));
        wowee::game::GameHandlerTestAccess::enterWorld(handler_);
    }

    // The callbacks the tests set capture the test's own locals, which are
    // gone before the session is: a test that fails between setting one and
    // clearing it would leave the handler's teardown calling into them. They
    // are cleared here, before the handler goes.
    ~Session() {
        handler_.setAddonEventCallback({});
        handler_.setAddonEventValuesCallback({});
        handler_.setWorldEntryCallback({});
        handler_.setUnitTooltipCallback({});
        handler_.setMountCallback({});
        handler_.setVehicleSeatMovedCallback({});
        handler_.setUnitMoveFlagsCallback({});
        handler_.setUnitEffectCallback({});
        handler_.setPlacedFallCallback({});
        handler_.setNpcLineCallback({});
        handler_.setMoverEventCallback({});
        handler_.setMeleeHitCallback({});
        handler_.setFallEndsCallback({});
        handler_.setBloodSpurtCallback({});
        handler_.setUnitDrawnCallback({});
        handler_.setSoundAtUnitCallback({});
        handler_.setLogoutCompleteCallback({});
        handler_.setCreatureMoveCallback({});
        handler_.setEmoteAnimCallback({});
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    wowee::game::GameHandler& handler() { return handler_; }
    const SentPackets& sent() const { return *sent_; }
    void setConnected(bool connected) { sent_->connected = connected; }

    void receive(const wowee::network::Packet& packet) {
        wowee::game::GameHandlerTestAccess::receive(handler_, packet);
    }
    void receive(const UpdateObject& update) { receive(update.packet()); }

    // The player's own create block, as the server sends it on login.
    void spawnPlayer(const UpdateObject::Fields& fields) {
        receive(UpdateObject().create(kPlayerGuid, wowee::game::ObjectType::PLAYER, fields,
                                      true));
    }

private:
    HeldClock clock_;
    // Never initialized: it is here because the game code asks the
    // application's expansion registry which protocol it is speaking.
    wowee::core::Application app_;
    wowee::game::GameServices services_;
    wowee::game::GameHandler handler_;
    SentPackets* sent_ = nullptr;
};

}  // namespace replay
