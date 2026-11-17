#!/bin/bash
set -euo pipefail

# The caller supplies an empty disposable directory and the built plugin.
work="$(realpath "$1")"
plugin="$(realpath "$2")"
libplugin="$(realpath "$3")"
test_dir="$(cd "$(dirname "$0")" && pwd)"
arch="$(uname -m)"
plugin_dir="$(rpm --eval '%{_libdir}')/dnf5/plugins"

if [[ -n "$(ls -A "$work")" ]]; then
    echo "Test directory must be empty: $work" >&2
    exit 1
fi

mkdir -p "$work"/{rpm,root,varlib/dnf5-gh/modrinth_code,varlib/dnf/plugins/gh,repoconf,plugins,libplugins,libconf,fixtures}
cp "$plugin" "$work/plugins/gh_cmd_plugin.so"
cp "$libplugin" "$work/libplugins/gh.so"
cp "$test_dir/../gh.conf" "$work/libconf/gh.conf"
mkdir -p "$work/libconf/gh.d"
printf '[main]\ntoken=older-token\n' > "$work/libconf/gh.d/10-old.conf"
printf '[main]\ntoken=integration-test-token\n' > "$work/libconf/gh.d/80-custom.conf"
chmod 600 "$work/libconf/gh.d/10-old.conf" "$work/libconf/gh.d/80-custom.conf"
echo 0.23.1 > "$work/version"
printf '[test]\nname=test\nbaseurl=file:///nonexistent\nenabled=0\ngpgcheck=0\n' > "$work/repoconf/test.repo"
cat > "$work/package.spec" <<'EOF'
Name: modrinth-app
Version: 0.23.1
Release: 1
Summary: Isolated transaction test
License: MIT
AutoReqProv: no
%description
An isolated test package.
%install
mkdir -p %{buildroot}/usr/share/gh-plugin-test
echo test > %{buildroot}/usr/share/gh-plugin-test/installed
%files
/usr/share/gh-plugin-test/installed
%preun -p <lua>
local marker = io.open("/fail-gh-removal", "r")
if marker then marker:close(); error("intentional removal failure") end
EOF
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/modrinth-app-0.23.1-1.$arch.rpm" "$work/fixtures/Modrinth.App-0.23.1-1.$arch.rpm"
cp "$work/fixtures/Modrinth.App-0.23.1-1.$arch.rpm" "$work/varlib/dnf5-gh/modrinth_code/"
touch "$work/varlib/dnf5-gh/modrinth_code/migration-marker"
sed -i 's/^Version: 0.23.1/Version: 0.23.2/' "$work/package.spec"
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/modrinth-app-0.23.2-1.$arch.rpm" "$work/fixtures/Modrinth.App-0.23.2-1.$arch.rpm"
sed -i 's/Name: modrinth-app/Name: Modrinth.Data/; s@gh-plugin-test/installed@gh-plugin-test/data@g' "$work/package.spec"
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/Modrinth.Data-0.23.2-1.$arch.rpm" "$work/fixtures/"
sed -i 's/Name: Modrinth.Data/Name: Unrelated.Test/; s@gh-plugin-test/data@gh-plugin-test/unrelated@g' "$work/package.spec"
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/Unrelated.Test-0.23.2-1.$arch.rpm" "$work/fixtures/"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=api.github.com" \
    -addext "subjectAltName=DNS:api.github.com" -keyout "$work/key.pem" -out "$work/cert.pem" > /dev/null 2>&1
printf '127.0.0.1 api.github.com\n' > "$work/hosts"

