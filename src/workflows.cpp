#include "workflows.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

namespace ghplugin {

InstallPlan make_install_plan(
    const std::vector<Release> & releases, const std::string & spec, const std::string & arch,
    bool include_prerelease) {
    auto [owner, repo] = parse_slug(spec);
    auto index = pick_release(releases, arch, include_prerelease);
    if (!index) {
        throw std::runtime_error(
            "no release of " + spec + " has an rpm for " + arch +
            (include_prerelease ? "" : " (try --prerelease)"));
    }

    const auto & release = releases[*index];
    return {std::move(owner), std::move(repo), release, compatible_assets(release, arch), include_prerelease};
}

InstallPlan fetch_install_plan(
    Client & client, const std::string & spec, const std::string & arch, bool include_prerelease) {
    auto [owner, repo] = parse_slug(spec);
    return make_install_plan(client.fetch_releases(owner, repo), spec, arch, include_prerelease);
}

std::vector<Asset> unique_package_assets(const std::vector<Asset> & assets) {
    std::vector<Asset> unique;
    std::set<std::string> seen;
    for (const auto & asset : assets) {
        if (asset.package_name.empty()) {
            throw std::runtime_error("missing RPM package name for " + asset.name);
        }
        if (seen.insert(asset.package_name).second) {
            unique.push_back(asset);
        }
    }
    return unique;
}

void replace_tracked_packages(Tracked & tracked, const std::vector<Asset> & assets) {
    tracked.packages.clear();
    for (const auto & asset : assets) {
        if (asset.package_name.empty()) {
            throw std::runtime_error("missing RPM package name for " + asset.name);
        }
        tracked.packages.push_back(asset.package_name);
    }
}

bool reconcile_tracked_packages(
    Tracked & tracked, const Store & store, const PackageNamesByRepo & removed_by_repo,
    const PackageNamesByRepo & remaining_by_repo, const PackageNamesByRepo & installed_by_repo) {
    std::set<std::string> removed;
    std::set<std::string> remaining;
    std::set<std::string> installed;
    for (const auto & id : {store.repo_id(tracked), store.legacy_repo_id(tracked)}) {
        if (const auto it = removed_by_repo.find(id); it != removed_by_repo.end()) {
            removed.insert(it->second.begin(), it->second.end());
        }
        if (const auto it = remaining_by_repo.find(id); it != remaining_by_repo.end()) {
            remaining.insert(it->second.begin(), it->second.end());
        }
        if (const auto it = installed_by_repo.find(id); it != installed_by_repo.end()) {
            installed.insert(it->second.begin(), it->second.end());
        }
    }

    const auto old_size = tracked.packages.size();
    std::erase_if(tracked.packages, [&](const auto & name) {
        return removed.count(name) != 0 && remaining.count(name) == 0;
    });
    bool changed = tracked.packages.size() != old_size;
    for (const auto & name : installed) {
        if (std::find(tracked.packages.begin(), tracked.packages.end(), name) == tracked.packages.end()) {
            tracked.packages.push_back(name);
            changed = true;
        }
    }
    return changed;
}

void record_release(Store & store, Tracked & tracked, const std::string & tag) {
    tracked.tag = tag;
    tracked.last_checked = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    store.save(tracked);
}

std::vector<std::filesystem::path> install_release(
    Store & store, Client & client, Tracked & tracked, const InstallPlan & plan,
    const std::vector<Asset> & selected, const std::filesystem::path & prepared) {
    if (selected.empty()) {
        throw std::invalid_argument("cannot install a release without selected rpm assets");
    }

    tracked.prerelease = plan.prerelease;
    replace_tracked_packages(tracked, selected);

    auto paths = store.sync(client, tracked, selected, prepared);
    record_release(store, tracked, plan.release.tag);
    return paths;
}

}  // namespace ghplugin
