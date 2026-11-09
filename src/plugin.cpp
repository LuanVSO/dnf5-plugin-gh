#include "assets.hpp"
#include "github.hpp"
#include "refresh.hpp"
#include "store.hpp"

#include <dnf5/context.hpp>
#include <dnf5/iplugin.hpp>
#include <dnf5/shared_options.hpp>
#include <libdnf5-cli/session.hpp>
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/rpm/package_query.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace ghplugin;
using ArgPositional = libdnf5::cli::ArgumentParser::PositionalArg;

namespace {

std::exception_ptr last_exception;

constexpr dnf5::PluginVersion PLUGIN_VERSION{.major = 0, .minor = 1, .micro = 0};
constexpr const char * attrs[]{"author.name", "author.email", "description", nullptr};
constexpr const char * attrs_value[]{"dnf5-plugin-gh contributors", "", "Install and update RPMs from GitHub releases"};

class GhCommand : public dnf5::Command {
public:
    using dnf5::Command::Command;

    void set_parent_command() override {
        auto * root = get_session().get_argument_parser().get_root_command();
        root->register_command(get_argument_parser_command());
    }

protected:
    Store store;

    void require_root() const {
        if (geteuid() != 0) {
            throw std::runtime_error("this command must be run as root");
        }
    }

    void say(const std::string & msg) { get_context().print_info(msg); }

    std::vector<Tracked> resolve_targets(const std::vector<std::string> & slugs) {
        if (slugs.empty()) {
            return store.list();
        }
        std::vector<Tracked> out;
        for (const auto & s : slugs) {
            auto [owner, repo] = parse_slug(s);
            auto t = store.find(owner + "/" + repo);
            if (!t) {
                throw std::runtime_error(s + " is not tracked (see gh-info)");
            }
            out.push_back(*t);
        }
        return out;
    }
};

class GhInstallCommand : public GhCommand {
public:
    explicit GhInstallCommand(dnf5::Context & ctx) : GhCommand(ctx, "gh-install") {}

    void set_argument_parser() override {
        auto & ctx = get_context();
        get_argument_parser_command()->set_description("Install RPMs from the latest GitHub release of <owner>/<repo>");
        specs = std::make_unique<libdnf5::cli::session::StringArgumentList>(
            *this, "owner/repo", "GitHub repositories to install from", ArgPositional::AT_LEAST_ONE);
        prerelease = std::make_unique<libdnf5::cli::session::BoolOption>(
            *this, "prerelease", '\0', "Consider pre-releases", false);
        all = std::make_unique<libdnf5::cli::session::BoolOption>(
            *this, "all", '\0', "Install every matching RPM asset without prompting", false);
        // dnf5 identifies transactional commands by the presence of its store option.
        dnf5::create_store_option(*this);
        (void)ctx;
    }

    void configure() override {
        require_root();
        auto & ctx = get_context();
        ctx.set_load_system_repo(true);
        ctx.set_load_available_repos(dnf5::Context::LoadAvailableRepos::ENABLED);
        lock = store.lock();
        Client client;
        auto arch = system_arch();
        for (const auto & spec : specs->get_value()) {
            auto [owner, repo] = parse_slug(spec);
            auto releases = client.fetch_releases(owner, repo);
            auto idx = pick_release(releases, arch, prerelease->get_value());
            if (!idx) {
                throw std::runtime_error(
                    "no release of " + spec + " has an rpm for " + arch +
                    (prerelease->get_value() ? "" : " (try --prerelease)"));
            }
            const auto & rel = releases[*idx];
            auto chosen = choose(spec, compatible_assets(rel, arch));

            Tracked t = store.find(owner + "/" + repo).value_or(Tracked{owner, repo});
            t.tag = rel.tag;
            t.prerelease = prerelease->get_value();
            t.packages.clear();
            for (const auto & a : chosen) {
                t.packages.push_back(rpm_package_name(a.name));
            }
            say("Using " + spec + " " + rel.tag);
            store.migrate_repo(t);
            libdnf5::repo::RepoQuery old_repos(ctx.get_base());
            old_repos.filter_id(store.legacy_repo_id(t));
            for (auto & repo : old_repos) {
                repo->disable();
            }
            auto paths = store.sync(client, t, chosen);
            t.last_checked = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            store.save(t);
            libdnf5::repo::RepoQuery repos(ctx.get_base());
            repos.filter_id(store.repo_id(t));
            if (repos.empty()) {
                ctx.get_base().get_repo_sack()->create_repos_from_file(store.repo_file(t).string());
                repos = libdnf5::repo::RepoQuery(ctx.get_base());
                repos.filter_id(store.repo_id(t));
            }
            if (repos.empty()) {
                throw std::runtime_error("cannot load repository for " + spec);
            }
            for (auto & repo : repos) {
                repo->enable();
                repo->expire();
            }
            for (const auto & p : paths) {
                selected.emplace_back(store.repo_id(t), p.filename().string());
            }
        }
    }

