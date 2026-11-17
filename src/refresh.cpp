#include "refresh.hpp"
#include "assets.hpp"
#include "workflows.hpp"

#include <algorithm>
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
    auto index = pick_release(releases, arch, tracked.prerelease, tracked.patterns);
    if (!index) {
        throw std::runtime_error("no release with an rpm for " + arch);
    }
    const auto & release = releases[*index];
    auto old = tracked.tag;
    auto prepared = store.prepare(client, tracked, compatible_assets(release, arch, tracked.patterns));
    std::vector<Asset> assets;
    std::set<std::string> found;
    for (const auto & asset : unique_package_assets(prepared->assets)) {
        const auto & name = asset.package_name;
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
    if (release.tag != old || !store.has_remote_metadata(tracked)) {
        store.sync(client, tracked, assets, prepared->path());
    }
    record_release(store, tracked, release.tag);
    return release.tag == old ? tracked.slug() + ": up to date (" + old + ")"
                              : tracked.slug() + ": " + old + " -> " + release.tag;
}

}  // namespace ghplugin