unshare -Ur -m -n bash -eu -o pipefail -c '
    work="$1"
    test_dir="$2"
    arch="$3"
    plugin_dir="$4"
    mount --make-rprivate /
    ip link set lo up
    mount --bind "$work/varlib" /var/lib
    mount --bind "$work/repoconf" /etc/yum.repos.d
    mount --bind "$work/plugins" "$plugin_dir"
    mount --bind "$work/hosts" /etc/hosts
    if test -e /etc/dnf/libdnf5-plugins/gh.conf; then
        mount --bind "$work/libconf/gh.conf" /etc/dnf/libdnf5-plugins/gh.conf
    fi
    unset HTTPS_PROXY HTTP_PROXY ALL_PROXY https_proxy http_proxy all_proxy NO_PROXY no_proxy GITHUB_TOKEN GH_TOKEN
    python3 "$test_dir/mock_github.py" "$work" "$arch" &
    server=$!
    trap "kill $server; wait $server || true" EXIT
    for i in {1..50}; do
        if curl --cacert "$work/cert.pem" -fsS https://api.github.com/repos/modrinth/code/releases?per_page=100 >/dev/null 2>&1; then break; fi
        sleep .1
    done
    curl --cacert "$work/cert.pem" -fsS https://api.github.com/repos/modrinth/code/releases?per_page=100 >/dev/null
    touch "$work/require-token"
    options=(--installroot="$work/root" --use-host-config --releasever=45 --setopt=reposdir="$work/repoconf"
             --setopt=pluginpath="$work/libplugins" --setopt=pluginconfpath="$work/libconf"
             --setopt=sslcacert="$work/cert.pem")
    dnf5() {
        timeout --signal=TERM --kill-after=5s 120s dnf5 "$@"
    }
    if dnf5 "${options[@]}" --setopt=sslcacert=/nonexistent --nogpgcheck -y gh install modrinth/code >"$work/invalid-ca.log" 2>&1; then
        echo "Install unexpectedly succeeded with an invalid CA file" >&2
        exit 1
    fi
    grep -q "request to GitHub failed" "$work/invalid-ca.log"
    if dnf5 "${options[@]}" --setopt=proxy=http://127.0.0.1:1 --nogpgcheck -y gh install modrinth/code >"$work/proxy.log" 2>&1; then
        echo "Install unexpectedly succeeded with an unreachable proxy" >&2
        exit 1
    fi
    grep -q "request to GitHub failed" "$work/proxy.log"
    dnf5 "${options[@]}" --nogpgcheck -y gh install --store "$work/stored" modrinth/code
    test -f "$work/stored/transaction.json"
    test ! -e /var/lib/dnf5-gh
    test -f /var/lib/dnf/plugins/gh/modrinth_code/migration-marker
    if rpm --root "$work/root" -q modrinth-app; then
        echo "Stored transaction unexpectedly installed the package" >&2
        exit 1
    fi
    state=/var/lib/dnf/plugins/gh/modrinth_code/state.json
    repo="$work/repoconf/gh:modrinth:code.repo"
    if dnf5 "${options[@]}" --assumeno gh remove modrinth/code; then
        echo "Cancelled removal of uninstalled project unexpectedly succeeded" >&2
        exit 1
    fi
    test -f "$state"
    test -f "$repo"
    dnf5 "${options[@]}" --setopt=tsflags=test -y gh remove modrinth/code
    test -f "$state"
    test -f "$repo"
    dnf5 "${options[@]}" -y gh remove modrinth/code
    test ! -e "$state"
    test ! -e "$repo"
    before_requests=$(wc -l < "$work/requests")
    if dnf5 "${options[@]}" --setopt=sslcacert= --setopt=sslverify=false --nogpgcheck --assumeno gh install modrinth/code >"$work/insecure.log" 2>&1; then
        echo "Declined install unexpectedly succeeded" >&2
        exit 1
    fi
    test "$(wc -l < "$work/requests")" -gt "$before_requests"
    test -s "$work/asset-requests"
    if dnf5 "${options[@]}" --nogpgcheck --assumeno gh install modrinth/code; then
        echo "Declined install unexpectedly succeeded" >&2
        exit 1
    fi
    test ! -e "$state"
    test ! -e "$repo"
    dnf5 "${options[@]}" --nogpgcheck -y gh install --store "$work/stored-again" modrinth/code
    test -s "$work/asset-requests"
    rm "$work/repoconf/gh:modrinth:code.repo"
    dnf5 "${options[@]}" --nogpgcheck -y gh install modrinth/code
    rpm --root "$work/root" -q modrinth-app
    mkdir -p /var/lib/dnf5-gh
    mv /var/lib/dnf/plugins/gh/modrinth_code /var/lib/dnf5-gh/modrinth_code
    sed -i "s@/var/lib/dnf/plugins/gh/@/var/lib/dnf5-gh/@" "$repo"
    dnf5 "${options[@]}" --setopt=gh:modrinth:code.skip_if_unavailable=false \
        --setopt=cachedir="$work/migration-cache" --nogpgcheck -y update
    test ! -e /var/lib/dnf5-gh
    grep -q "^baseurl=file:///var/lib/dnf/plugins/gh/modrinth_code$" "$repo"
    dnf5 "${options[@]}" --setopt=cachedir="$work/migration-cache" repoquery --available modrinth-app |
        grep -q "modrinth-app"
    sed -i "s@/var/lib/dnf/plugins/gh/@/var/lib/dnf5-gh/@" "$repo"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]=0; open(p,\"w\").write(json.dumps(d))" "$state"
    touch "$work/fail"
    dnf5 "${options[@]}" --setopt=gh:modrinth:code.skip_if_unavailable=false \
        --setopt=cachedir="$work/repair-cache" --nogpgcheck -y update 2>"$work/repair-refresh.log"
    grep -q "Warning: GitHub refresh failed" "$work/repair-refresh.log"
    rm "$work/fail"
    grep -q "^baseurl=file:///var/lib/dnf/plugins/gh/modrinth_code$" "$repo"
    dnf5 "${options[@]}" --setopt=cachedir="$work/repair-cache" repoquery --available modrinth-app |
        grep -q "modrinth-app"
    cp -a /var/lib/dnf/plugins/gh/modrinth_code /var/lib/dnf/plugins/gh/other_project
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d.update(owner=\"other\", repo=\"project\"); open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/other_project/state.json
    sed "s@gh:modrinth:code@gh:other:project@; s@/var/lib/dnf/plugins/gh/modrinth_code@/var/lib/dnf5-gh/other_project@" \
        "$repo" > "$work/repoconf/gh:other:project.repo"
    sed -i "s@/var/lib/dnf/plugins/gh/@/var/lib/dnf5-gh/@" "$repo"
    dnf5 "${options[@]}" --setopt=gh:modrinth:code.skip_if_unavailable=false \
        --setopt=gh:other:project.skip_if_unavailable=false \
        --setopt=cachedir="$work/install-repair-cache" --nogpgcheck -y \
        gh install --store "$work/stored-repair" modrinth/code
    grep -q "^baseurl=file:///var/lib/dnf/plugins/gh/modrinth_code$" "$repo"
    grep -q "^baseurl=file:///var/lib/dnf/plugins/gh/other_project$" "$work/repoconf/gh:other:project.repo"
    rm "$work/repoconf/gh:other:project.repo"
    rm -r /var/lib/dnf/plugins/gh/other_project
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" modrinth-app)" = 0.23.1
    test "$(dnf5 "${options[@]}" repoquery --installed --queryformat "%{from_repo}" modrinth-app)" = "gh:modrinth:code"
    rm /var/lib/dnf/plugins/gh/modrinth_code/repodata/gh-assets.json
    cp "$work/fixtures/Modrinth.App-0.23.1-1.$arch.rpm" /var/lib/dnf/plugins/gh/modrinth_code/
    dnf5 "${options[@]}" --nogpgcheck -y update
    test -f /var/lib/dnf/plugins/gh/modrinth_code/repodata/gh-assets.json
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    dnf5 "${options[@]}" gh list > "$work/list-initial.out"
    grep -q "^  packages:   modrinth-app$" "$work/list-initial.out"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"packages\"]=[\"Modrinth.App\"]; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/modrinth_code/state.json
    dnf5 "${options[@]}" gh list > "$work/list-legacy.out"
    grep -q "^  packages:   modrinth-app$" "$work/list-legacy.out"
    grep -q "^metadata_expire=0$" "$work/repoconf/gh:modrinth:code.repo"
    sed "s/^\\[gh:modrinth:code\\]/[gh-modrinth_code]/; s/^enabled=1/enabled=0/" \
        "$work/repoconf/gh:modrinth:code.repo" > "$work/repoconf/gh-modrinth_code.repo"
    rm "$work/repoconf/gh:modrinth:code.repo"
    before_migration=$(wc -l < "$work/requests")
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before_migration"
    test ! -f "$work/repoconf/gh-modrinth_code.repo"
    grep -q "^enabled=0$" "$work/repoconf/gh:modrinth:code.repo"
    sed -i "s/^enabled=0/enabled=1/" "$work/repoconf/gh:modrinth:code.repo"
    echo 0.23.2 > "$work/version"
    before=$(wc -l < "$work/requests")
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" modrinth-app)" = 0.23.1
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/modrinth_code/state.json
    sed -i "s/metadata_expire=0/metadata_expire=never/" "$work/repoconf/gh:modrinth:code.repo"
    dnf5 "${options[@]}" --cacheonly --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before"
    dnf5 "${options[@]}" --disablerepo=gh:modrinth:code --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before"
    before_metadata=$(sha256sum /var/lib/dnf/plugins/gh/modrinth_code/repodata/repomd.xml)
    before_state=$(sha256sum /var/lib/dnf/plugins/gh/modrinth_code/state.json)
    touch "$work/fail-asset"
    dnf5 "${options[@]}" --nogpgcheck -y update 2>"$work/failed-download.log"
    grep -q "Warning: GitHub refresh failed" "$work/failed-download.log"
    test "$(sha256sum /var/lib/dnf/plugins/gh/modrinth_code/repodata/repomd.xml)" = "$before_metadata"
    test "$(sha256sum /var/lib/dnf/plugins/gh/modrinth_code/state.json)" = "$before_state"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" modrinth-app)" = 0.23.1
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    rm "$work/fail-asset"
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" modrinth-app)" = 0.23.2
    test ! -e "/var/lib/dnf/plugins/gh/modrinth_code/Modrinth.App-0.23.1-1.$arch.rpm"
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    test "$(wc -l < "$work/requests")" -eq "$((before + 2))"
    dnf5 "${options[@]}" --nogpgcheck -y upgrade
    test "$(wc -l < "$work/requests")" -eq "$((before + 2))"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/modrinth_code/state.json
    touch "$work/fail"
    dnf5 "${options[@]}" --nogpgcheck -y update 2>"$work/failed-update.log"
    grep -q "Warning: GitHub refresh failed" "$work/failed-update.log"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" modrinth-app)" = 0.23.2
    rm "$work/fail"
    dnf5 "${options[@]}" gh update modrinth/code
    after=$(wc -l < "$work/requests")
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$after"
    dnf5 "${options[@]}" gh update modrinth/code
    test "$(wc -l < "$work/requests")" -eq "$((after + 1))"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/modrinth_code/state.json
    dnf5 "${options[@]}" gh list > "$work/list.out"
    test "$(wc -l < "$work/requests")" -eq "$((after + 2))"
    grep -q "^modrinth/code$" "$work/list.out"
    grep -q "^  release:    v0.23.2$" "$work/list.out"
    grep -q "^  prerelease: no$" "$work/list.out"
    grep -q "^  packages:   modrinth-app$" "$work/list.out"
    grep -q "^  repo id:    gh:modrinth:code$" "$work/list.out"
    grep -q "^  location:   " "$work/list.out"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf/plugins/gh/modrinth_code/state.json
    dnf5 "${options[@]}" --nogpgcheck -y upgrade
    test "$(wc -l < "$work/requests")" -eq "$((after + 3))"
    if dnf5 "${options[@]}" -y gh remove --store "$work/stored" modrinth/code; then
        echo "gh remove --store should have failed" >&2
        exit 1
    fi
    test -f /var/lib/dnf/plugins/gh/modrinth_code/state.json
    rpm --root "$work/root" -q modrinth-app
    state=/var/lib/dnf/plugins/gh/modrinth_code/state.json
    repo="$work/repoconf/gh:modrinth:code.repo"
    before_state=$(sha256sum "$state")
    if dnf5 "${options[@]}" --assumeno gh remove modrinth/code; then
        echo "Cancelled gh removal unexpectedly succeeded" >&2
        exit 1
    fi
    test "$(sha256sum "$state")" = "$before_state"
    test -f "$repo"
    rpm --root "$work/root" -q modrinth-app
    if dnf5 "${options[@]}" --setopt=skip_system_repo_lock=true -y gh remove modrinth/code; then
        echo "gh removal without the system lock unexpectedly succeeded" >&2
        exit 1
    fi
    if dnf5 "${options[@]}" --setopt=persistence=transient -y gh remove modrinth/code; then
        echo "gh removal with transient history unexpectedly succeeded" >&2
        exit 1
    fi
    test "$(sha256sum "$state")" = "$before_state"
    test -f "$repo"
    rpm --root "$work/root" -q modrinth-app
    dnf5 "${options[@]}" --setopt=tsflags=test -y gh remove modrinth/code
    test "$(sha256sum "$state")" = "$before_state"
    test -f "$repo"
    rpm --root "$work/root" -q modrinth-app
    touch "$work/root/fail-gh-removal"
    if dnf5 "${options[@]}" -y gh remove modrinth/code >"$work/failed-gh-removal.log" 2>&1; then
        echo "Failed gh removal unexpectedly succeeded" >&2
        exit 1
    fi
    grep -q "intentional removal failure" "$work/failed-gh-removal.log"
    test "$(sha256sum "$state")" = "$before_state"
    test -f "$repo"
    rpm --root "$work/root" -q modrinth-app
    rm "$work/root/fail-gh-removal"
    dnf5 "${options[@]}" -y gh remove modrinth/code
    test ! -e "$state"
    test ! -e "$repo"
    test ! -d /var/lib/dnf/plugins/gh/modrinth_code
    if rpm --root "$work/root" -q modrinth-app; then
        echo "Package was not removed" >&2
        exit 1
    fi

    touch "$work/two-assets"
    dnf5 "${options[@]}" --nogpgcheck -y gh install --all modrinth/code
    rpm --root "$work/root" -q modrinth-app Modrinth.Data
    state=/var/lib/dnf/plugins/gh/modrinth_code/state.json
    repo="$work/repoconf/gh:modrinth:code.repo"
    before_state=$(sha256sum "$state")
    cp "$work/fixtures/Modrinth.App-0.23.2-1.$arch.rpm" /var/lib/dnf/plugins/gh/modrinth_code/stale.rpm
    dnf5 "${options[@]}" --nogpgcheck -y install "$work/fixtures/Unrelated.Test-0.23.2-1.$arch.rpm"
    test ! -e /var/lib/dnf/plugins/gh/modrinth_code/stale.rpm
    dnf5 "${options[@]}" -y remove Unrelated.Test
    test "$(sha256sum "$state")" = "$before_state"
    if dnf5 "${options[@]}" --assumeno remove modrinth-app; then
        echo "Cancelled removal unexpectedly succeeded" >&2
        exit 1
    fi
    test "$(sha256sum "$state")" = "$before_state"
    dnf5 "${options[@]}" -y remove --store "$work/stored-remove" modrinth-app
    test -f "$work/stored-remove/transaction.json"
    test "$(sha256sum "$state")" = "$before_state"
    touch "$work/root/fail-gh-removal"
    if dnf5 "${options[@]}" -y remove Modrinth.Data >"$work/failed-removal.log" 2>&1; then
        echo "Failed RPM removal unexpectedly succeeded" >&2
        exit 1
    fi
    grep -q "intentional removal failure" "$work/failed-removal.log"
    test "$(sha256sum "$state")" = "$before_state"
    rm "$work/root/fail-gh-removal"
    dnf5 "${options[@]}" --setopt=skip_system_repo_lock=true -y remove modrinth-app 2>"$work/skip-lock.log"
    grep -q "GitHub tracking update skipped" "$work/skip-lock.log"
    test "$(sha256sum "$state")" = "$before_state"
    dnf5 "${options[@]}" --nogpgcheck -y install modrinth-app
    dnf5 "${options[@]}" -y remove modrinth-app
    test -f "$state"
    test -f "$repo"
    python3 -c "import json,sys; assert json.load(open(sys.argv[1]))[\"packages\"] == [\"Modrinth.Data\"]" "$state"
    rpm --root "$work/root" -q Modrinth.Data
    before_assets=$(wc -l < "$work/asset-requests")
    if dnf5 "${options[@]}" --setopt=cachedir="$work/fresh-cache" --nogpgcheck --assumeno install modrinth-app; then
        echo "Cancelled installation unexpectedly succeeded" >&2
        exit 1
    fi
    test "$(wc -l < "$work/asset-requests")" -eq "$before_assets"
    touch "$work/fail-asset"
    if dnf5 "${options[@]}" --setopt=cachedir="$work/fresh-cache" --nogpgcheck -y install modrinth-app; then
        echo "Installation with a failed asset download unexpectedly succeeded" >&2
        exit 1
    fi
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    rm "$work/fail-asset"
    before_assets=$(wc -l < "$work/asset-requests")
    dnf5 "${options[@]}" --setopt=cachedir="$work/fresh-cache" --nogpgcheck -y install modrinth-app
    test "$(wc -l < "$work/asset-requests")" -eq "$((before_assets + 1))"
    test -z "$(find /var/lib/dnf/plugins/gh/modrinth_code -type f -name "*.rpm" -print)"
    python3 -c "import json,sys; assert set(json.load(open(sys.argv[1]))[\"packages\"]) == {\"modrinth-app\", \"Modrinth.Data\"}" "$state"
    dnf5 "${options[@]}" gh update modrinth/code
    dnf5 "${options[@]}" -y remove Modrinth.Data
    python3 -c "import json,sys; assert json.load(open(sys.argv[1]))[\"packages\"] == [\"modrinth-app\"]" "$state"
    dnf5 "${options[@]}" -y remove modrinth-app
    test ! -e "$state"
    test ! -e "$repo"
    test ! -d /var/lib/dnf/plugins/gh/modrinth_code
' _ "$work" "$test_dir" "$arch" "$plugin_dir"
