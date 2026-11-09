#include "store.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <linux/fs.h>
#include <set>
#include <stdexcept>

namespace ghplugin {

namespace fs = std::filesystem;

StoreLock::StoreLock(const fs::path & path) : fd(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
    if (fd < 0) {
        throw std::runtime_error("cannot open GitHub repository lock: " + std::string(std::strerror(errno)));
    }
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            const auto message = std::string(std::strerror(errno));
            close(fd);
            throw std::runtime_error("cannot acquire GitHub repository lock: " + message);
        }
    }
}

StoreLock::~StoreLock() {
    close(fd);
}

std::string rpm_package_name(const std::string & filename) {
    if (filename.size() < 5 || filename.compare(filename.size() - 4, 4, ".rpm") != 0) {
        return {};
    }
    auto stem = filename.substr(0, filename.size() - 4);
    auto dot = stem.rfind('.');
    if (dot == std::string::npos) {
        return {};
    }
    stem.resize(dot);  // drop arch
    auto rel = stem.rfind('-');
    if (rel == std::string::npos || rel == 0) {
        return {};
    }
    auto ver = stem.rfind('-', rel - 1);
    if (ver == std::string::npos || ver == 0) {
        return {};
    }
    return stem.substr(0, ver);
}

bool is_safe_rpm_name(const std::string & filename) {
    return !filename.empty() && filename[0] != '.' && filename.find('/') == std::string::npos &&
           filename.find('\n') == std::string::npos && filename.size() > 4 &&
           filename.compare(filename.size() - 4, 4, ".rpm") == 0;
}

Store::Store(fs::path state_root, fs::path repo_conf_dir, std::string createrepo_cmd)
    : state_root(std::move(state_root)),
      repo_conf_dir(std::move(repo_conf_dir)),
      createrepo_cmd(std::move(createrepo_cmd)) {}

std::string Store::repo_id(const Tracked & t) const {
    return "gh:" + t.owner + ":" + t.repo;
}

std::string Store::legacy_repo_id(const Tracked & t) const {
    return "gh-" + t.owner + "_" + t.repo;
}

fs::path Store::dir(const Tracked & t) const {
    return state_root / (t.owner + "_" + t.repo);
}

fs::path Store::repo_file(const Tracked & t) const {
    return repo_conf_dir / (repo_id(t) + ".repo");
}

bool Store::migrate_repo(const Tracked & t) const {
    const auto old_file = repo_conf_dir / (legacy_repo_id(t) + ".repo");
    const auto new_file = repo_file(t);
    if (!fs::exists(old_file)) {
        return false;
    }
    if (fs::exists(new_file)) {
        throw std::runtime_error("both old and new repository files exist for " + t.slug());
    }
    std::ifstream in(old_file);
    if (!in) {
        throw std::runtime_error("cannot read " + old_file.string());
    }
    auto temp = new_file;
    temp += ".tmp";
    std::ofstream out(temp);
    if (!out) {
        throw std::runtime_error("cannot write " + temp.string());
    }
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line == "[" + legacy_repo_id(t) + "]") {
            line = "[" + repo_id(t) + "]";
            found = true;
        }
        out << line << '\n';
    }
    if (in.bad() || !found) {
        throw std::runtime_error("cannot migrate repository configuration " + old_file.string());
    }
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + temp.string());
    }
    fs::rename(temp, new_file);
    fs::remove(old_file);
    return true;
}

std::unique_ptr<StoreLock> Store::lock() const {
    fs::create_directories(state_root);
    return std::make_unique<StoreLock>(state_root / ".lock");
}

std::vector<Tracked> Store::list() const {
    std::vector<Tracked> out;
    std::error_code ec;
    for (const auto & e : fs::directory_iterator(state_root, ec)) {
        if (!e.is_directory()) {
            continue;
        }
        std::ifstream in(e.path() / "state.json");
        if (!in) {
            continue;
        }
        try {
            auto j = nlohmann::json::parse(in);
            Tracked t;
            t.owner = j.at("owner");
            t.repo = j.at("repo");
            t.tag = j.value("tag", "");
            t.prerelease = j.value("prerelease", false);
            t.packages = j.value("packages", std::vector<std::string>{});
            t.last_checked = j.value("last_checked", std::int64_t{0});
            t.remote_packages = j.value("remote_packages", false);
            out.push_back(std::move(t));
        } catch (const nlohmann::json::exception & ex) {
            throw std::runtime_error("invalid tracking state in " + e.path().string() + ": " + ex.what());
        }
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("cannot read tracking directory " + state_root.string() + ": " + ec.message());
    }
    std::sort(out.begin(), out.end(), [](const Tracked & a, const Tracked & b) { return a.slug() < b.slug(); });
    return out;
}

std::optional<Tracked> Store::find(const std::string & slug) const {
    for (auto & t : list()) {
        if (t.slug() == slug) {
            return t;
        }
    }
    return std::nullopt;
}

void Store::save(const Tracked & t) const {
    fs::create_directories(dir(t));
    nlohmann::json j{
        {"owner", t.owner}, {"repo", t.repo}, {"tag", t.tag}, {"prerelease", t.prerelease},
        {"packages", t.packages}, {"last_checked", t.last_checked}, {"remote_packages", t.remote_packages}};
    auto temporary = dir(t) / "state.json.tmp";
    std::ofstream out(temporary);
    out << j.dump(2) << "\n";
    if (!out) {
        throw std::runtime_error("cannot write state for " + t.slug());
    }
    out.close();
    if (!out) {
        throw std::runtime_error("cannot close state for " + t.slug());
    }
    fs::rename(temporary, dir(t) / "state.json");
}

