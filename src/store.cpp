#include "store.hpp"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace ghplugin {

namespace fs = std::filesystem;

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
    return "gh-" + t.owner + "_" + t.repo;
}

fs::path Store::dir(const Tracked & t) const {
    return state_root / (t.owner + "_" + t.repo);
}

fs::path Store::repo_file(const Tracked & t) const {
    return repo_conf_dir / (repo_id(t) + ".repo");
}

std::vector<Tracked> Store::list() const {
    std::vector<Tracked> out;
    std::error_code ec;
    for (const auto & e : fs::directory_iterator(state_root, ec)) {
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
            out.push_back(std::move(t));
        } catch (const nlohmann::json::exception &) {
            continue;
        }
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
        {"owner", t.owner}, {"repo", t.repo}, {"tag", t.tag}, {"prerelease", t.prerelease}, {"packages", t.packages}};
    std::ofstream out(dir(t) / "state.json");
    out << j.dump(2) << "\n";
    if (!out) {
        throw std::runtime_error("cannot write state for " + t.slug());
    }
}

void Store::remove(const Tracked & t) const {
    fs::remove(repo_file(t));
    fs::remove_all(dir(t));
}

namespace {

void run_createrepo(const std::string & cmd, const fs::path & dir) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        execlp(cmd.c_str(), cmd.c_str(), "--quiet", "--no-database", dir.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(cmd + " failed (is createrepo_c installed?)");
    }
}

}  // namespace

std::vector<fs::path> Store::sync(Client & client, const Tracked & t, const std::vector<Asset> & assets) const {
    auto d = dir(t);
    fs::create_directories(d);

    std::set<std::string> wanted;
    std::vector<fs::path> paths;
    for (const auto & a : assets) {
        if (!is_safe_rpm_name(a.name)) {
            throw std::runtime_error("refusing unsafe asset name: " + a.name);
        }
        if (a.url.rfind("https://", 0) != 0) {
            throw std::runtime_error("refusing non-https asset url: " + a.url);
        }
        wanted.insert(a.name);
        auto p = d / a.name;
        if (!fs::exists(p) || (a.size > 0 && static_cast<long long>(fs::file_size(p)) != a.size)) {
            client.download(a.url, p);
        }
        paths.push_back(p);
    }
    for (const auto & e : fs::directory_iterator(d)) {
        if (e.path().extension() == ".rpm" && !wanted.count(e.path().filename().string())) {
            fs::remove(e.path());
        }
    }

    run_createrepo(createrepo_cmd, d);

    std::ofstream out(repo_file(t));
    out << "[" << repo_id(t) << "]\n"
        << "name=GitHub releases: " << t.slug() << "\n"
        << "baseurl=file://" << d.string() << "\n"
        << "enabled=1\n"
        << "gpgcheck=0\n"
        << "metadata_expire=never\n";
    if (!out) {
        throw std::runtime_error("cannot write " + repo_file(t).string());
    }
    return paths;
}

}  // namespace ghplugin
