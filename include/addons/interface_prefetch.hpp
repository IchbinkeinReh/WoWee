#pragma once

#include <string>
#include <vector>

namespace wowee::addons::interface_prefetch {

/// The key a file is kept under: its path with either slash, in any case,
/// with "." and ".." resolved - FrameXML's paths are put together from a
/// directory, a manifest's line and an include's attribute, and the same file
/// arrives spelt several ways.
[[nodiscard]] std::string key(const std::string& path);

/// Read every .lua, .xml and .toc under `roots` into memory on a thread of
/// its own, now. The interface is loaded on the first frame in the world,
/// when the terrain and model workers have the disk; on a spinning disk each
/// of its 139 FrameXML files then waited 100-300 ms for its turn (17.5 s for
/// the lot, against 1.8 s with the files cached). Read while the player is
/// still logging in, they are in memory by the time they are asked for.
void start(std::vector<std::string> roots);

/// The file's contents if the prefetch has it; false to read it from disk.
[[nodiscard]] bool take(const std::string& path, std::string& out);

/// Wait for the reading to finish.
void wait();

/// Cut the reading short and wait for the thread (shutdown).
void stop();

}  // namespace wowee::addons::interface_prefetch