    void run() override {
        auto * goal = get_context().get_goal();
        for (const auto & [repo_id, filename] : selected) {
            libdnf5::rpm::PackageQuery packages(get_context().get_base());
            packages.filter_repo_id(repo_id);
            packages.filter_location(filename);
            if (packages.size() != 1) {
                throw std::runtime_error("expected one package for " + filename + " in " + repo_id);
            }
            goal->add_rpm_install(*packages.begin());
        }
    }

private:
    std::unique_ptr<libdnf5::cli::session::StringArgumentList> specs;
    std::unique_ptr<libdnf5::cli::session::BoolOption> prerelease;
    std::unique_ptr<libdnf5::cli::session::BoolOption> all;
    std::vector<std::pair<std::string, std::string>> selected;
    std::unique_ptr<StoreLock> lock;

    // Reduces the candidate assets to the ones to install, one per package name.
    std::vector<Asset> choose(const std::string & spec, const std::vector<Asset> & candidates) {
        std::vector<Asset> unique;
        std::set<std::string> seen;
        for (const auto & a : candidates) {
            if (seen.insert(rpm_package_name(a.name)).second) {
                unique.push_back(a);
            }
        }
        if (unique.size() == 1 || all->get_value()) {
            return unique;
        }
        if (!isatty(STDIN_FILENO)) {
            throw std::runtime_error(spec + " has several rpm assets; use --all or run interactively");
        }
        for (size_t i = 0; i < unique.size(); ++i) {
            std::cout << "  " << i + 1 << ") " << unique[i].name << "\n";
        }
        std::cout << "Select assets (comma-separated numbers, 'a' for all): " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            throw std::runtime_error("no selection made");
        }
        if (line == "a" || line == "all") {
            return unique;
        }
        std::vector<Asset> out;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            size_t n = 0;
            try {
                n = std::stoul(tok);
            } catch (const std::exception &) {
                throw std::runtime_error("invalid selection: " + tok);
            }
            if (n < 1 || n > unique.size()) {
                throw std::runtime_error("selection out of range: " + tok);
            }
            out.push_back(unique[n - 1]);
        }
        if (out.empty()) {
            throw std::runtime_error("no selection made");
        }
        return out;
    }
};

class GhUpdateCommand : public GhCommand {
public:
    explicit GhUpdateCommand(dnf5::Context & ctx) : GhCommand(ctx, "gh-update") {}

    void set_argument_parser() override {
        get_argument_parser_command()->set_description(
            "Refresh the local repositories of tracked GitHub projects (then run 'dnf5 upgrade')");
        specs = std::make_unique<libdnf5::cli::session::StringArgumentList>(
            *this, "owner/repo", "Tracked repositories (default: all)", ArgPositional::UNLIMITED);
    }

    void configure() override {
        require_root();
        auto & ctx = get_context();
        ctx.set_load_system_repo(false);
        ctx.set_load_available_repos(dnf5::Context::LoadAvailableRepos::NONE);
    }

    void run() override {
        auto lock = store.lock();
        Client client;
        auto arch = system_arch();
        auto targets = resolve_targets(specs->get_value());
        if (targets.empty()) {
            say("No tracked GitHub repositories.");
            return;
        }
        int failures = 0;
        for (auto & t : targets) {
            try {
                say(refresh_repository(store, client, t, arch));
            } catch (const std::exception & e) {
                ++failures;
                get_context().print_error(t.slug() + ": " + e.what());
            }
        }
        if (failures) {
            throw std::runtime_error(std::to_string(failures) + " repositories failed to update");
        }
        say("Run 'dnf5 upgrade' to apply updates.");
    }

private:
    std::unique_ptr<libdnf5::cli::session::StringArgumentList> specs;
};

class GhInfoCommand : public GhCommand {
public:
    explicit GhInfoCommand(dnf5::Context & ctx) : GhCommand(ctx, "gh-info") {}

    void set_argument_parser() override {
        get_argument_parser_command()->set_description("Show tracked GitHub repositories");
        specs = std::make_unique<libdnf5::cli::session::StringArgumentList>(
            *this, "owner/repo", "Tracked repositories (default: all)", ArgPositional::UNLIMITED);
    }

    void configure() override {
        auto & ctx = get_context();
        ctx.set_load_system_repo(false);
        ctx.set_load_available_repos(dnf5::Context::LoadAvailableRepos::NONE);
    }

