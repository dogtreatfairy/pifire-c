#!/bin/sh
# The whole gate a tag has to pass, in one place, exactly as GitHub runs it: the sanitised Debug
# build and its tests (what CI does on every push), the native Release build, and the two arm
# Release builds in the containers the release workflow uses. Run it before tagging; a tag pushed
# on a tree that fails any of these is a red run on GitHub for everyone to see.
#
#   tools/preflight.sh <version>        e.g. tools/preflight.sh 0.1.0-alpha.136
#
# Exits non-zero at the first failure, saying which.
set -u
VER="${1:-0.0.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
fail() { echo "PREFLIGHT FAILED: $*" >&2; exit 1; }

echo "+ shell checks"
sh tests/check_sw_cache.sh || fail "service worker cache list"
python3 tools/icons.py --check || fail "icons out of date"
[ -f web/_probe.js ] && fail "web/_probe.js is still in the tree"
grep -q "_probe\.js" web/index.html CMakeLists.txt && fail "_probe.js still referenced"

echo "+ sanitised build and tests (as CI)"
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPF_SANITIZE=ON >/dev/null || fail "asan configure"
cmake --build build-asan -j"$(nproc)" 2>&1 | grep -E "error|warning" && fail "asan build has warnings or errors"
ctest --test-dir build-asan --output-on-failure -j4 2>&1 | tee /tmp/pf-asan-ctest.log | grep -qE "100% tests passed" || fail "sanitised tests (see /tmp/pf-asan-ctest.log)"

echo "+ native release build"
cmake -B build -DPF_VERSION="$VER" >/dev/null || fail "configure"
cmake --build build -j"$(nproc)" 2>&1 | grep -E "error|warning" && fail "build has warnings or errors"
ctest --test-dir build -j4 2>&1 | grep -qE "100% tests passed" || fail "tests"

for arch in "arm64 linux/arm64 arm64v8/debian:bookworm-slim" "armhf linux/arm/v7 arm32v7/debian:bookworm-slim"; do
  set -- $arch
  echo "+ $1 release build (container)"
  podman run --rm --platform "$2" -v "$ROOT":/src -w /src "$3" bash -euc '
    apt-get update -qq >/dev/null; apt-get install -y -qq --no-install-recommends cmake gcc make pkg-config libsqlite3-dev libmosquitto-dev libcurl4-openssl-dev libssl-dev libsystemd-dev gzip tar binutils >/dev/null 2>&1
    cmake -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DPF_BUILD_TESTS=OFF -DPF_VERSION="'"$VER"'" >/dev/null
    cmake --build /tmp/b -j"$(nproc)" >/tmp/b.log 2>&1 || { grep -E "error" -A3 /tmp/b.log | head -20; exit 1; }' || fail "$1 release build"
done
echo "PREFLIGHT OK $VER"
