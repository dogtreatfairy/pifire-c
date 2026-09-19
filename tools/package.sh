#!/bin/bash
# package.sh <arch> <version> [build-dir]  ->  dist/pifire-<version>-<arch>.tar.gz
# The archive is what the OTA updater and `install/install.sh` consume: binary, installer, share/, docs/.
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH="${1:?arch (arm64|armhf|amd64)}"; VER="${2:?version}"; BUILD="${3:-build}"
[ -x "$BUILD/pifired" ] || { echo "$BUILD/pifired missing"; exit 1; }
NAME="pifire-$VER-$ARCH"
rm -rf "dist/$NAME" && mkdir -p "dist/$NAME"
cp "$BUILD/pifired" "dist/$NAME/pifired"
strip "dist/$NAME/pifired" 2>/dev/null || true
cp -r install share docs README.md "dist/$NAME/"
[ -f LICENSE ] && cp LICENSE "dist/$NAME/"
echo "$VER" > "dist/$NAME/VERSION"
tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
rm -rf "dist/$NAME"
(cd dist && sha256sum "$NAME.tar.gz")
