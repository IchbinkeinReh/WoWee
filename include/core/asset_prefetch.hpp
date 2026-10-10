#pragma once

#include "pipeline/blp_loader.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
struct M2Model;
}  // namespace pipeline
namespace core {

/// Item models and textures read and decoded off the main thread ahead of the
/// unit that wears them.
///
/// An NPC's helm, its shoulders and the hair and skin it is coloured with were
/// read, parsed and decoded on the main thread at the moment it spawned - 20
/// to 130 ms a spawn, every time a crowd came into range. The spawn queue asks
/// ready() first and holds the unit back until everything it will wear has
/// been prepared here, so it still appears whole, only a few frames later.
///
/// Main thread only, apart from the two workers it owns.
class AssetPrefetch {
public:
    explicit AssetPrefetch(pipeline::AssetManager& assets);
    ~AssetPrefetch();
    AssetPrefetch(const AssetPrefetch&) = delete;
    AssetPrefetch& operator=(const AssetPrefetch&) = delete;

    /// Whether every one of these has been prepared - or found missing, which
    /// is as final. The ones never asked for are queued for the workers.
    bool ready(const std::vector<std::string>& modelPaths, const std::vector<std::string>& texturePaths);

    /// Takes in what the workers have finished. Once a frame, before ready().
    void pump();

    /// The parsed model, or null for one not prepared or that failed to load.
    [[nodiscard]] std::shared_ptr<const pipeline::M2Model> model(const std::string& path) const;

    /// Decoded textures, keyed as CharacterRenderer looks them up. It takes
    /// each one as it uploads it; pass this to setPredecodedBLPCache around
    /// the spawn.
    std::unordered_map<std::string, pipeline::BLPImage>& textures() { return textures_; }

    /// The key a texture is found under: back slashes, lower case.
    [[nodiscard]] static std::string textureKey(const std::string& path);

private:
    struct Job {
        bool isModel = false;
        std::string path;
    };
    struct Done {
        bool isModel = false;
        std::string path;
        std::shared_ptr<const pipeline::M2Model> model;
        pipeline::BLPImage texture;
    };
    void workerLoop();

    pipeline::AssetManager& assets_;
    // Main-thread state.
    std::unordered_map<std::string, std::shared_ptr<const pipeline::M2Model>> models_;  // null: failed
    std::unordered_set<std::string> modelsAsked_;
    std::unordered_map<std::string, pipeline::BLPImage> textures_;
    std::unordered_set<std::string> texturesAsked_;
    std::unordered_set<std::string> texturesDone_;
    std::unordered_map<std::string, uint64_t> textureReadyAt_;  // pump it arrived on
    uint64_t pumpCount_ = 0;
    // Shared with the workers.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace core
}  // namespace wowee
