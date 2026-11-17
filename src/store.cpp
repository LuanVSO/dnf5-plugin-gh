#include "store.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <rpm/header.h>
#include <rpm/rpmio.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmts.h>

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
#include <map>
#include <set>
#include <stdexcept>

namespace ghplugin {

namespace fs = std::filesystem;

namespace {

constexpr const char * REPO_ID_PREFIX = "gh:";
constexpr const char * LEGACY_REPO_ID_PREFIX = "gh-";

}  // namespace

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

std::string rpm_header_name(const fs::path & path) {
    auto ts = rpmtsCreate();
    if (!ts) {
        throw std::runtime_error("cannot create RPM transaction");
    }
    rpmtsSetVSFlags(ts, static_cast<rpmVSFlags>(_RPMVSF_NOSIGNATURES));
    auto fd = Fopen(path.c_str(), "r.ufdio");
    if (!fd || Ferror(fd)) {
        if (fd) Fclose(fd);
        rpmtsFree(ts);
        throw std::runtime_error("cannot open RPM " + path.string());
    }
    Header header = nullptr;
    auto rc = rpmReadPackageFile(ts, fd, path.c_str(), &header);
    Fclose(fd);
    std::string name;
    if (header) {
        if (const auto * value = headerGetString(header, RPMTAG_NAME)) {
            name = value;
        }
        headerFree(header);
    }
    rpmtsFree(ts);
    if (rc != RPMRC_OK || name.empty()) {
        throw std::runtime_error("cannot read RPM package name from " + path.string());
    }
    return name;
}

bool is_github_repo_id(const std::string & repo_id) {
    return repo_id.rfind(REPO_ID_PREFIX, 0) == 0 || repo_id.rfind(LEGACY_REPO_ID_PREFIX, 0) == 0;
}

bool is_safe_rpm_name(const std::string & filename) {
    return !filename.empty() && filename[0] != '.' && filename.find('/') == std::string::npos &&
           filename.find('\n') == std::string::npos && filename.size() > 4 &&
           filename.compare(filename.size() - 4, 4, ".rpm") == 0;
}

Store::Store(fs::path state_root, fs::path repo_conf_dir, std::string createrepo_cmd, fs::path legacy_state_root)
    : state_root(std::move(state_root)),
      legacy_state_root(std::move(legacy_state_root)),
      repo_conf_dir(std::move(repo_conf_dir)),
      createrepo_cmd(std::move(createrepo_cmd)) {
    if (this->legacy_state_root.empty() && this->state_root == "/var/lib/dnf/plugins/gh") {
        this->legacy_state_root = "/var/lib/dnf5-gh";
    }
}

std::string Store::repo_id(const Tracked & t) const {
    return REPO_ID_PREFIX + t.owner + ":" + t.repo;
}

std::string Store::legacy_repo_id(const Tracked & t) const {
    return LEGACY_REPO_ID_PREFIX + t.owner + "_" + t.repo;
}

fs::path Store::dir(const Tracked & t) const {
    return state_root / (t.owner + "_" + t.repo);
}

fs::path Store::repo_file(const Tracked & t) const {
    return repo_conf_dir / (repo_id(t) + ".repo");
}

bool Store::relocate_baseurls(const Tracked & t, std::vector<std::string> & urls) const {
    if (legacy_state_root.empty()) {
        return false;
    }
    const auto old_url = "file://" + (legacy_state_root / (t.owner + "_" + t.repo)).string();
    const auto new_url = "file://" + dir(t).string();
    bool changed = false;
    for (auto & url : urls) {
        if (url == old_url || url == old_url + "/") {
            url = new_url + (url.back() == '/' ? "/" : "");
            changed = true;
        }
    }
    return changed;
}

void Store::relocate_repo_configs(const Tracked & t) const {
    relocate_repo_config(t, repo_file(t));
    relocate_repo_config(t, repo_conf_dir / (legacy_repo_id(t) + ".repo"));
}

void Store::relocate_repo_config(const Tracked & t, const fs::path & path) const {
    if (!fs::exists(path)) {
        return;
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::string line;
    std::string contents;
    bool changed = false;
    while (std::getline(in, line)) {
        auto equals = line.find('=');
        if (equals != std::string::npos) {
            auto key = line.substr(0, equals);
            auto first = key.find_first_not_of(" \t");
            auto last = key.find_last_not_of(" \t");
            if (first != std::string::npos && key.substr(first, last - first + 1) == "baseurl") {
                auto value_start = line.find_first_not_of(" \t", equals + 1);
                auto value_end = line.find_last_not_of(" \t\r");
                if (value_start != std::string::npos && value_end >= value_start) {
                    std::vector<std::string> urls{line.substr(value_start, value_end - value_start + 1)};
                    if (relocate_baseurls(t, urls)) {
                        line.replace(value_start, value_end - value_start + 1, urls.front());
                        changed = true;
                    }
                }
            }
        }
        contents += line + '\n';
    }
    if (in.bad()) {
        throw std::runtime_error("cannot read " + path.string());
    }
    if (!changed) {
        return;
    }
    auto temp = path;
    temp += ".tmp";
    std::ofstream out(temp);
    if (!out) {
        throw std::runtime_error("cannot write " + temp.string());
    }
    out << contents;
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + temp.string());
    }
    fs::permissions(temp, fs::status(path).permissions());
    fs::rename(temp, path);
}

