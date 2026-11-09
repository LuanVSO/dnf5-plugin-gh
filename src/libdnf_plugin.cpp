#include "assets.hpp"
#include "refresh.hpp"

#include <libdnf5/base/base.hpp>
#include <libdnf5/conf/config_main.hpp>
#include <libdnf5/plugin/iplugin.hpp>
#include <libdnf5/repo/repo_query.hpp>
#include <libdnf5/repo/repo_sack.hpp>
#include <libdnf5/repo/config_repo.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
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
