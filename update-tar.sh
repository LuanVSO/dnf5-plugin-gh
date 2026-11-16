
tar -czf "dnf5-plugin-gh.tar.gz" \
  --transform='s,^,dnf5-plugin-gh/,' \
  CMakeLists.txt README.md gh.conf src tests
fedpkg --release f$(rpm -E '%fedora') mockbuild -N
