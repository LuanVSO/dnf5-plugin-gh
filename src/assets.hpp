#pragma once

#include "github.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ghplugin {

// Architecture of the running system, as used in RPM file names (e.g. x86_64).
std::string system_arch();

// Returns the arch field of an "<name>-<ver>-<rel>.<arch>.rpm" file name, or empty if not an rpm.
std::string rpm_arch(const std::string & filename);

// Binary rpm assets installable on `arch` (matching arch or noarch; no debuginfo/debugsource).
std::vector<Asset> compatible_assets(const Release & release, const std::string & arch);

// Newest release (API order) that has compatible assets. Drafts are always skipped;
// pre-releases only when `include_prerelease`.
std::optional<size_t> pick_release(
    const std::vector<Release> & releases, const std::string & arch, bool include_prerelease);

// Splits "owner/repo"; throws std::invalid_argument when malformed.
std::pair<std::string, std::string> parse_slug(const std::string & slug);

}  // namespace ghplugin
