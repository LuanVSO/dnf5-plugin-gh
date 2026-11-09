#!/bin/bash
set -euo pipefail

# The caller supplies an empty disposable directory and the built plugin.
work="$(realpath "$1")"
plugin="$(realpath "$2")"
libplugin="$(realpath "$3")"
test_dir="$(cd "$(dirname "$0")" && pwd)"
arch="$(uname -m)"
plugin_dir="$(rpm --eval '%{_libdir}')/dnf5/plugins"
ca_bundle="${GH_TEST_CA_BUNDLE:-/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem}"

if [[ -n "$(ls -A "$work")" ]]; then
    echo "Test directory must be empty: $work" >&2
    exit 1
fi

mkdir -p "$work"/{rpm,root,varlib/dnf5-gh/modrinth_code,repoconf,plugins,libplugins,libconf,fixtures}
cp "$plugin" "$work/plugins/gh_cmd_plugin.so"
cp "$libplugin" "$work/libplugins/gh.so"
cp "$test_dir/../gh.conf" "$work/libconf/gh.conf"
echo 0.23.1 > "$work/version"
printf '[test]\nname=test\nbaseurl=file:///nonexistent\nenabled=0\ngpgcheck=0\n' > "$work/repoconf/test.repo"
cat > "$work/package.spec" <<'EOF'
Name: Modrinth.App
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
EOF
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/Modrinth.App-0.23.1-1.$arch.rpm" "$work/fixtures/"
cp "$work/fixtures/Modrinth.App-0.23.1-1.$arch.rpm" "$work/varlib/dnf5-gh/modrinth_code/"
sed -i 's/^Version: 0.23.1/Version: 0.23.2/' "$work/package.spec"
rpmbuild -bb --target "$arch" --define "_topdir $work/rpm" "$work/package.spec"
cp "$work/rpm/RPMS/$arch/Modrinth.App-0.23.2-1.$arch.rpm" "$work/fixtures/"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=api.github.com" \
    -addext "subjectAltName=DNS:api.github.com" -keyout "$work/key.pem" -out "$work/cert.pem" > /dev/null 2>&1
printf '127.0.0.1 api.github.com\n' > "$work/hosts"

unshare -Ur -m -n bash -eu -o pipefail -c '
    work="$1"
    test_dir="$2"
    arch="$3"
    plugin_dir="$4"
    ca_bundle="$5"
    mount --make-rprivate /
    ip link set lo up
    mount --bind "$work/varlib" /var/lib
    mount --bind "$work/repoconf" /etc/yum.repos.d
    mount --bind "$work/plugins" "$plugin_dir"
    mount --bind "$work/hosts" /etc/hosts
    mount --bind "$work/cert.pem" "$ca_bundle"
    unset HTTPS_PROXY HTTP_PROXY ALL_PROXY https_proxy http_proxy all_proxy GITHUB_TOKEN GH_TOKEN
    python3 "$test_dir/mock_github.py" "$work" "$arch" &
    server=$!
    trap "kill $server; wait $server || true" EXIT
    for i in {1..50}; do
        if curl -fsS https://api.github.com/repos/modrinth/code/releases?per_page=100 >/dev/null 2>&1; then break; fi
        sleep .1
    done
    curl -fsS https://api.github.com/repos/modrinth/code/releases?per_page=100 >/dev/null
    options=(--installroot="$work/root" --use-host-config --releasever=45 --setopt=reposdir="$work/repoconf"
             --setopt=pluginpath="$work/libplugins" --setopt=pluginconfpath="$work/libconf")
    dnf5 "${options[@]}" --nogpgcheck -y gh-install --store "$work/stored" modrinth/code
    test -f "$work/stored/transaction.json"
    if rpm --root "$work/root" -q Modrinth.App; then
        echo "Stored transaction unexpectedly installed the package" >&2
        exit 1
    fi
    rm "$work/repoconf/gh:modrinth:code.repo"
    dnf5 "${options[@]}" --nogpgcheck -y gh-install modrinth/code
    rpm --root "$work/root" -q Modrinth.App
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" Modrinth.App)" = 0.23.1
    test "$(dnf5 "${options[@]}" repoquery --installed --queryformat "%{from_repo}" Modrinth.App)" = "gh:modrinth:code"
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
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" Modrinth.App)" = 0.23.1
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf5-gh/modrinth_code/state.json
    sed -i "s/metadata_expire=0/metadata_expire=never/" "$work/repoconf/gh:modrinth:code.repo"
    dnf5 "${options[@]}" --cacheonly --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before"
    dnf5 "${options[@]}" --disablerepo=gh:modrinth:code --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$before"
    before_metadata=$(sha256sum /var/lib/dnf5-gh/modrinth_code/repodata/repomd.xml)
    before_state=$(sha256sum /var/lib/dnf5-gh/modrinth_code/state.json)
    touch "$work/fail-asset"
    dnf5 "${options[@]}" --nogpgcheck -y update 2>"$work/failed-download.log"
    grep -q "Warning: GitHub refresh failed" "$work/failed-download.log"
    test "$(sha256sum /var/lib/dnf5-gh/modrinth_code/repodata/repomd.xml)" = "$before_metadata"
    test "$(sha256sum /var/lib/dnf5-gh/modrinth_code/state.json)" = "$before_state"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" Modrinth.App)" = 0.23.1
    rm "$work/fail-asset"
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" Modrinth.App)" = 0.23.2
    test "$(wc -l < "$work/requests")" -eq "$((before + 2))"
    dnf5 "${options[@]}" --nogpgcheck -y upgrade
    test "$(wc -l < "$work/requests")" -eq "$((before + 2))"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf5-gh/modrinth_code/state.json
    touch "$work/fail"
    dnf5 "${options[@]}" --nogpgcheck -y update 2>"$work/failed-update.log"
    grep -q "Warning: GitHub refresh failed" "$work/failed-update.log"
    test "$(rpm --root "$work/root" -q --qf "%{VERSION}" Modrinth.App)" = 0.23.2
    rm "$work/fail"
    dnf5 "${options[@]}" gh-update modrinth/code
    after=$(wc -l < "$work/requests")
    dnf5 "${options[@]}" --nogpgcheck -y update
    test "$(wc -l < "$work/requests")" = "$after"
    dnf5 "${options[@]}" gh-update modrinth/code
    test "$(wc -l < "$work/requests")" -eq "$((after + 1))"
    python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d[\"last_checked\"]-=86400; open(p,\"w\").write(json.dumps(d))" /var/lib/dnf5-gh/modrinth_code/state.json
    dnf5 "${options[@]}" --nogpgcheck -y upgrade
    test "$(wc -l < "$work/requests")" -eq "$((after + 2))"
    if dnf5 "${options[@]}" -y gh-remove --store "$work/stored" modrinth/code; then
        echo "gh-remove --store should have failed" >&2
        exit 1
    fi
    test -f /var/lib/dnf5-gh/modrinth_code/state.json
    rpm --root "$work/root" -q Modrinth.App
    dnf5 "${options[@]}" -y gh-remove modrinth/code
    if rpm --root "$work/root" -q Modrinth.App; then
        echo "Package was not removed" >&2
        exit 1
    fi
' _ "$work" "$test_dir" "$arch" "$plugin_dir" "$ca_bundle"
