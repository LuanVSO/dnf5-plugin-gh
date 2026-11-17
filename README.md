# dnf5-plugin-gh

A dnf5 plugin that installs and updates RPMs published as GitHub release assets.

Each tracked project gets local `createrepo_c` metadata under
`/var/lib/dnf/plugins/gh/<owner>_<repo>/` and a repo file `/etc/yum.repos.d/gh:<owner>:<repo>.repo`,
so dnf5 resolves, upgrades and removes those packages natively.
Data in the previous `/var/lib/dnf5-gh/` location is migrated automatically on
the first eligible root invocation.
Repository configurations still pointing to the old location are also repaired,
including configurations left behind by an earlier migration.
The metadata points to GitHub release assets: DNF downloads the packages needed
for a transaction, and the local repository does not retain RPM files.
New installs are recorded as originating from `gh:<owner>:<repo>`. A slash
cannot be used in a dnf repository ID. Existing `gh-<owner>_<repo>` files are
migrated when the project is refreshed or installed again; existing installed
package origins are not rewritten.

## Commands

| Command | Description |
|---|---|
| `dnf5 gh install <owner/repo>... [--prerelease] [--all] [--pattern GLOB]` | Install RPMs from the newest release with an RPM for this architecture (`x86_64`/`noarch`). Prompts when several packages are available (`--all` takes all). `--pattern` selects assets by file-name glob (bypassing architecture detection) without prompting and is remembered, so upgrades keep following the same assets. |
| `dnf5 gh update [owner/repo...]` | Refresh tracked projects from GitHub, then run `dnf5 upgrade` to apply. |
| `dnf5 gh list [owner/repo...]` | List tracked projects with release, prerelease, package, repo ID, and local path details. |
| `dnf5 gh remove <owner/repo>...` | Remove the installed packages and stop tracking. |

Package names come from RPM headers, not release asset filenames. Compatible
assets are downloaded before selection so their package names can be identified.
Older filename-based tracking entries are corrected using cached RPMs when
available, and saved on the next tracking update.

Every eligible dnf5 invocation automatically checks each enabled tracked
GitHub repository at most once every 24 hours. Repositories are refreshed
before resolving transactions, so available updates can be installed normally.
`dnf5 gh update` forces an immediate check, regardless of the interval.
When the selected release tag is unchanged and remote metadata exists, the
check does not download RPMs or rebuild metadata. Changes to assets under the
same tag are therefore not detected until the metadata is rebuilt.
If preparing a refresh fails, a warning is printed and the previous repository
remains available; the next update retries. Existing repositories are migrated
to remote asset locations on their next successful refresh; their old RPMs are
retained until then so their existing metadata continues to work. Clients using
cached older metadata may need to refresh it before downloading packages.
DNF's own package cache is controlled separately by its `keepcache` setting.

Automatic refresh requires the enabled libdnf5 `gh` plugin. It is skipped for
non-root runs, `--cacheonly`, disabled repositories, and alternate installroots
unless `--use-host-config` is supplied.
With `--use-host-config`, tracked repositories and metadata are shared with the
host. Generated local repositories use `metadata_expire=0`: dnf checks the
local metadata on each use, while GitHub itself is queried only after the
24-hour interval.

Set `GITHUB_TOKEN` (or `GH_TOKEN`) to avoid API rate limits; use `sudo -E` to keep it.
Alternatively, create a file such as `/etc/dnf/libdnf5-plugins/gh.d/token.conf`,
owned by root with permissions `0600`, containing:

    [main]
    token = YOUR_TOKEN

Create the `gh.d` directory owned by root with permissions `0700` before adding
the file, so the token is protected while the file is being created.
Environment variables take precedence over this file. The libdnf plugin config
`/etc/dnf/libdnf5-plugins/gh.conf` must be readable by all users and must not
contain a token. The plugin reads `gh.d/*.conf` from DNF's `plugin_conf_dir`
directories (or `pluginconfpath` when configured). Files are processed by
filename in alphabetical order; later token settings override earlier ones.
If the same filename occurs in multiple directories, the first directory wins,
as with DNF configuration drop-ins. The existing `gh.d/token.conf` remains
supported. A token in a drop-in overrides the legacy `gh.conf` token.

If upgrading from a version that stored a token in `gh.conf`, move the token to
the private file **before upgrading or changing the permissions**. Remove the
token from `gh.conf`, then run `sudo chmod 644 /etc/dnf/libdnf5-plugins/gh.conf`
to fix existing installations. The old token location is still read for
compatibility, but a token left there will be exposed if the file is readable
by other users. RPM may preserve an existing modified configuration on upgrade;
check and update its permissions after removing the token.

GitHub API and asset requests use DNF's main network settings for the proxy,
proxy authentication, TLS verification and certificates, IP resolution, timeout
and minimum transfer rate. These settings also apply to automatic refreshes.
Disabling `sslverify` disables certificate and hostname verification for GitHub
requests; configured client certificates may be presented to remote servers.

`gh install` also supports dnf5's `--store PATH` option. It prepares the local
repository and records the project even though the transaction is only stored,
not executed. `--store` is not supported by `gh remove`.

The enabled libdnf5 `gh` plugin reconciles successful transaction installs and
removals from tracked GitHub repositories, including transactions initiated by
other libdnf5 clients such as PackageKit. Newly installed packages are added to
tracking; removing the last tracked package deletes the repository configuration
and local tracking data. Cancelled, stored and failed transactions do not change
tracking. Tracking updates are skipped with a warning if the system repository
lock is disabled, or for alternate installroots without `--use-host-config`.
Existing packages recorded as `@commandline` cannot be identified by repository
origin.

`gh remove` deletes tracking data only after the removal transaction succeeds.
Cancelling or failing the transaction leaves the repository tracked. It requires
the system repository lock and persistent transaction history. If none of the
tracked packages are installed, it asks before removing the tracking data.

## Build

Requires `libdnf5-devel libdnf5-cli-devel dnf5-devel libcurl-devel json-devel rpm-devel`; `createrepo_c` at runtime.

    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc -DDNF5_PLUGIN_DIR="$(rpm --eval '%{_libdir}')/dnf5/plugins"
    cmake --build build && ctest --test-dir build
    sudo cmake --install build

An optional isolated transaction test requires user, mount and network
namespaces, `rpmbuild`, `createrepo_c`, `ip`, `openssl`, `curl` and Python 3.
Pass an empty disposable directory and the built plugin:

    bash tests/test_transactions.sh "$(mktemp -d)" "$PWD/build/gh_cmd_plugin.so" "$PWD/build/gh.so"

The test directory is left for inspection.

## Security note

Release RPMs are fetched over HTTPS but are not GPG-verified by default (`gpgcheck=0`); only track projects you trust.
`gh install` displays a warning before installation.
