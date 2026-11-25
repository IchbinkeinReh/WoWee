#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::addons {

struct TocFile {
    std::string addonName;
    std::string basePath;

    std::unordered_map<std::string, std::string> directives;
    std::vector<std::string> files;

    [[nodiscard]] std::string getTitle() const;
    [[nodiscard]] bool isLoadOnDemand() const;
    /// ## Interface, 0 without one.
    [[nodiscard]] uint32_t interfaceVersion() const;
    /// Whether it is loaded until the player says (## DefaultState).
    [[nodiscard]] bool enabledByDefault() const;
    /// The addons this one requires, as the client reads them (0x005f86a0):
    /// every directive whose name begins "RequiredDep" or "Dep", in any case,
    /// split at spaces and commas. Directives taken in name order.
    [[nodiscard]] std::vector<std::string> getRequiredDependencies() const;
    /// The same for "OptionalDep", "LoadWith" and "LoadManager".
    [[nodiscard]] std::vector<std::string> getOptionalDependencies() const;
    [[nodiscard]] std::vector<std::string> getLoadWith() const;
    [[nodiscard]] std::vector<std::string> getLoadManagers() const;
    [[nodiscard]] std::vector<std::string> getSavedVariables() const;
    [[nodiscard]] std::vector<std::string> getSavedVariablesPerCharacter() const;
};

/// A directive of a .toc, "Title-deDE: Probe" without its "##" (0x005f85d0): for
/// Title, Notes, Author, Version and X-... what stands behind the last "-" of
/// the name may be one of the client's nine locales. Another's is no directive
/// (an empty key); the client's goes by the name without it.
std::pair<std::string, std::string> tocDirective(const std::string& text, const std::string& locale);

/// Whether an addon stands before another in the list: by title, without regard to case (0x005f7910).
[[nodiscard]] bool listedBefore(const TocFile& a, const TocFile& b);

std::optional<TocFile> parseTocFile(const std::string& tocPath, const std::string& locale = "enUS");

} // namespace wowee::addons
