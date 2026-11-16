#pragma once

#include "assets.hpp"
#include "store.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ghplugin {

struct InstallPlan {
    std::string owner;
    std::string repo;
    Release release;
    std::vector<Asset> candidates;
    bool prerelease = false;
};

InstallPlan make_install_plan(
    const std::vector<Release> & releases, const std::string & spec, const std::string & arch,
    bool include_prerelease);
InstallPlan fetch_install_plan(
    Client & client, const std::string & spec, const std::string & arch, bool include_prerelease);
std::vector<Asset> unique_package_assets(const std::vector<Asset> & assets);
void replace_tracked_packages(Tracked & tracked, const std::vector<Asset> & assets);
using PackageNamesByRepo = std::map<std::string, std::set<std::string>>;
bool reconcile_tracked_packages(
    Tracked & tracked, const Store & store, const PackageNamesByRepo & removed,
    const PackageNamesByRepo & remaining, const PackageNamesByRepo & installed);
void record_release(Store & store, Tracked & tracked, const std::string & tag);
std::vector<std::filesystem::path> install_release(
    Store & store, Client & client, Tracked & tracked, const InstallPlan & plan,
    const std::vector<Asset> & selected, const std::filesystem::path & prepared);

}  // namespace ghplugin
