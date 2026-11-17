#pragma once

#include "github.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace ghplugin {

// A GitHub repository whose releases are mirrored into a local rpm repository.
struct Tracked {
    std::string owner;
    std::string repo;
    std::string tag;
    bool prerelease = false;
    std::vector<std::string> packages;  // rpm package names to follow (not file names)
    std::int64_t last_checked = 0;
    std::vector<std::string> patterns;  // glob patterns on rpm file names; empty matches all

    std::string slug() const { return owner + "/" + repo; }
};

// "name-version-release.arch.rpm" -> "name"; empty if not parseable.
std::string rpm_package_name(const std::string & filename);
std::string rpm_header_name(const std::filesystem::path & path);

// Whether a repository id uses the current or legacy GitHub plugin prefix.
bool is_github_repo_id(const std::string & repo_id);

// File name safe to create inside the repo directory (no separators, ends in .rpm).
bool is_safe_rpm_name(const std::string & filename);

class StoreLock {
public:
    explicit StoreLock(const std::filesystem::path & path);
    ~StoreLock();
    StoreLock(const StoreLock &) = delete;
    StoreLock & operator=(const StoreLock &) = delete;

private:
    int fd;
};

class PreparedAssets {
public:
    explicit PreparedAssets(std::filesystem::path path);
    ~PreparedAssets();
    PreparedAssets(const PreparedAssets &) = delete;
    PreparedAssets & operator=(const PreparedAssets &) = delete;

    std::filesystem::path path;
    std::vector<Asset> assets;
};

class Store {
public:
    Store(std::filesystem::path state_root = "/var/lib/dnf/plugins/gh",
          std::filesystem::path repo_conf_dir = "/etc/yum.repos.d",
          std::string createrepo_cmd = "createrepo_c",
          std::filesystem::path legacy_state_root = {});

    std::string repo_id(const Tracked & t) const;
    std::string legacy_repo_id(const Tracked & t) const;
    std::filesystem::path dir(const Tracked & t) const;
    std::filesystem::path repo_file(const Tracked & t) const;
    bool relocate_baseurls(const Tracked & t, std::vector<std::string> & urls) const;
    void relocate_repo_configs(const Tracked & t) const;
    bool migrate_repo(const Tracked & t) const;
    std::unique_ptr<StoreLock> lock() const;

    std::vector<Tracked> list() const;
    std::optional<Tracked> find(const std::string & slug) const;
    void save(const Tracked & t) const;
    void remove(const Tracked & t) const;
    std::unique_ptr<PreparedAssets> prepare(Client & client, const Tracked & t, const std::vector<Asset> & assets) const;
    bool has_remote_metadata(const Tracked & t) const;
    void remove_rpms(const Tracked & t) const;

    // Publishes metadata referring to release assets and returns their repository locations.
    std::vector<std::filesystem::path> sync(
        Client & client, const Tracked & t, const std::vector<Asset> & assets,
        const std::filesystem::path & prepared = {}) const;

private:
    void migrate_state_root() const;
    void relocate_repo_config(const Tracked & t, const std::filesystem::path & path) const;

    std::filesystem::path state_root;
    std::filesystem::path legacy_state_root;
    std::filesystem::path repo_conf_dir;
    std::string createrepo_cmd;
};

}  // namespace ghplugin