    void run() override {
        auto targets = resolve_targets(specs->get_value());
        if (targets.empty()) {
            say("No tracked GitHub repositories.");
            return;
        }
        for (const auto & t : targets) {
            std::string pk;
            for (const auto & p : t.packages) {
                pk += (pk.empty() ? "" : ", ") + p;
            }
            get_context().print_output(
                t.slug() + "\n  release:    " + t.tag + "\n  prerelease: " + (t.prerelease ? "yes" : "no") +
                "\n  packages:   " + pk + "\n  repo id:    " + store.repo_id(t) + "\n  location:   " +
                store.dir(t).string() + "\n");
        }
    }

private:
    std::unique_ptr<libdnf5::cli::session::StringArgumentList> specs;
};

class GhRemoveCommand : public GhCommand {
public:
    explicit GhRemoveCommand(dnf5::Context & ctx) : GhCommand(ctx, "gh-remove") {}

    void set_argument_parser() override {
        get_argument_parser_command()->set_description(
            "Remove the packages installed from a GitHub repository and stop tracking it");
        specs = std::make_unique<libdnf5::cli::session::StringArgumentList>(
            *this, "owner/repo", "Tracked repositories", ArgPositional::AT_LEAST_ONE);
        // Registering the option also makes dnf5 acquire a write lock.
        dnf5::create_store_option(*this);
    }

    void configure() override {
        require_root();
        auto & ctx = get_context();
        if (!ctx.get_transaction_store_path().empty()) {
            throw std::runtime_error("--store is not supported by gh-remove");
        }
        ctx.set_load_system_repo(true);
        ctx.set_load_available_repos(dnf5::Context::LoadAvailableRepos::NONE);
    }

    void run() override {
        lock = store.lock();
        auto * goal = get_context().get_goal();
        for (const auto & t : resolve_targets(specs->get_value())) {
            for (const auto & p : t.packages) {
                goal->add_remove(p);
            }
            pending.push_back(t);
        }
    }

    // Only forget the repositories once the removal transaction went through.
    void goal_resolved() override {
        dnf5::Command::goal_resolved();
        for (const auto & t : pending) {
            store.remove(t);
        }
    }

private:
    std::unique_ptr<libdnf5::cli::session::StringArgumentList> specs;
    std::vector<Tracked> pending;
    std::unique_ptr<StoreLock> lock;
};

class GhPlugin : public dnf5::IPlugin {
public:
    using IPlugin::IPlugin;

    void init() override {
        unsetenv("DNF5_GH_AUTO_REFRESH");
        auto & command = get_context().get_argument_parser().get_command("upgrade");
        auto previous = command.get_parse_hook_func();
        command.set_parse_hook_func([previous](
            auto * arg, const char * option, int argc, const char * const argv[]) {
            if (previous && !previous(arg, option, argc, argv)) {
                return false;
            }
            if (setenv("DNF5_GH_AUTO_REFRESH", "1", 1) != 0) {
                throw std::runtime_error("cannot enable GitHub refresh for upgrade");
            }
            return true;
        });
    }

    dnf5::PluginAPIVersion get_api_version() const noexcept override { return dnf5::PLUGIN_API_VERSION; }
    const char * get_name() const noexcept override { return "gh"; }
    dnf5::PluginVersion get_version() const noexcept override { return PLUGIN_VERSION; }
    const char * const * get_attributes() const noexcept override { return attrs; }

    const char * get_attribute(const char * name) const noexcept override {
        for (size_t i = 0; attrs[i]; ++i) {
            if (std::string_view(attrs[i]) == name) {
                return attrs_value[i];
            }
        }
        return nullptr;
    }

    std::vector<std::unique_ptr<dnf5::Command>> create_commands() override {
        auto & ctx = get_context();
        std::vector<std::unique_ptr<dnf5::Command>> cmds;
        cmds.push_back(std::make_unique<GhInstallCommand>(ctx));
        cmds.push_back(std::make_unique<GhUpdateCommand>(ctx));
        cmds.push_back(std::make_unique<GhInfoCommand>(ctx));
        cmds.push_back(std::make_unique<GhRemoveCommand>(ctx));
        return cmds;
    }

    void finish() noexcept override {}
};

}  // namespace

extern "C" {

dnf5::PluginAPIVersion dnf5_plugin_get_api_version(void) {
    return dnf5::PLUGIN_API_VERSION;
}

const char * dnf5_plugin_get_name(void) {
    return "gh";
}

dnf5::PluginVersion dnf5_plugin_get_version(void) {
    return PLUGIN_VERSION;
}

dnf5::IPlugin * dnf5_plugin_new_instance(
    [[maybe_unused]] dnf5::ApplicationVersion application_version, dnf5::Context & context) try {
    return new GhPlugin(context);
} catch (...) {
    last_exception = std::current_exception();
    return nullptr;
}

void dnf5_plugin_delete_instance(dnf5::IPlugin * plugin_instance) {
    delete plugin_instance;
}

std::exception_ptr * dnf5_plugin_get_last_exception(void) {
    return &last_exception;
}
}
