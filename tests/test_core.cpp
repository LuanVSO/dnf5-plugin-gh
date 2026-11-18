#include "assets.hpp"
#include "github.hpp"
#include "refresh.hpp"
#include "store.hpp"
#include "workflows.hpp"

#include <libdnf5/base/base.hpp>
#include <libdnf5/conf/config_parser.hpp>
#include <libdnf5/conf/option_string_list.hpp>

#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

using namespace ghplugin;

static const char * RELEASES = R"([
 {"tag_name":"v3.0.0-rc1","name":null,"prerelease":true,"draft":false,"assets":[
   {"name":"foo-3.0.0~rc1-1.x86_64.rpm","browser_download_url":"https://x/a","size":10}]},
 {"tag_name":"v2.1.0","prerelease":false,"draft":true,"assets":[
   {"name":"foo-2.1.0-1.x86_64.rpm","browser_download_url":"https://x/b","size":10}]},
 {"tag_name":"v2.0.0","prerelease":false,"draft":false,"assets":[
   {"name":"foo-2.0.0-1.src.rpm","browser_download_url":"https://x/c","size":10},
   {"name":"foo-2.0.0-1.aarch64.rpm","browser_download_url":"https://x/d","size":10},
   {"name":"foo-debuginfo-2.0.0-1.x86_64.rpm","browser_download_url":"https://x/e","size":10},
   {"name":"foo-2.0.0-1.x86_64.rpm","browser_download_url":"https://x/f","size":10},
   {"name":"foo-data-2.0.0-1.noarch.rpm","browser_download_url":"https://x/g","size":10},
   {"name":"foo.tar.gz","browser_download_url":"https://x/h","size":10}]}
])";

