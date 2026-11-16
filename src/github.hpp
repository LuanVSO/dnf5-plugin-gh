#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ghplugin {

struct Asset {
    std::string name;
    std::string url;
    long long size = 0;
    std::string package_name;
};

struct Release {
    std::string tag;
    std::string name;
    bool prerelease = false;
    bool draft = false;
    std::vector<Asset> assets;
};

// Parses the JSON array returned by GET /repos/{owner}/{repo}/releases.
std::vector<Release> parse_releases(const std::string & json);

// Reads environment tokens, then the private token file, then the legacy plugin config.
std::string resolve_github_token(
    const std::filesystem::path & config_file = {}, const std::filesystem::path & token_file = {});

class Client {
public:
    // `api_base` is overridable for tests.
    explicit Client(std::string api_base = "https://api.github.com");

    std::vector<Release> fetch_releases(const std::string & owner, const std::string & repo);
    void download(const std::string & url, const std::filesystem::path & dest);

private:
    std::string api_base;
    std::string token;
};

}  // namespace ghplugin
