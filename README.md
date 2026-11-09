# dnf5-plugin-gh

A dnf5 plugin that installs and updates RPMs published as GitHub release assets.

Each tracked project gets a local repository under `/var/lib/dnf5-gh/<owner>_<repo>/`
(RPMs + `createrepo_c` metadata) and a repo file `/etc/yum.repos.d/gh-<owner>_<repo>.repo`,
so dnf5 resolves, upgrades and removes those packages natively.

## Commands

| Command | Description |
|---|---|
| `dnf5 gh-install <owner/repo>... [--prerelease] [--all]` | Install RPMs from the newest release with an RPM for this architecture (`x86_64`/`noarch`). Prompts when several packages are available (`--all` takes all). |
| `dnf5 gh-update [owner/repo...]` | Refresh tracked projects from GitHub, then run `dnf5 upgrade` to apply. |
| `dnf5 gh-info [owner/repo...]` | Show tracked projects. |
| `dnf5 gh-remove <owner/repo>...` | Remove the installed packages and stop tracking. |

Set `GITHUB_TOKEN` (or `GH_TOKEN`) to avoid API rate limits; use `sudo -E` to keep it.

`gh-install` also supports dnf5's `--store PATH` option. It prepares the local
repository and records the project even though the transaction is only stored,
not executed. `--store` is not supported by `gh-remove`.

**Removal caveat:** `gh-remove` currently deletes tracking data before the
transaction is confirmed or run. If removal is cancelled or fails, the package
may remain installed but no longer be tracked.

## Build

Requires `libdnf5-devel libdnf5-cli-devel dnf5-devel libcurl-devel json-devel`; `createrepo_c` at runtime.

    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DDNF5_PLUGIN_DIR="$(rpm --eval '%{_libdir}')/dnf5/plugins"
    cmake --build build && ctest --test-dir build
    sudo cmake --install build

An optional isolated transaction test requires user, mount and network
namespaces, `rpmbuild`, `createrepo_c`, `ip`, `openssl`, `curl` and Python 3.
Pass an empty disposable directory and the built plugin:

    bash tests/test_transactions.sh "$(mktemp -d)" "$PWD/build/gh_cmd_plugin.so"

The test directory is left for inspection. On systems with a different curl CA
bundle path, set `GH_TEST_CA_BUNDLE` to that path.

## Security note

Release RPMs are fetched over HTTPS but are not GPG-verified (`gpgcheck=0`); only track projects you trust.