int main() {
    auto releases = parse_releases(RELEASES);
    assert(releases.size() == 3);

    assert(rpm_arch("foo-2.0.0-1.x86_64.rpm", "x86_64") == "x86_64");
    assert(rpm_arch("winboat-0.9.2-x86_64.rpm", "x86_64") == "x86_64");
    assert(rpm_arch("winboat_0.9.2_x86_64.rpm", "x86_64") == "x86_64");
    assert(rpm_arch("foo-2.0.0-aarch64.rpm", "x86_64").empty());
    assert(rpm_arch("foo-2.0.0-noarch.rpm", "x86_64") == "noarch");
    assert(rpm_arch("foo.tar.gz", "x86_64").empty());
    assert(rpm_package_name("foo-data-2.0.0-1.noarch.rpm") == "foo-data");
    assert(rpm_package_name("bad.rpm").empty());

    Release alternate_names;
    alternate_names.assets = {
        {"winboat-0.9.2-x86_64.rpm", "https://x/winboat", 10},
        {"winboat_0.9.2_x86_64.rpm", "https://x/winboat-alt", 10},
        {"foo-2.0.0-aarch64.rpm", "https://x/aarch64", 10},
        {"foo-2.0.0-noarch.rpm", "https://x/noarch", 10},
    };
    auto alternate_compatible = compatible_assets(alternate_names, "x86_64");
    assert(alternate_compatible.size() == 3);
    assert(alternate_compatible[0].name == "winboat-0.9.2-x86_64.rpm");
    assert(alternate_compatible[1].name == "winboat_0.9.2_x86_64.rpm");
    assert(alternate_compatible[2].name == "foo-2.0.0-noarch.rpm");

    auto globbed = compatible_assets(alternate_names, "x86_64", {"*_x86_64.rpm", "*noarch*"});
    assert(globbed.size() == 2 && globbed[0].name == "winboat_0.9.2_x86_64.rpm");
    assert(compatible_assets(alternate_names, "x86_64", {"foo-2.0.0-aarch64*"}).size() == 1);
    assert(compatible_assets(alternate_names, "x86_64", {"nomatch*"}).empty());
    assert(!pick_release(releases, "x86_64", true, {"nomatch*"}));

    auto stable = pick_release(releases, "x86_64", false);
    assert(stable && releases[*stable].tag == "v2.0.0");
    auto pre = pick_release(releases, "x86_64", true);
    assert(pre && releases[*pre].tag == "v3.0.0-rc1");
    assert(compatible_assets(releases[2], "ppc64le").size() == 1);
    auto plan = make_install_plan(releases, "owner/repo", "x86_64", false);
    assert(plan.owner == "owner" && plan.repo == "repo");
    assert(plan.release.tag == "v2.0.0" && !plan.prerelease && plan.candidates.size() == 2);

    auto c = compatible_assets(releases[2], "x86_64");
    assert(c.size() == 2);
    assert(c[0].name == "foo-2.0.0-1.x86_64.rpm" && c[1].name == "foo-data-2.0.0-1.noarch.rpm");
    auto unique = unique_package_assets({
        Asset{c[0].name, c[0].url, 10, "foo"},
        Asset{"different-2.0.0-1.x86_64.rpm", "duplicate", 10, "foo"},
        Asset{c[1].name, c[1].url, 10, "foo-data"}});
    assert(unique.size() == 2 && unique[0].url == c[0].url);
    Tracked selected{"owner", "repo"};
    replace_tracked_packages(selected, unique);
    assert((selected.packages == std::vector<std::string>{"foo", "foo-data"}));

    assert(parse_slug("a/b.c").first == "a");
    for (auto bad : {"a", "a/b/c", "/b", "a/..", "-a/b", "a b/c"}) {
        try {
            parse_slug(bad);
            assert(false);
        } catch (const std::invalid_argument &) {
        }
    }
    assert(is_safe_rpm_name("x.rpm") && !is_safe_rpm_name("../x.rpm") && !is_safe_rpm_name(".x.rpm"));
    assert(is_github_repo_id("gh:owner:repo"));
    assert(is_github_repo_id("gh-owner_repo"));
    assert(!is_github_repo_id("third-party"));

    auto tmp_pattern = (std::filesystem::temp_directory_path() / "ghplugin-test-XXXXXX").string();
    std::vector<char> tmp_buffer(tmp_pattern.begin(), tmp_pattern.end());
    tmp_buffer.push_back('\0');
    auto * tmp_created = mkdtemp(tmp_buffer.data());
    assert(tmp_created);
    std::filesystem::path tmp(tmp_created);
    std::filesystem::path prepared_path;
    {
        PreparedAssets prepared(tmp);
        prepared_path = prepared.path();
        std::ofstream(prepared_path / "asset") << "data";
        assert(std::filesystem::exists(prepared_path / "asset"));
    }
    assert(!std::filesystem::exists(prepared_path));

    auto migration_old = tmp / "old-state";
    auto migration_new = tmp / "new-state";
    auto migration_conf = tmp / "migration-conf";
    std::filesystem::create_directories(migration_old / "owner_repo");
    std::ofstream(migration_old / "owner_repo" / "state.json") << R"({"owner":"owner","repo":"repo"})";
    std::ofstream(migration_old / "preserved") << "old data";
    std::filesystem::create_directories(migration_new);
    std::ofstream(migration_new / "current") << "new data";
    Store migration_store(migration_new, migration_conf, "createrepo_c", migration_old);
    auto migration_lock = migration_store.lock();
    auto migrated_projects = migration_store.list();
    assert(migrated_projects.size() == 1 && migrated_projects[0].slug() == "owner/repo");
    assert(!std::filesystem::exists(migration_old));
    assert(std::filesystem::exists(migration_new / "owner_repo" / "state.json"));
    assert(std::filesystem::exists(migration_new / "preserved"));
    assert(std::filesystem::exists(migration_new / "current"));
    std::filesystem::create_directories(migration_conf);
    Tracked relocated{"owner", "repo"};
    auto old_url = "file://" + (migration_old / "owner_repo").string();
    auto new_url = "file://" + (migration_new / "owner_repo").string();
    {
        std::ofstream repo(migration_store.repo_file(relocated));
        repo << "# preserved comment\n[gh:owner:repo]\nbaseurl = " << old_url
             << "/\n  https://example.test/other\n  " << old_url << "-other\n"
             << "enabled=0\n[other]\nbaseurl=https://example.test/unrelated\n";
    }
    std::filesystem::permissions(
        migration_store.repo_file(relocated), std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
    assert(!migration_store.migrate_repo(relocated));
    {
        auto repo_file = migration_store.repo_file(relocated);
        std::ifstream repo(repo_file);
        std::string contents((std::istreambuf_iterator<char>(repo)), {});
        assert(contents.find("# preserved comment\n") != std::string::npos);
        assert(contents.find("enabled=0\n[other]\nbaseurl=https://example.test/unrelated\n") != std::string::npos);
        libdnf5::ConfigParser parser;
        parser.read(repo_file.string());
        libdnf5::OptionStringList option(std::vector<std::string>{});
        assert((option.from_string(parser.get_value("gh:owner:repo", "baseurl")) ==
                std::vector<std::string>{new_url + "/", "https://example.test/other", old_url + "-other"}));
        assert((std::filesystem::status(repo_file).permissions() & std::filesystem::perms::all) ==
               (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    }
    std::vector<std::string> urls{old_url, old_url + "/", old_url + "-other", "https://example.test/"};
    assert(migration_store.relocate_baseurls(relocated, urls));
    assert((urls == std::vector<std::string>{new_url, new_url + "/", old_url + "-other", "https://example.test/"}));
    assert(!migration_store.relocate_baseurls(relocated, urls));
    std::filesystem::remove(migration_store.repo_file(relocated));
    {
        std::ofstream repo(migration_conf / "gh-owner_repo.repo");
        repo << "[gh-owner_repo]\nbaseurl=" << old_url << "\nenabled=0\n";
    }
    assert(migration_store.migrate_repo(relocated));
    {
        std::ifstream repo(migration_store.repo_file(relocated));
        std::string contents((std::istreambuf_iterator<char>(repo)), {});
        assert(contents == "[gh:owner:repo]\nbaseurl=" + new_url + "\nenabled=0\n");
    }
    assert(!std::filesystem::exists(migration_conf / "gh-owner_repo.repo"));
    migration_lock.reset();
    std::filesystem::create_directories(migration_old / "owner_repo");
    std::ofstream(migration_old / "owner_repo" / "state.json") << "legacy";
    try {
        migration_store.lock();
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(std::filesystem::exists(migration_old / "owner_repo" / "state.json"));
    assert(std::filesystem::exists(migration_new / "owner_repo" / "state.json"));
    std::filesystem::remove_all(migration_old);

    const auto old_github_token = std::getenv("GITHUB_TOKEN")
        ? std::optional<std::string>(std::getenv("GITHUB_TOKEN"))
        : std::nullopt;
    const auto old_gh_token = std::getenv("GH_TOKEN")
        ? std::optional<std::string>(std::getenv("GH_TOKEN"))
        : std::nullopt;
    unsetenv("GITHUB_TOKEN");
    unsetenv("GH_TOKEN");
    auto token_config = tmp / "gh.conf";
    auto dropins = tmp / "gh.d";
    auto private_token_config = dropins / "token.conf";
    auto vendor = tmp / "vendor";
    const std::vector<std::filesystem::path> config_dirs{tmp, vendor};
    {
        std::ofstream config(token_config);
        config << "[main]\nenabled = 1\ntoken = config-token\n";
    }
    assert(resolve_github_token(config_dirs) == "config-token");
    std::filesystem::create_directories(dropins);
    std::filesystem::create_directories(vendor / "gh.d");
    std::ofstream(vendor / "gh.d" / "token.conf") << "[main]\ntoken = masked-token\n";
    std::ofstream(vendor / "gh.d" / "00-vendor.conf") << "[main]\ntoken = vendor-token\n";
    std::ofstream(dropins / "ignore.txt") << "[main]\ntoken = ignored-token\n";
    {
        std::ofstream config(private_token_config);
        config << "[main]\ntoken = private-token\n";
    }
    assert(resolve_github_token(config_dirs) == "private-token");
    std::ofstream(dropins / "zz-override.conf") << "[main]\ntoken = override-token\n";
    assert(resolve_github_token(config_dirs) == "override-token");
    std::filesystem::remove(dropins / "zz-override.conf");
    std::ofstream(dropins / "zz-empty.conf") << "[main]\ntoken =\n";
    assert(resolve_github_token(config_dirs).empty());
    std::filesystem::remove(dropins / "zz-empty.conf");
    {
        std::ofstream config(private_token_config);
        config << "[main]\n";
    }
    assert(resolve_github_token(config_dirs) == "vendor-token");
    {
        std::ofstream config(private_token_config);
        config << "[main]\ntoken = private-token\n";
    }
    setenv("GH_TOKEN", "environment-token", 1);
    assert(resolve_github_token(config_dirs) == "environment-token");
    setenv("GITHUB_TOKEN", "preferred-token", 1);
    assert(resolve_github_token(config_dirs) == "preferred-token");
    unsetenv("GITHUB_TOKEN");
    unsetenv("GH_TOKEN");
    std::filesystem::remove_all(dropins);
    std::filesystem::remove_all(vendor);
    assert(resolve_github_token(config_dirs) == "config-token");
    std::filesystem::remove(token_config);
    assert(resolve_github_token(config_dirs).empty());
    std::ofstream(dropins) << "not a directory";
    try {
        resolve_github_token(config_dirs);
        assert(false);
    } catch (const std::filesystem::filesystem_error &) {
    }
    std::filesystem::remove(dropins);
    if (old_github_token) {
        setenv("GITHUB_TOKEN", old_github_token->c_str(), 1);
    }
    if (old_gh_token) {
        setenv("GH_TOKEN", old_gh_token->c_str(), 1);
    }

    Store store(tmp / "state", tmp / "conf");
    std::filesystem::create_directories(tmp / "conf");
    Tracked sync_tracked{"sync", "repo", "v1", false, {"foo"}};
    auto sync_dir = store.dir(sync_tracked);
    std::filesystem::create_directories(sync_dir / "repodata");
    std::filesystem::create_directories(sync_dir / "keep.rpm");
    std::ofstream(sync_dir / "repodata" / "old") << "old metadata";
    std::ofstream(sync_dir / "foo-1-1.x86_64.rpm") << "old";
    std::ofstream(sync_dir / "foo-2-1.x86_64.rpm") << "new";
    std::filesystem::create_directories(sync_dir / "nested");
    std::ofstream(sync_dir / "nested" / ".stale.rpm") << "stale";
    std::ofstream(sync_dir / "notes.txt") << "keep";
    const std::vector<Asset> sync_assets{{"foo-2-1.x86_64.rpm", "https://example.test/foo-2-1.x86_64.rpm", 3, "foo"}};
    setenv("GITHUB_TOKEN", "test-token", 1);
    libdnf5::Base base;
    Client client(base);
    if (old_github_token) {
        setenv("GITHUB_TOKEN", old_github_token->c_str(), 1);
    } else {
        unsetenv("GITHUB_TOKEN");
    }
    Store failing_store(tmp / "state", tmp / "conf", "/bin/false");
    try {
        failing_store.sync(client, sync_tracked, sync_assets);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(std::filesystem::exists(sync_dir / "foo-1-1.x86_64.rpm"));
    assert(std::filesystem::exists(sync_dir / "repodata" / "old"));
    for (const auto & entry : std::filesystem::directory_iterator(sync_dir)) {
        assert(!entry.path().filename().string().starts_with(".staging-"));
    }

    auto createrepo = tmp / "createrepo";
    {
        std::ofstream script(createrepo);
        script << "#!/bin/sh\nmkdir \"$5/repodata\"\necho new > \"$5/repodata/new\"\ntouch \"$5/repodata/repomd.xml\"\n";
    }
    std::filesystem::permissions(createrepo, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    Store sync_store(tmp / "state", tmp / "conf", createrepo.string());
    sync_store.sync(client, sync_tracked, sync_assets);
    assert(!std::filesystem::exists(sync_dir / "foo-1-1.x86_64.rpm"));
    assert(!std::filesystem::exists(sync_dir / "foo-2-1.x86_64.rpm"));
    assert(!std::filesystem::exists(sync_dir / "nested" / ".stale.rpm"));
    assert(std::filesystem::exists(sync_dir / "repodata" / "new"));
    assert(std::filesystem::exists(sync_dir / "notes.txt"));
    assert(std::filesystem::is_directory(sync_dir / "keep.rpm"));
    assert(sync_store.has_remote_metadata(sync_tracked));
    std::filesystem::remove(sync_dir / "repodata" / "repomd.xml");
    assert(!sync_store.has_remote_metadata(sync_tracked));

    Tracked package_state{"owner", "repo", "v1", false, {"foo", "bar"}};
    PackageNamesByRepo removed{{store.legacy_repo_id(package_state), {"foo"}}};
    PackageNamesByRepo installed{{store.repo_id(package_state), {"baz"}}};
    assert(reconcile_tracked_packages(package_state, store, removed, {}, installed));
    assert((package_state.packages == std::vector<std::string>{"bar", "baz"}));
    PackageNamesByRepo remaining{{store.repo_id(package_state), {"bar"}}};
    installed[store.repo_id(package_state)].insert("baz");
    assert(!reconcile_tracked_packages(package_state, store, removed, remaining, installed));

    Tracked t{"o", "r", "v1", true, {"foo"}};
    assert(refresh_due(t, 100000));
    t.last_checked = 100000;
    assert(!refresh_due(t, 100000));
    assert(!refresh_due(t, 100000 + REFRESH_INTERVAL - 1));
    assert(refresh_due(t, 100000 + REFRESH_INTERVAL));
    assert(refresh_due(t, 99999));
    store.save(t);
    auto found = store.find("o/r");
    assert(found && found->tag == "v1" && found->prerelease && found->packages.size() == 1);
    assert(found->last_checked == 100000);
    record_release(store, t, "v2");
    found = store.find("o/r");
    assert(found && found->tag == "v2" && found->last_checked > 0);
    assert(store.repo_id(t) == "gh:o:r");
    std::filesystem::create_directories(tmp / "conf");
    {
        std::ofstream old(tmp / "conf" / "gh-o_r.repo");
        old << "[gh-o_r]\nname=test\nenabled=0\n";
    }
    assert(store.migrate_repo(t));
    assert(!std::filesystem::exists(tmp / "conf" / "gh-o_r.repo"));
    std::ifstream migrated(store.repo_file(t));
    std::string contents((std::istreambuf_iterator<char>(migrated)), {});
    assert(contents == "[gh:o:r]\nname=test\nenabled=0\n");
    {
        std::ofstream old(tmp / "conf" / "gh-o_r.repo");
        old << "[gh-o_r]\n";
    }
    try {
        store.migrate_repo(t);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(std::filesystem::exists(tmp / "conf" / "gh-o_r.repo"));
    assert(std::filesystem::exists(store.repo_file(t)));
    store.remove(t);
    assert(store.list().empty());
    std::filesystem::remove_all(tmp);

    std::cout << "ok\n";
}
