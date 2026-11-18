#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace libdnf5 {
class Base;
}

namespace ghplugin {

struct Asset {
    std::string name;
    std::string url;
    long long size = 0;
    std::string package_name;
};

struct Release {
    std::string tag;
    bool prerelease = false;
    bool draft = false;
    std::vector<Asset> assets;
};

// Parses the JSON array returned by GET /repos/{owner}/{repo}/releases.
std::vector<Release> parse_releases(const std::string & json);

// Environment tokens override sorted gh.d/*.conf drop-ins and the legacy gh.conf token.
std::string resolve_github_token(const std::vector<std::filesystem::path> & config_dirs);

class Client {
public:
    explicit Client(libdnf5::Base & base);

    std::vector<Release> fetch_releases(const std::string & owner, const std::string & repo);
    void download(const std::string & url, const std::filesystem::path & dest);

private:
    std::string token;
    libdnf5::Base & base;
};

}  // namespace ghplugin
