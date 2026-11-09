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

    std::string slug() const { return owner + "/" + repo; }
};

// "name-version-release.arch.rpm" -> "name"; empty if not parseable.
std::string rpm_package_name(const std::string & filename);

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

class Store {
public:
    Store(std::filesystem::path state_root = "/var/lib/dnf5-gh",
          std::filesystem::path repo_conf_dir = "/etc/yum.repos.d",
          std::string createrepo_cmd = "createrepo_c");

    std::string repo_id(const Tracked & t) const;
    std::string legacy_repo_id(const Tracked & t) const;
    std::filesystem::path dir(const Tracked & t) const;
    std::filesystem::path repo_file(const Tracked & t) const;
    bool migrate_repo(const Tracked & t) const;
    std::unique_ptr<StoreLock> lock() const;

    std::vector<Tracked> list() const;
    std::optional<Tracked> find(const std::string & slug) const;
    void save(const Tracked & t) const;
    void remove(const Tracked & t) const;

    // Makes the local repo contain exactly `assets` (downloads missing, drops stale), regenerates
    // metadata and writes the dnf repo file. Returns the paths of the rpm files in the repo.
    std::vector<std::filesystem::path> sync(Client & client, const Tracked & t, const std::vector<Asset> & assets) const;

private:
    std::filesystem::path state_root;
    std::filesystem::path repo_conf_dir;
    std::string createrepo_cmd;
};

}  // namespace ghplugin
