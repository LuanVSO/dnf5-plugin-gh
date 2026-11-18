#include "github.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>
#include <libdnf5/base/base.hpp>
#include <libdnf5/conf/config_main.hpp>
#include <libdnf5/conf/config_parser.hpp>
#include <libdnf5/plugin/utils.hpp>
#include <libdnf5/repo/file_downloader.hpp>
#include <libdnf5/utils/fs/utils.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
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

std::optional<std::string> environment_token() {
    for (const char * var : {"GITHUB_TOKEN", "GH_TOKEN"}) {
        if (const char * value = std::getenv(var); value && *value) {
            return value;
        }
    }
    return std::nullopt;
}

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

template <typename T>
void set_curl_option(CURL * handle, CURLoption option, T value) {
    auto result = curl_easy_setopt(handle, option, value);
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("cannot configure GitHub request: ") + curl_easy_strerror(result));
    }
}

void configure_network(CURL * handle, const libdnf5::ConfigMain & config) {
    const auto set_string = [handle](CURLoption option, const libdnf5::OptionString & value) {
        if (!value.empty() && !value.get_value().empty()) {
            set_curl_option(handle, option, value.get_value().c_str());
        }
    };
    if (!config.get_proxy_option().empty() && config.get_proxy_option().get_value() != "_none_") {
        set_string(CURLOPT_PROXY, config.get_proxy_option());
    }
    set_string(CURLOPT_PROXYUSERNAME, config.get_proxy_username_option());
    set_string(CURLOPT_PROXYPASSWORD, config.get_proxy_password_option());
    set_string(CURLOPT_CAINFO, config.get_sslcacert_option());
    set_string(CURLOPT_SSLCERT, config.get_sslclientcert_option());
    set_string(CURLOPT_SSLKEY, config.get_sslclientkey_option());
    set_string(CURLOPT_PROXY_CAINFO, config.get_proxy_sslcacert_option());
    set_string(CURLOPT_PROXY_SSLCERT, config.get_proxy_sslclientcert_option());
    set_string(CURLOPT_PROXY_SSLKEY, config.get_proxy_sslclientkey_option());
    const auto sslverify = config.get_sslverify_option().get_value();
    const auto proxy_sslverify = config.get_proxy_sslverify_option().get_value();
    set_curl_option(handle, CURLOPT_SSL_VERIFYPEER, sslverify ? 1L : 0L);
    set_curl_option(handle, CURLOPT_SSL_VERIFYHOST, sslverify ? 2L : 0L);
    set_curl_option(handle, CURLOPT_PROXY_SSL_VERIFYPEER, proxy_sslverify ? 1L : 0L);
    set_curl_option(handle, CURLOPT_PROXY_SSL_VERIFYHOST, proxy_sslverify ? 2L : 0L);

    long auth = 0;
    for (const auto & method : config.get_proxy_auth_method_option().get_value()) {
        if (method == "any") auth |= CURLAUTH_ANY;
        else if (method == "basic") auth |= CURLAUTH_BASIC;
        else if (method == "digest") auth |= CURLAUTH_DIGEST;
        else if (method == "digest_ie") auth |= CURLAUTH_DIGEST_IE;
        else if (method == "negotiate") auth |= CURLAUTH_NEGOTIATE;
        else if (method == "ntlm") auth |= CURLAUTH_NTLM;
        else if (method == "ntlm_wb") auth |= CURLAUTH_NTLM_WB;
        else if (method != "none") throw std::runtime_error("unsupported proxy authentication method: " + method);
    }
    set_curl_option(handle, CURLOPT_PROXYAUTH, auth);
    auto ip = config.get_ip_resolve_option().get_value();
    std::transform(ip.begin(), ip.end(), ip.begin(), [](unsigned char ch) { return std::tolower(ch); });
    const long ip_mode = ip == "4" || ip == "ipv4" ? CURL_IPRESOLVE_V4 :
                         ip == "6" || ip == "ipv6" ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_WHATEVER;
    set_curl_option(handle, CURLOPT_IPRESOLVE, ip_mode);
    const auto timeout = config.get_timeout_option().get_value();
    set_curl_option(handle, CURLOPT_CONNECTTIMEOUT, timeout > 0 ? static_cast<long>(timeout) : 0L);
    set_curl_option(handle, CURLOPT_LOW_SPEED_TIME, timeout > 0 ? static_cast<long>(timeout) : 0L);
    set_curl_option(handle, CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(config.get_minrate_option().get_value()));
}

CurlPtr make_handle(const std::string & token, curl_slist *& headers, const libdnf5::ConfigMain & config) {
    static CurlGlobal global;
    CurlPtr h(curl_easy_init());
    if (!h) {
        throw std::runtime_error("failed to initialize libcurl");
    }
    configure_network(h.get(), config);
    headers = curl_slist_append(headers, "User-Agent: dnf5-plugin-gh");
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    if (!token.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    }
    curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 1L);
    return h;
}

}  // namespace

std::string resolve_github_token(const std::vector<std::filesystem::path> & config_dirs) {
    if (auto token = environment_token()) {
        return *token;
    }

    libdnf5::ConfigParser parser;
    for (const auto & directory : config_dirs) {
        const auto path = directory / "gh.conf";
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            throw std::filesystem::filesystem_error("cannot check GitHub plugin config", path, ec);
        }
        if (!exists) {
            continue;
        }
        parser.read(path.string());
        break;
    }

    std::vector<std::filesystem::path> dropin_dirs;
    for (const auto & directory : config_dirs) {
        const auto dropins = directory / "gh.d";
        std::error_code ec;
        // DNF's file-list helper ignores directory-open errors.
        std::filesystem::directory_iterator(dropins, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            continue;
        }
        if (ec) {
            throw std::filesystem::filesystem_error("cannot read GitHub plugin drop-ins", dropins, ec);
        }
        dropin_dirs.push_back(dropins);
    }
    for (const auto & path : libdnf5::utils::fs::create_sorted_file_list(dropin_dirs, ".conf")) {
        parser.read(path.string());
    }
    return parser.has_option("main", "token") ? parser.get_value("main", "token") : std::string{};
}

Client::Client(libdnf5::Base & base) : base(base) {
    token = resolve_github_token(libdnf5::plugin::get_config_dirs(base));
}

std::vector<Release> Client::fetch_releases(const std::string & owner, const std::string & repo) {
    std::string url = "https://api.github.com/repos/" + owner + "/" + repo + "/releases?per_page=100";
    std::string body;
    curl_slist * headers = nullptr;
    auto h = make_handle(token, headers, base.get_config());
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
            "); set GITHUB_TOKEN or configure a GitHub plugin token");
    }
    if (status != 200) {
        throw std::runtime_error("GitHub API returned HTTP " + std::to_string(status));
    }
    return parse_releases(body);
}

void Client::download(const std::string & url, const std::filesystem::path & dest) {
    auto tmp = dest;
    tmp += ".part";
    try {
        libdnf5::repo::FileDownloader downloader(base);
        downloader.set_resume(false);
        downloader.add(url, tmp.string());
        downloader.download();
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (ec) {
            std::throw_with_nested(std::filesystem::filesystem_error("cannot remove failed download", tmp, ec));
        }
        throw;
    }
    std::filesystem::rename(tmp, dest);
}

}  // namespace ghplugin
