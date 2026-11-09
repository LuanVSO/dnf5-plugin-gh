#include "assets.hpp"
#include "refresh.hpp"

#include <libdnf5/base/base.hpp>
#include <libdnf5/base/transaction.hpp>
#include <libdnf5/base/transaction_package.hpp>
#include <libdnf5/conf/config_main.hpp>
#include <libdnf5/plugin/iplugin.hpp>
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/repo/repo_sack.hpp>
#include <libdnf5/repo/config_repo.hpp>
#include <libdnf5/rpm/package.hpp>
#include <libdnf5/transaction/transaction.hpp>
#include <libdnf5/transaction/transaction_history.hpp>
#include <libdnf5/transaction/transaction_item_action.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <set>
#include <string_view>

namespace {

std::exception_ptr last_exception;
constexpr libdnf5::plugin::Version VERSION{0, 1, 0};
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
        const char * marker = std::getenv("DNF5_GH_AUTO_REFRESH");
        auto & base = get_base();
        const auto & config = base.get_config();
        if (!marker || std::string_view(marker) != "1" || geteuid() != 0 ||
            config.get_cacheonly_option().get_value() != "none") {
            return;
        }
        if (config.get_installroot_option().get_value() != "/" &&
            !config.get_use_host_config_option().get_value()) {
            std::cerr << "GitHub refresh skipped for an alternate installroot without --use-host-config\n";
            return;
        }
        try {
            ghplugin::Store store;
            auto lock = store.lock();
            auto tracked = store.list();
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ghplugin::Client client;
            for (auto & t : tracked) {
                try {
                    libdnf5::repo::RepoQuery legacy(base);
                    legacy.filter_id(store.legacy_repo_id(t));
                    if (store.migrate_repo(t)) {
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
                    libdnf5::repo::RepoQuery repos(base);
                    repos.filter_id(store.repo_id(t));
                    repos.filter_enabled(true);
                    repos.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
                    for (auto & repo : repos) {
                        if (ghplugin::refresh_due(t, now)) {
                            std::cerr << "GitHub: "
                                      << ghplugin::refresh_repository(store, client, t, ghplugin::system_arch()) << '\n';
                        }
                        // Existing repo files may still have metadata_expire=never.
                        repo->expire();
                    }
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
        cleanup_repos.clear();
        for (const auto & item : transaction.get_transaction_packages()) {
            if (!libdnf5::transaction::transaction_item_action_is_inbound(item.get_action())) {
                continue;
            }
            const auto package = item.get_package();
            if (package.get_repo_id().rfind("gh:", 0) == 0 &&
                package.get_baseurl().rfind("https://", 0) == 0) {
                cleanup_repos.insert(package.get_repo_id());
            }
        }
        if (cleanup_repos.empty()) {
            return;
        }
        if (get_base().get_config().get_skip_system_repo_lock_option().get_value()) {
            cleanup_repos.clear();
            std::cerr << "Warning: GitHub RPM cleanup skipped because the system repository lock is disabled\n";
            return;
        }
        try {
            const auto ids = get_base().get_transaction_history()->list_transaction_ids();
            previous_history_id = ids.empty() ? 0 : ids.back();
        } catch (const std::exception & ex) {
            cleanup_repos.clear();
            std::cerr << "Warning: cannot prepare GitHub RPM cleanup: " << ex.what() << '\n';
        }
    }

    void post_transaction(const libdnf5::base::Transaction &) override {
        if (cleanup_repos.empty()) {
            return;
        }
        try {
            // post_transaction is also called when the RPM transaction fails.
            const auto latest = get_base().get_transaction_history()->get_latest_transaction();
            if (latest.get_id() <= previous_history_id ||
                latest.get_state() != libdnf5::transaction::TransactionState::OK) {
                return;
            }
            ghplugin::Store store;
            auto lock = store.lock();
            for (const auto & tracked : store.list()) {
                if (tracked.remote_packages && cleanup_repos.count(store.repo_id(tracked))) {
                    store.remove_rpms(tracked);
                }
            }
        } catch (const std::exception & ex) {
            std::cerr << "Warning: cannot clean up GitHub RPMs: " << ex.what() << '\n';
        }
        cleanup_repos.clear();
    }

private:
    std::set<std::string> cleanup_repos;
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