void Store::remove(const Tracked & t) const {
    fs::remove(repo_file(t));
    fs::remove(repo_conf_dir / (legacy_repo_id(t) + ".repo"));
    fs::remove_all(dir(t));
}

void Store::remove_rpms(const Tracked & t) const {
    for (const auto & entry : fs::directory_iterator(dir(t))) {
        if (is_safe_rpm_name(entry.path().filename().string()) && entry.is_regular_file()) {
            fs::remove(entry.path());
        }
    }
}

namespace {

std::string asset_base_url(const std::vector<Asset> & assets) {
    std::string base;
    for (const auto & asset : assets) {
        const auto slash = asset.url.rfind('/');
        if (asset.url.rfind("https://", 0) != 0 || slash == std::string::npos || slash < 8 ||
            asset.url.find_first_of("?#") != std::string::npos) {
            throw std::runtime_error("unsupported release asset URL: " + asset.url);
        }
        const auto encoded_name = asset.url.substr(slash + 1);
        int length = 0;
        std::unique_ptr<char, decltype(&curl_free)> decoded(
            curl_easy_unescape(nullptr, encoded_name.c_str(), static_cast<int>(encoded_name.size()), &length), curl_free);
        if (!decoded || std::string(decoded.get(), length) != asset.name) {
            throw std::runtime_error("release asset URL does not match its filename: " + asset.name);
        }
        const auto candidate = asset.url.substr(0, slash + 1);
        if (!base.empty() && base != candidate) {
            throw std::runtime_error("release asset URLs do not share a download directory");
        }
        base = candidate;
    }
    if (base.empty()) {
        throw std::runtime_error("cannot create a repository without release assets");
    }
    return base;
}

void run_createrepo(const std::string & cmd, const fs::path & dir, const std::string & base_url) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        execlp(cmd.c_str(), cmd.c_str(), "--quiet", "--no-database", "--baseurl", base_url.c_str(),
               dir.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::runtime_error("cannot wait for " + cmd + ": " + std::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(cmd + " failed (is createrepo_c installed?)");
    }
}

}  // namespace

std::vector<fs::path> Store::sync(Client & client, const Tracked & t, const std::vector<Asset> & assets) const {
    const auto base_url = asset_base_url(assets);
    auto d = dir(t);
    fs::create_directories(d);

    auto pattern = (d / ".staging-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char * created = mkdtemp(buffer.data());
    if (!created) {
        throw std::runtime_error("cannot create staging directory for " + t.slug() + ": " + std::strerror(errno));
    }
    struct Staging {
        fs::path path;
        ~Staging() { std::error_code ec; fs::remove_all(path, ec); }
    } staging{created};

    std::set<std::string> wanted;
    std::vector<fs::path> paths;
    for (const auto & a : assets) {
        if (!is_safe_rpm_name(a.name)) {
            throw std::runtime_error("refusing unsafe asset name: " + a.name);
        }
        if (a.url.rfind("https://", 0) != 0) {
            throw std::runtime_error("refusing non-https asset url: " + a.url);
        }
        if (!wanted.insert(a.name).second) {
            throw std::runtime_error("duplicate asset name: " + a.name);
        }
        auto p = d / a.name;
        auto staged = staging.path / a.name;
        if (fs::exists(p)) {
            if (a.size > 0 && static_cast<long long>(fs::file_size(p)) != a.size) {
                throw std::runtime_error("existing RPM differs from release asset: " + p.string());
            }
            fs::create_hard_link(p, staged);
        } else {
            client.download(a.url, staged);
            if (a.size > 0 && static_cast<long long>(fs::file_size(staged)) != a.size) {
                throw std::runtime_error("downloaded RPM has unexpected size: " + a.name);
            }
        }
        paths.push_back(p);
    }

    run_createrepo(createrepo_cmd, staging.path, base_url);
    for (const auto & path : paths) {
        if (!fs::exists(path)) {
            fs::create_hard_link(staging.path / path.filename(), path);
        }
    }

    if (!fs::exists(repo_file(t))) {
        auto temporary_repo = repo_file(t);
        temporary_repo += ".tmp";
        std::ofstream out(temporary_repo);
        out << "[" << repo_id(t) << "]\n"
            << "name=GitHub releases: " << t.slug() << "\n"
            << "baseurl=file://" << d.string() << "\n"
            << "enabled=1\n"
            << "gpgcheck=0\n"
            << "metadata_expire=0\n";
        if (!out) {
            throw std::runtime_error("cannot write " + temporary_repo.string());
        }
        out.close();
        if (!out) {
            throw std::runtime_error("cannot close " + temporary_repo.string());
        }
        fs::rename(temporary_repo, repo_file(t));
    }
    auto metadata = d / "repodata";
    auto staged_metadata = staging.path / "repodata";
    if (fs::exists(metadata)) {
        if (syscall(SYS_renameat2, AT_FDCWD, metadata.c_str(), AT_FDCWD, staged_metadata.c_str(), RENAME_EXCHANGE) != 0) {
            throw std::runtime_error("cannot replace repository metadata: " + std::string(std::strerror(errno)));
        }
    } else {
        fs::rename(staged_metadata, metadata);
    }
    return paths;
}

}  // namespace ghplugin
