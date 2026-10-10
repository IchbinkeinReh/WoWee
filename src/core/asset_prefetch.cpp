#include "core/asset_prefetch.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <cctype>

namespace wowee {
namespace core {

namespace {
// Two: the terrain workers are reading at the same time, and a spawn waits
// for a handful of files, not hundreds.
constexpr int kWorkerCount = 2;
}  // namespace

AssetPrefetch::AssetPrefetch(pipeline::AssetManager& assets) : assets_(assets) {
    for (int i = 0; i < kWorkerCount; ++i) workers_.emplace_back([this] { workerLoop(); });
}

AssetPrefetch::~AssetPrefetch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

std::string AssetPrefetch::textureKey(const std::string& path) {
    std::string key = path;
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool AssetPrefetch::ready(const std::vector<std::string>& modelPaths,
                          const std::vector<std::string>& texturePaths) {
    bool allReady = true;
    std::vector<Job> wanted;
    for (const std::string& path : modelPaths) {
        if (path.empty() || models_.count(path)) continue;
        allReady = false;
        if (modelsAsked_.insert(path).second) wanted.push_back({.isModel = true, .path = path});
    }
    for (const std::string& path : texturePaths) {
        if (path.empty()) continue;
        const std::string key = textureKey(path);
        if (texturesDone_.count(key)) continue;
        allReady = false;
        if (texturesAsked_.insert(key).second) wanted.push_back({.isModel = false, .path = key});
    }
    if (!wanted.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& job : wanted) jobs_.push_back(std::move(job));
        }
        wake_.notify_all();
    }
    return allReady;
}

void AssetPrefetch::pump() {
    ++pumpCount_;
    std::vector<Done> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished.swap(done_);
    }
    for (Done& d : finished) {
        if (d.isModel) {
            models_[d.path] = std::move(d.model);
        } else {
            if (d.texture.isValid()) {
                textures_[d.path] = std::move(d.texture);
                textureReadyAt_[d.path] = pumpCount_;
            }
            texturesDone_.insert(d.path);
        }
    }
    // A decoded texture the spawn never took (a hair sheet for a model with no
    // hair slot) would sit here for good, a megabyte or so each. One still
    // here long after it was ready goes, and is decoded again should anything
    // want it. By age rather than by count, so a crowd's worth waiting to
    // spawn is never thrown away from under it.
    constexpr uint64_t kHoldPumps = 1800;
    for (auto it = textureReadyAt_.begin(); it != textureReadyAt_.end();) {
        if (!textures_.count(it->first)) {
            it = textureReadyAt_.erase(it);
        } else if (pumpCount_ - it->second > kHoldPumps) {
            textures_.erase(it->first);
            texturesDone_.erase(it->first);
            texturesAsked_.erase(it->first);
            it = textureReadyAt_.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<const pipeline::M2Model> AssetPrefetch::model(const std::string& path) const {
    const auto it = models_.find(path);
    return it != models_.end() ? it->second : nullptr;
}

void AssetPrefetch::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done done;
        done.isModel = job.isModel;
        done.path = job.path;
        if (job.isModel) {
            auto model = std::make_shared<pipeline::M2Model>();
            if (pipeline::loadM2WithSkin(assets_, job.path, *model) && model->isValid()) {
                done.model = std::move(model);
            }
        } else {
            done.texture = assets_.loadTexture(job.path);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(done));
    }
}

}  // namespace core
}  // namespace wowee
