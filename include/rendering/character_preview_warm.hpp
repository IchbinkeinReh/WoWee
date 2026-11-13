#pragma once

/// The files the character select screen's preview of one character will
/// read, read once on a worker thread first.
///
/// In the streaming data mode a file that is not in the local cache is
/// fetched from the server while its reader waits. The preview is built on
/// the main thread (it uploads to the GPU as it goes), so on an empty cache
/// the screen stood still for about four seconds while the character, its
/// racial backdrop and its gear came over the network. The original client
/// does not stop drawing for its models either. With this the main thread
/// asks the worker to read what the preview needs, keeps drawing, and builds
/// the preview from the cache once the worker is done.
///
/// A file this list misses is still read when the preview asks for it, so a
/// gap costs only the wait it always did. Safe on any thread: the asset
/// manager may be read from several.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/character.hpp"
#include "game/guild_emblem.hpp"

namespace wowee {
namespace pipeline {
class AssetManager;
}
namespace rendering {

struct PreviewWarmCharacter {
    game::Race race = game::Race::HUMAN;
    game::Gender gender = game::Gender::MALE;
    bool useFemaleModel = false;
    uint8_t classId = 0;
    uint32_t appearanceBytes = 0;
    uint8_t facialFeatures = 0;
    uint32_t flags = 0;
    std::vector<game::EquipmentItem> equipment;
    std::optional<game::GuildEmblem> emblem;
};

/// Reads the player model with its animations, its CharSections textures, the
/// racial backdrop and the gear the select screen draws. Returns how many
/// files were read.
size_t warmCharacterPreviewFiles(pipeline::AssetManager& assets, const PreviewWarmCharacter& who);

}  // namespace rendering
}  // namespace wowee
