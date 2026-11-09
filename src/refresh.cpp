#include "refresh.hpp"
#include "assets.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace ghplugin {

bool refresh_due(const Tracked & tracked, std::int64_t now) {
    return tracked.last_checked <= 0 || tracked.last_checked > now ||
           now - tracked.last_checked >= REFRESH_INTERVAL;
}

std::string refresh_repository(Store & store, Client & client, Tracked & tracked, const std::string & arch) {
    store.migrate_repo(tracked);
    auto releases = client.fetch_releases(tracked.owner, tracked.repo);
    auto index = pick_release(releases, arch, tracked.prerelease);
    if (!index) {
        throw std::runtime_error("no release with an rpm for " + arch);
    }
    const auto & release = releases[*index];
    std::vector<Asset> assets;
    std::set<std::string> found;
    for (const auto & asset : compatible_assets(release, arch)) {
        auto name = rpm_package_name(asset.name);
        if (std::find(tracked.packages.begin(), tracked.packages.end(), name) != tracked.packages.end()) {
            assets.push_back(asset);
            found.insert(name);
        }
    }
    for (const auto & package : tracked.packages) {
        if (!found.count(package)) {
            throw std::runtime_error("release " + release.tag + " lacks tracked package " + package);
        }
    }
    if (assets.empty()) {
        throw std::runtime_error("release " + release.tag + " lacks the tracked packages");
    }
    auto old = tracked.tag;
    if (release.tag != old) {
        store.sync(client, tracked, assets);
        tracked.tag = release.tag;
    }
    tracked.last_checked = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    store.save(tracked);
    return release.tag == old ? tracked.slug() + ": up to date (" + old + ")"
                              : tracked.slug() + ": " + old + " -> " + release.tag;
}

}  // namespace ghplugin
