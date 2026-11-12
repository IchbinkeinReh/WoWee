#pragma once

#include "manifest_writer.hpp"

#include <string>
#include <vector>

namespace wowee {
namespace tools {

/// The client's movies are not in its archives: the installer leaves them
/// loose beside the locale's archives (Data\<locale>\Interface\Cinematics\*.avi),
/// where Storm finds them as files. Each is copied into the extracted tree
/// at its mapped path; the manifest entries for them are returned.
/// `mpqDir` is the Data folder or the client folder holding it; an empty
/// `locale` is found beside the archives.
std::vector<ManifestWriter::FileEntry> copyLooseMovies(const std::string& mpqDir, const std::string& locale,
                                                       const std::string& outputDir);

}  // namespace tools
}  // namespace wowee