bool Store::migrate_repo(const Tracked & t) const {
    const auto old_file = repo_conf_dir / (legacy_repo_id(t) + ".repo");
    const auto new_file = repo_file(t);
    if (fs::exists(old_file) && fs::exists(new_file)) {
        throw std::runtime_error("both old and new repository files exist for " + t.slug());
    }
    relocate_repo_configs(t);
    if (!fs::exists(old_file)) {
        return false;
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
    fs::permissions(temp, fs::status(old_file).permissions());
    fs::rename(temp, new_file);
    fs::remove(old_file);
    return true;
}

std::unique_ptr<StoreLock> Store::lock() const {
    fs::create_directories(state_root);
    auto store_lock = std::make_unique<StoreLock>(state_root / ".lock");
    migrate_state_root();
    return store_lock;
}

void Store::migrate_state_root() const {
    if (legacy_state_root.empty() || !fs::exists(legacy_state_root)) {
        return;
    }
    if (!fs::is_directory(legacy_state_root)) {
        throw std::runtime_error("legacy GitHub repository path is not a directory: " + legacy_state_root.string());
    }

    fs::create_directories(state_root.parent_path());
    if (!fs::exists(state_root)) {
        fs::rename(legacy_state_root, state_root);
        return;
    }
    if (!fs::is_directory(state_root)) {
        throw std::runtime_error("GitHub repository path is not a directory: " + state_root.string());
    }
    if (fs::is_empty(state_root)) {
        fs::remove(state_root);
        fs::rename(legacy_state_root, state_root);
        return;
    }

    for (const auto & entry : fs::recursive_directory_iterator(legacy_state_root)) {
        const auto relative = fs::relative(entry.path(), legacy_state_root);
        const auto destination = state_root / relative;
        if (!fs::exists(destination)) {
            continue;
        }
        if (entry.is_directory() && fs::is_directory(destination)) {
            continue;
        }
        if (relative == ".lock" && !entry.is_directory()) {
            continue;
        }
        throw std::runtime_error("cannot migrate GitHub repository data; both paths contain " +
                                 destination.string());
    }

    for (const auto & entry : fs::recursive_directory_iterator(legacy_state_root)) {
        const auto relative = fs::relative(entry.path(), legacy_state_root);
        const auto destination = state_root / relative;
        if (entry.is_directory()) {
            fs::create_directories(destination);
        } else if (relative != ".lock" || !fs::exists(destination)) {
            fs::rename(entry.path(), destination);
        }
    }
    fs::remove_all(legacy_state_root);
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
            t.patterns = j.value("patterns", std::vector<std::string>{});
            t.packages = j.value("packages", std::vector<std::string>{});
            t.last_checked = j.value("last_checked", std::int64_t{0});
            std::map<std::string, std::set<std::string>> aliases;
            std::set<std::string> actual_names;
            std::ifstream manifest(e.path() / "repodata" / "gh-assets.json");
            if (manifest) {
                auto assets = nlohmann::json::parse(manifest);
                for (const auto & asset : assets) {
                    auto name = asset.at("package").get<std::string>();
                    actual_names.insert(name);
                    aliases[rpm_package_name(asset.at("filename").get<std::string>())].insert(name);
                }
            }
            for (const auto & file : fs::directory_iterator(e.path())) {
                if (!file.is_regular_file() || !is_safe_rpm_name(file.path().filename().string())) {
                    continue;
                }
                auto name = rpm_header_name(file.path());
                actual_names.insert(name);
                aliases[rpm_package_name(file.path().filename().string())].insert(name);
            }
            for (auto & package : t.packages) {
                if (actual_names.count(package)) continue;
                auto it = aliases.find(package);
                if (it != aliases.end()) {
                    if (it->second.size() != 1) {
                        throw std::runtime_error("ambiguous legacy package name " + package + " for " + t.slug());
                    }
                    package = *it->second.begin();
                }
            }
            std::set<std::string> seen;
            std::erase_if(t.packages, [&](const auto & name) { return !seen.insert(name).second; });
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
        {"patterns", t.patterns}, {"packages", t.packages}, {"last_checked", t.last_checked}};
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

bool Store::has_remote_metadata(const Tracked & t) const {
    return fs::is_regular_file(dir(t) / "repodata" / "gh-assets.json");
}

void Store::clean_rpms() const {
    for (const auto & t : list()) {
        if (has_remote_metadata(t)) {
            remove_rpms(t);
        }
    }
}

void Store::remove_rpms(const Tracked & t) const {
    for (const auto & entry : fs::recursive_directory_iterator(dir(t))) {
        if (entry.path().extension() == ".rpm" && !entry.is_directory()) {
            fs::remove(entry.path());
        }
    }
}

namespace {

fs::path make_staging_dir(const fs::path & dir, const std::string & slug) {
    fs::create_directories(dir);
    auto pattern = (dir / ".staging-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char * created = mkdtemp(buffer.data());
    if (!created) {
        throw std::runtime_error("cannot create staging directory for " + slug + ": " + std::strerror(errno));
    }
    return created;
}

void run_command(const std::vector<std::string> & args) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        std::vector<char *> argv;
        for (const auto & arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::runtime_error("cannot wait for " + args[0] + ": " + std::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(args[0] + " failed (is createrepo_c installed?)");
    }
}

std::string asset_base_url(const Asset & asset) {
    const auto slash = asset.url.rfind('/');
    if (asset.url.rfind("https://", 0) != 0 || slash == std::string::npos || slash < 8) {
        throw std::runtime_error("refusing invalid asset url: " + asset.url);
    }
    const auto encoded = asset.url.substr(slash + 1);
    if (encoded.find_first_of("?#") != std::string::npos) {
        throw std::runtime_error("unsupported asset url: " + asset.url);
    }
    int length = 0;
    std::unique_ptr<char, decltype(&curl_free)> decoded(
        curl_easy_unescape(nullptr, encoded.c_str(), static_cast<int>(encoded.size()), &length), curl_free);
    if (!decoded || std::string(decoded.get(), length) != asset.name) {
        throw std::runtime_error("asset url does not match filename: " + asset.url);
    }
    return asset.url.substr(0, slash + 1);
}

}  // namespace

PreparedAssets::PreparedAssets(fs::path path) : path(std::move(path)) {}

PreparedAssets::~PreparedAssets() {
    std::error_code ec;
    fs::remove_all(path, ec);
}

std::unique_ptr<PreparedAssets> Store::prepare(
    Client & client, const Tracked & t, const std::vector<Asset> & assets) const {
    auto result = std::make_unique<PreparedAssets>(make_staging_dir(dir(t), t.slug()));
    std::set<std::string> seen;
    for (auto asset : assets) {
        if (!is_safe_rpm_name(asset.name)) {
            throw std::runtime_error("refusing unsafe asset name: " + asset.name);
        }
        if (asset.url.rfind("https://", 0) != 0) {
            throw std::runtime_error("refusing non-https asset url: " + asset.url);
        }
        if (!seen.insert(asset.name).second) {
            throw std::runtime_error("duplicate asset name: " + asset.name);
        }
        auto cached = dir(t) / asset.name;
        auto staged = result->path / asset.name;
        if (fs::exists(cached)) {
            if (asset.size > 0 && static_cast<long long>(fs::file_size(cached)) != asset.size) {
                throw std::runtime_error("existing RPM differs from release asset: " + cached.string());
            }
            fs::create_hard_link(cached, staged);
        } else {
            client.download(asset.url, staged);
            if (asset.size > 0 && static_cast<long long>(fs::file_size(staged)) != asset.size) {
                throw std::runtime_error("downloaded RPM has unexpected size: " + asset.name);
            }
        }
        asset.package_name = rpm_header_name(staged);
        result->assets.push_back(std::move(asset));
    }
    return result;
}

std::vector<fs::path> Store::sync(
    Client & client, const Tracked & t, const std::vector<Asset> & assets, const fs::path & prepared) const {
    auto d = dir(t);
    fs::create_directories(d);

    struct Staging {
        fs::path path;
        ~Staging() { std::error_code ec; fs::remove_all(path, ec); }
    } staging{make_staging_dir(d, t.slug())};

    std::set<std::string> wanted;
    std::vector<fs::path> paths;
    std::map<std::string, fs::path> groups;
    nlohmann::json manifest = nlohmann::json::array();
    for (const auto & a : assets) {
        if (!is_safe_rpm_name(a.name)) {
            throw std::runtime_error("refusing unsafe asset name: " + a.name);
        }
        const auto base_url = asset_base_url(a);
        if (!wanted.insert(a.name).second) {
            throw std::runtime_error("duplicate asset name: " + a.name);
        }
        auto p = d / a.name;
        auto staged = staging.path / a.name;
        if (!prepared.empty()) {
            fs::create_hard_link(prepared / a.name, staged);
        } else if (fs::exists(p)) {
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
        auto [group, inserted] = groups.try_emplace(base_url, staging.path / ("group-" + std::to_string(groups.size())));
        if (inserted) {
            fs::create_directories(group->second);
        }
        fs::create_hard_link(staged, group->second / a.name);
        const auto package_name = a.package_name.empty() ? rpm_header_name(staged) : a.package_name;
        manifest.push_back({{"filename", a.name}, {"package", package_name}});
        paths.push_back(p);
    }

    if (groups.empty()) {
        throw std::runtime_error("cannot publish an empty GitHub repository");
    }
    for (const auto & [url, group] : groups) {
        run_command({createrepo_cmd, "--quiet", "--no-database", "--baseurl", url, group.string()});
    }
    auto staged_metadata = staging.path / "repodata";
    if (groups.size() == 1) {
        fs::rename(groups.begin()->second / "repodata", staged_metadata);
    } else {
        std::vector<std::string> args{"mergerepo_c", "--no-database", "--all", "--outputdir", staging.path.string()};
        for (const auto & [url, group] : groups) {
            args.push_back("--repo");
            args.push_back("file://" + group.string());
        }
        run_command(args);
    }
    {
        std::ofstream out(staged_metadata / "gh-assets.json");
        out << manifest.dump(2) << '\n';
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write asset manifest for " + t.slug());
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
    if (fs::exists(metadata)) {
        if (syscall(SYS_renameat2, AT_FDCWD, metadata.c_str(), AT_FDCWD, staged_metadata.c_str(), RENAME_EXCHANGE) != 0) {
            throw std::runtime_error("cannot replace repository metadata: " + std::string(std::strerror(errno)));
        }
    } else {
        fs::rename(staged_metadata, metadata);
    }
    remove_rpms(t);
    return paths;
}

}  // namespace ghplugin
