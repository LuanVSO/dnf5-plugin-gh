#include "assets.hpp"
#include "refresh.hpp"
#include "workflows.hpp"

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/conf/config_main.hpp>
#include <libdnf5/plugin/iplugin.hpp>
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/repo/repo_sack.hpp>
#include <libdnf5/repo/config_repo.hpp>
#include <libdnf5/rpm/package.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/transaction/transaction.hpp>
#include <libdnf5/transaction/transaction_history.hpp>
#include <libdnf5/transaction/transaction_item_action.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <set>

namespace {

std::exception_ptr last_exception;
constexpr libdnf5::plugin::Version VERSION{GH_VERSION_MAJOR, GH_VERSION_MINOR, GH_VERSION_PATCH};
constexpr const char * ATTRIBUTES[]{nullptr};

class GhLibdnfPlugin : public libdnf5::plugin::IPlugin {
public:
    using IPlugin::IPlugin;

    libdnf5::PluginAPIVersion get_api_version() const noexcept override { return libdnf5::PLUGIN_API_VERSION; }
    const char * get_name() const noexcept override { return "gh"; }
    libdnf5::plugin::Version get_version() const noexcept override { return VERSION; }
    const char * const * get_attributes() const noexcept override { return ATTRIBUTES; }
    const char * get_attribute(const char *) const noexcept override { return nullptr; }

    void repos_configured() override {
        if (std::getenv("DNF5_GH_OWN_TRANSACTION")) {
            return;
        }
        const auto & config = get_base().get_config();
        if (geteuid() != 0 || config.get_cacheonly_option().get_value() != "none") {
            return;
        }
        if (!targets_host_config()) {
            std::cerr << "GitHub refresh skipped for an alternate installroot without --use-host-config\n";
            return;
        }
        try {
            ghplugin::Store store;
            auto lock = store.lock();
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ghplugin::Client client(get_base());
            for (auto & t : store.list()) {
                try {
                    migrate_loaded_repo(store, t);
                    refresh_loaded_repo(store, client, t, now);
                } catch (const std::exception & ex) {
                    std::cerr << "Warning: GitHub refresh failed for " << t.slug() << ": " << ex.what()
                              << "; using existing repository metadata\n";
                }
            }
        } catch (const std::exception & ex) {
            std::cerr << "Warning: GitHub refresh failed: " << ex.what() << '\n';
        }
    }

    void pre_transaction(const libdnf5::base::Transaction & transaction) override {
        reset_tracking();
        const auto & config = get_base().get_config();
        if (geteuid() != 0 || std::getenv("DNF5_GH_OWN_TRANSACTION") || !targets_host_config()) {
            return;
        }
        try {
            std::set<std::string> removed_nevras;
            for (const auto & item : transaction.get_transaction_packages()) {
                const auto action = item.get_action();
                const auto package = item.get_package();
                if (libdnf5::transaction::transaction_item_action_is_inbound(action)) {
                    const auto repo_id = package.get_repo_id();
                    if (ghplugin::is_github_repo_id(repo_id)) {
                        installed_packages[repo_id].insert(package.get_name());
                    }
                }
                if (action == libdnf5::transaction::TransactionItemAction::REMOVE) {
                    const auto origin = package.get_from_repo_id();
                    if (ghplugin::is_github_repo_id(origin)) {
                        removed_packages[origin].insert(package.get_name());
                        removed_nevras.insert(package.get_nevra());
                    }
                }
            }
            if (removed_packages.empty() && installed_packages.empty()) {
                return;
            }
            if (!removed_packages.empty()) {
                libdnf5::rpm::PackageQuery installed(get_base());
                installed.filter_installed();
                for (const auto & package : installed) {
                    if (!removed_nevras.count(package.get_nevra())) {
                        remaining_packages[package.get_from_repo_id()].insert(package.get_name());
                    }
                }
                for (const auto & item : transaction.get_transaction_packages()) {
                    if (libdnf5::transaction::transaction_item_action_is_inbound(item.get_action())) {
                        const auto package = item.get_package();
                        remaining_packages[package.get_repo_id()].insert(package.get_name());
                    }
                }
            }
            if (config.get_skip_system_repo_lock_option().get_value()) {
                reset_tracking();
                std::cerr << "Warning: GitHub tracking update skipped because the system repository lock is disabled\n";
                return;
            }
            const auto ids = get_base().get_transaction_history()->list_transaction_ids();
            previous_history_id = ids.empty() ? 0 : ids.back();
        } catch (const std::exception & ex) {
            reset_tracking();
            std::cerr << "Warning: cannot prepare GitHub tracking update: " << ex.what() << '\n';
        }
    }

