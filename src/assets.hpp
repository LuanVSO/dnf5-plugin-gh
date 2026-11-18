#pragma once

#include "github.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ghplugin {

// Returns `arch` or "noarch" when the RPM asset name has a matching architecture suffix, otherwise empty.
std::string rpm_arch(const std::string & filename, const std::string & arch);

// Binary rpm assets installable on `arch` (matching arch or noarch; no debuginfo/debugsource)
// whose file name matches `patterns`. When `patterns` is given, any .rpm asset matching one is
// accepted regardless of architecture.
std::vector<Asset> compatible_assets(
    const Release & release, const std::string & arch, const std::vector<std::string> & patterns = {});

// Newest release (API order) that has compatible assets. Drafts are always skipped;
// pre-releases only when `include_prerelease`.
std::optional<size_t> pick_release(
    const std::vector<Release> & releases, const std::string & arch, bool include_prerelease,
    const std::vector<std::string> & patterns = {});

// Splits "owner/repo"; throws std::invalid_argument when malformed.
std::pair<std::string, std::string> parse_slug(const std::string & slug);

}  // namespace ghplugin
