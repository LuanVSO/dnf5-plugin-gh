#include "assets.hpp"

#include <sys/utsname.h>

#include <regex>
#include <stdexcept>

namespace ghplugin {

std::string system_arch() {
    utsname u{};
    if (uname(&u) != 0) {
        return "x86_64";
    }
    return u.machine;
}

std::string rpm_arch(const std::string & filename) {
    static const std::string ext = ".rpm";
    if (filename.size() <= ext.size() || filename.compare(filename.size() - ext.size(), ext.size(), ext) != 0) {
        return {};
    }
    auto stem = filename.substr(0, filename.size() - ext.size());
    auto dot = stem.rfind('.');
    if (dot == std::string::npos || dot + 1 == stem.size()) {
        return {};
    }
    return stem.substr(dot + 1);
}

std::vector<Asset> compatible_assets(const Release & release, const std::string & arch) {
    std::vector<Asset> out;
    for (const auto & a : release.assets) {
        auto a_arch = rpm_arch(a.name);
        if (a_arch.empty() || (a_arch != arch && a_arch != "noarch")) {
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
