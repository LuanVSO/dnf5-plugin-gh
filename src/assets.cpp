#include "assets.hpp"

#include <fnmatch.h>

#include <regex>
#include <stdexcept>
#include <string_view>

namespace ghplugin {

namespace {

bool has_arch_suffix(const std::string & stem, std::string_view arch) {
    if (stem.size() <= arch.size()) {
        return false;
    }
    const auto offset = stem.size() - arch.size();
    if (stem.compare(offset, arch.size(), arch) != 0) {
        return false;
    }
    const auto separator = stem[offset - 1];
    return separator == '.' || separator == '-' || separator == '_';
}

bool is_rpm_name(const std::string & filename) {
    static const std::string ext = ".rpm";
    return filename.size() > ext.size() && filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0;
}

}  // namespace

std::string rpm_arch(const std::string & filename, const std::string & system_arch) {
    if (!is_rpm_name(filename)) {
        return {};
    }
    auto stem = filename.substr(0, filename.size() - 4);

    if (has_arch_suffix(stem, system_arch)) {
        return system_arch;
    }
    if (has_arch_suffix(stem, "noarch")) {
        return "noarch";
    }
    return {};
}

bool matches_patterns(const std::string & name, const std::vector<std::string> & patterns) {
    if (patterns.empty()) {
        return true;
    }
    for (const auto & pattern : patterns) {
        if (fnmatch(pattern.c_str(), name.c_str(), 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<Asset> compatible_assets(
    const Release & release, const std::string & arch, const std::vector<std::string> & patterns) {
    std::vector<Asset> out;
    for (const auto & a : release.assets) {
        if (!matches_patterns(a.name, patterns)) {
            continue;
        }
        if (!patterns.empty()) {
            // An explicit pattern replaces the arch and debug-package heuristics.
            if (!is_rpm_name(a.name)) {
                continue;
            }
            out.push_back(a);
            continue;
        }
        if (rpm_arch(a.name, arch).empty()) {
            continue;
        }
        if (a.name.find("-debuginfo-") != std::string::npos || a.name.find("-debugsource-") != std::string::npos) {
            continue;
        }
        out.push_back(a);
    }
    return out;
}

std::optional<size_t> pick_release(
    const std::vector<Release> & releases, const std::string & arch, bool include_prerelease,
    const std::vector<std::string> & patterns) {
    for (size_t i = 0; i < releases.size(); ++i) {
        const auto & r = releases[i];
        if (r.draft || (r.prerelease && !include_prerelease)) {
            continue;
        }
        if (!compatible_assets(r, arch, patterns).empty()) {
            return i;
        }
    }
    return std::nullopt;
}

std::pair<std::string, std::string> parse_slug(const std::string & slug) {
    static const std::regex re(R"(^([A-Za-z0-9][A-Za-z0-9-]*)/([A-Za-z0-9._-]+)$)");
    std::smatch m;
    if (!std::regex_match(slug, m, re) || m[2] == "." || m[2] == "..") {
        throw std::invalid_argument("expected <owner>/<repo>, got '" + slug + "'");
    }
    return {m[1], m[2]};
}

}  // namespace ghplugin
