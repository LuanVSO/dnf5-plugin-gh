#include "assets.hpp"
#include "refresh.hpp"
#include "store.hpp"
#include "workflows.hpp"

#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace ghplugin;

static const char * RELEASES = R"([
 {"tag_name":"v3.0.0-rc1","prerelease":true,"draft":false,"assets":[
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

    assert(rpm_arch("foo-2.0.0-1.x86_64.rpm") == "x86_64");
    assert(rpm_arch("foo.tar.gz").empty());
    assert(rpm_package_name("foo-data-2.0.0-1.noarch.rpm") == "foo-data");
    assert(rpm_package_name("bad.rpm").empty());

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
    auto unique = unique_package_assets({c[0], Asset{c[0].name, "duplicate", 10}, c[1]});
    assert(unique.size() == 2 && unique[0].url == c[0].url);

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

    auto tmp = std::filesystem::temp_directory_path() / "ghplugin-test";
    std::filesystem::remove_all(tmp);
    Store store(tmp / "state", tmp / "conf");
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
