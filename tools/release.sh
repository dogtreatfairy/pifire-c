#!/bin/bash
# Build release binaries for the Pi inside Debian containers (needs podman or docker with
# qemu-user-static/binfmt for foreign architectures). Output: dist/pifired-<arch>
set -euo pipefail
cd "$(dirname "$0")/.."
RUNTIME=${RUNTIME:-$(command -v podman || command -v docker || true)}
[ -n "$RUNTIME" ] || { echo "podman or docker required"; exit 1; }
mkdir -p dist
for arch in arm64v8 arm32v7; do
	echo "== $arch"
	$RUNTIME run --rm -v "$PWD":/src -w /src "$arch/debian:bookworm-slim" bash -c '
		set -e
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends cmake gcc make pkg-config libsqlite3-dev libmosquitto-dev libcurl4-openssl-dev libsystemd-dev gzip >/dev/null
		rm -rf build-rel && cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DPF_BUILD_TESTS=OFF >/dev/null
		cmake --build build-rel -j"$(nproc)"
		strip build-rel/pifired
		cp build-rel/pifired dist/pifired-'"$arch"'
	'
done
ls -la dist/
