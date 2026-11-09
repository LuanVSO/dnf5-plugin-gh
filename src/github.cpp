#include "github.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace ghplugin {

std::vector<Release> parse_releases(const std::string & json) {
    auto doc = nlohmann::json::parse(json);
    if (!doc.is_array()) {
        throw std::runtime_error("unexpected GitHub API response: " + doc.value("message", std::string("not an array")));
    }
    std::vector<Release> out;
    for (const auto & r : doc) {
        Release rel;
        rel.tag = r.value("tag_name", "");
        rel.name = r.value("name", "");
        rel.prerelease = r.value("prerelease", false);
        rel.draft = r.value("draft", false);
        for (const auto & a : r.value("assets", nlohmann::json::array())) {
            rel.assets.push_back({a.value("name", ""), a.value("browser_download_url", ""), a.value("size", 0LL)});
        }
        out.push_back(std::move(rel));
    }
    return out;
}

namespace {

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

struct CurlDeleter {
    void operator()(CURL * c) const { curl_easy_cleanup(c); }
};
using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;

size_t write_string(char * ptr, size_t size, size_t nmemb, void * userdata) {
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_file(char * ptr, size_t size, size_t nmemb, void * userdata) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata));
}

CurlPtr make_handle(const std::string & token, curl_slist *& headers, bool api) {
    static CurlGlobal global;
    CurlPtr h(curl_easy_init());
    if (!h) {
        throw std::runtime_error("failed to initialize libcurl");
    }
    headers = curl_slist_append(headers, "User-Agent: dnf5-plugin-gh");
    if (api) {
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
        if (!token.empty()) {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
        }
    }
    curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_TIME, 30L);
    return h;
}

}  // namespace

Client::Client(std::string api_base) : api_base(std::move(api_base)) {
    for (const char * var : {"GITHUB_TOKEN", "GH_TOKEN"}) {
        if (const char * v = std::getenv(var); v && *v) {
            token = v;
            break;
        }
    }
}

std::vector<Release> Client::fetch_releases(const std::string & owner, const std::string & repo) {
    std::string url = api_base + "/repos/" + owner + "/" + repo + "/releases?per_page=100";
    std::string body;
    curl_slist * headers = nullptr;
    auto h = make_handle(token, headers, true);
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(h.get());
    long status = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("request to GitHub failed: ") + curl_easy_strerror(rc));
    }
    if (status == 404) {
        throw std::runtime_error("repository " + owner + "/" + repo + " not found (or no access)");
    }
    if (status == 403 || status == 429) {
        throw std::runtime_error(
            "GitHub API rate limit or access denied (HTTP " + std::to_string(status) +
            "); set GITHUB_TOKEN (use sudo -E to keep it)");
    }
    if (status != 200) {
        throw std::runtime_error("GitHub API returned HTTP " + std::to_string(status));
    }
    return parse_releases(body);
}

void Client::download(const std::string & url, const std::filesystem::path & dest) {
    auto tmp = dest;
    tmp += ".part";
    std::unique_ptr<FILE, int (*)(FILE *)> f(std::fopen(tmp.c_str(), "wb"), std::fclose);
    if (!f) {
        throw std::runtime_error("cannot write " + tmp.string());
    }
    curl_slist * headers = nullptr;
    auto h = make_handle(token, headers, false);
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, f.get());
    CURLcode rc = curl_easy_perform(h.get());
    long status = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    f.reset();
    if (rc != CURLE_OK || status != 200) {
        std::filesystem::remove(tmp);
        throw std::runtime_error(
            "download of " + url +
            " failed: " + (rc != CURLE_OK ? std::string(curl_easy_strerror(rc)) : "HTTP " + std::to_string(status)));
    }
    std::filesystem::rename(tmp, dest);
}

}  // namespace ghplugin
