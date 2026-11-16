#include "assets.hpp"

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

}  // namespace

std::string rpm_arch(const std::string & filename, const std::string & system_arch) {
    static const std::string ext = ".rpm";
    if (filename.size() <= ext.size() || filename.compare(filename.size() - ext.size(), ext.size(), ext) != 0) {
        return {};
    }
    auto stem = filename.substr(0, filename.size() - ext.size());

    if (has_arch_suffix(stem, system_arch)) {
        return system_arch;
    }
    if (has_arch_suffix(stem, "noarch")) {
        return "noarch";
    }
    return {};
}

std::vector<Asset> compatible_assets(const Release & release, const std::string & arch) {
    std::vector<Asset> out;
    for (const auto & a : release.assets) {
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
    const std::vector<Release> & releases, const std::string & arch, bool include_prerelease) {
    for (size_t i = 0; i < releases.size(); ++i) {
        const auto & r = releases[i];
        if (r.draft || (r.prerelease && !include_prerelease)) {
            continue;
        }
        if (!compatible_assets(r, arch).empty()) {
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
