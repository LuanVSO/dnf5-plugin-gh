# dnf5-plugin-gh

A dnf5 plugin that installs and updates RPMs published as GitHub release assets.

Each tracked project gets a local repository under `/var/lib/dnf5-gh/<owner>_<repo>/`
(`createrepo_c` metadata and temporary RPMs) and a repo file `/etc/yum.repos.d/gh:<owner>:<repo>.repo`,
so dnf5 resolves, upgrades and removes those packages natively.
New installs are recorded as originating from `gh:<owner>:<repo>`. A slash
cannot be used in a dnf repository ID. Existing `gh-<owner>_<repo>` files are
migrated when the project is refreshed or installed again; existing installed
package origins are not rewritten.

## Commands

| Command | Description |
|---|---|
| `dnf5 gh-install <owner/repo>... [--prerelease] [--all]` | Install RPMs from the newest release with an RPM for this architecture (`x86_64`/`noarch`). Prompts when several packages are available (`--all` takes all). |
| `dnf5 gh-update [owner/repo...]` | Refresh tracked projects from GitHub, then run `dnf5 upgrade` to apply. |
| `dnf5 gh-info [owner/repo...]` | Show tracked projects. |
| `dnf5 gh-remove <owner/repo>...` | Remove the installed packages and stop tracking. |

`dnf5 update` and `dnf5 upgrade` automatically check each enabled tracked
GitHub repository at most once every 24 hours. They refresh local metadata
before resolving the transaction and install available updates normally.
`dnf5 gh-update` forces an immediate check, regardless of the interval.
On a failed refresh, a warning is printed and the previous repository metadata
remains available; the next update retries. Metadata points to GitHub release
assets, so a package whose RPM is no longer cached requires network access.

After a successful transaction that installs or updates packages from a GitHub
repository, the enabled libdnf5 `gh` plugin removes RPMs from that local
repository. Metadata is retained, and dnf can download the assets again when
needed. Stored, cancelled, and failed transactions do not trigger cleanup.
DNF's own package cache is managed separately by its `keepcache` setting.
Cleanup is skipped with a warning if the system repository lock is disabled.
Older repositories are converted to remote asset locations on their next
successful refresh or `gh-install`; until then their RPMs are retained.
The release asset URLs must share a download directory and end with the
corresponding asset filenames, as GitHub release download URLs do.

Automatic refresh requires both the dnf5 command plugin and the enabled
libdnf5 `gh` plugin. It is skipped for non-root runs, `--cacheonly`, disabled
repositories, and alternate installroots unless `--use-host-config` is supplied.
With `--use-host-config`, tracked repositories and metadata are shared with the
host. Generated local repositories use `metadata_expire=0`: dnf checks the
local metadata on each use, while GitHub itself is queried only after the
24-hour interval.

Set `GITHUB_TOKEN` (or `GH_TOKEN`) to avoid API rate limits; use `sudo -E` to keep it.

`gh-install` also supports dnf5's `--store PATH` option. It prepares the local
repository and records the project even though the transaction is only stored,
not executed. `--store` is not supported by `gh-remove`.

**Removal caveat:** `gh-remove` currently deletes tracking data before the
transaction is confirmed or run. If removal is cancelled or fails, the package
may remain installed but no longer be tracked.

## Build

Requires `libdnf5-devel libdnf5-cli-devel dnf5-devel libcurl-devel json-devel`; `createrepo_c` at runtime.

    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc -DDNF5_PLUGIN_DIR="$(rpm --eval '%{_libdir}')/dnf5/plugins"
    cmake --build build && ctest --test-dir build
    sudo cmake --install build

An optional isolated transaction test requires user, mount and network
namespaces, `rpmbuild`, `createrepo_c`, `ip`, `openssl`, `curl` and Python 3.
Pass an empty disposable directory and the built plugin:

    bash tests/test_transactions.sh "$(mktemp -d)" "$PWD/build/gh_cmd_plugin.so" "$PWD/build/gh.so"

The test directory is left for inspection. On systems with a different curl CA
bundle path, set `GH_TEST_CA_BUNDLE` to that path.

## Security note

Release RPMs are fetched over HTTPS but are not GPG-verified (`gpgcheck=0`); only track projects you trust.