    void post_transaction(const libdnf5::base::Transaction &) override {
        if (geteuid() == 0 && !std::getenv("DNF5_GH_OWN_TRANSACTION") && targets_host_config()) {
            try {
                ghplugin::Store store;
                auto lock = store.lock();
                store.clean_rpms();
            } catch (const std::exception & ex) {
                std::cerr << "Warning: cannot clean GitHub repository RPMs: " << ex.what() << '\n';
            }
        }
        if (removed_packages.empty() && installed_packages.empty()) {
            return;
        }
        try {
            // This hook also runs after failed RPM transactions.
            const auto latest = get_base().get_transaction_history()->get_latest_transaction();
            if (latest.get_id() <= previous_history_id ||
                latest.get_state() != libdnf5::transaction::TransactionState::OK) {
                reset_tracking();
                return;
            }
            ghplugin::Store store;
            auto lock = store.lock();
            for (auto & tracked : store.list()) {
                if (!ghplugin::reconcile_tracked_packages(
                        tracked, store, removed_packages, remaining_packages, installed_packages)) {
                    continue;
                }
                if (tracked.packages.empty()) {
                    store.remove(tracked);
                    std::cerr << "GitHub: stopped tracking " << tracked.slug() << '\n';
                } else {
                    store.save(tracked);
                    std::cerr << "GitHub: updated tracked packages for " << tracked.slug() << '\n';
                }
            }
        } catch (const std::exception & ex) {
            std::cerr << "Warning: cannot update GitHub tracking after transaction: " << ex.what() << '\n';
        }
        reset_tracking();
    }

private:
    // Whether the transaction runs against the host's own configuration and state.
    bool targets_host_config() {
        const auto & config = get_base().get_config();
        return config.get_installroot_option().get_value() == "/" ||
               config.get_use_host_config_option().get_value();
    }

    // Replaces a legacy-named repository with its migrated one, keeping its enabled state.
    void migrate_loaded_repo(const ghplugin::Store & store, const ghplugin::Tracked & t) {
        auto & base = get_base();
        libdnf5::repo::RepoQuery legacy(base);
        legacy.filter_id(store.legacy_repo_id(t));
        if (!store.migrate_repo(t)) {
            return;
        }
        bool enabled = true;
        for (auto & repo : legacy) {
            enabled = repo->get_config().get_enabled_option().get_value();
        }
        base.get_repo_sack()->create_repos_from_file(store.repo_file(t).string());
        libdnf5::repo::RepoQuery replacement(base);
        replacement.filter_id(store.repo_id(t));
        if (replacement.empty()) {
            throw std::runtime_error("cannot load migrated repository");
        }
        for (auto & repo : replacement) {
            if (!enabled) {
                repo->disable();
            }
        }
        for (auto & repo : legacy) {
            repo->disable();
        }
    }

    void refresh_loaded_repo(
        ghplugin::Store & store, ghplugin::Client & client, ghplugin::Tracked & t, std::int64_t now) {
        auto & base = get_base();
        libdnf5::repo::RepoQuery repos(base);
        repos.filter_id(store.repo_id(t));
        repos.filter_enabled(true);
        repos.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
        for (auto & repo : repos) {
            auto & baseurl = repo->get_config().get_baseurl_option();
            auto urls = baseurl.get_value();
            if (store.relocate_baseurls(t, urls)) {
                baseurl.set(baseurl.get_priority(), urls);
            }
            if (ghplugin::refresh_due(t, now) || !store.has_remote_metadata(t)) {
                std::cerr << "GitHub: "
                          << ghplugin::refresh_repository(store, client, t, base.get_vars()->get_value("arch"))
                          << '\n';
            }
            // Existing repo files may still have metadata_expire=never.
            repo->expire();
        }
    }

    void reset_tracking() {
        removed_packages.clear();
        remaining_packages.clear();
        installed_packages.clear();
    }

    ghplugin::PackageNamesByRepo removed_packages;
    ghplugin::PackageNamesByRepo remaining_packages;
    ghplugin::PackageNamesByRepo installed_packages;
    std::int64_t previous_history_id = 0;
};

}  // namespace

extern "C" {
libdnf5::PluginAPIVersion libdnf_plugin_get_api_version() { return libdnf5::PLUGIN_API_VERSION; }
const char * libdnf_plugin_get_name() { return "gh"; }
libdnf5::plugin::Version libdnf_plugin_get_version() { return VERSION; }
libdnf5::plugin::IPlugin * libdnf_plugin_new_instance(
    libdnf5::LibraryVersion, libdnf5::plugin::IPluginData & data, libdnf5::ConfigParser &) try {
    return new GhLibdnfPlugin(data);
} catch (...) {
    last_exception = std::current_exception();
    return nullptr;
}
void libdnf_plugin_delete_instance(libdnf5::plugin::IPlugin * plugin) { delete plugin; }
std::exception_ptr * libdnf_plugin_get_last_exception() { return &last_exception; }
}
