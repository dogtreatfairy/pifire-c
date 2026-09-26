#!/bin/sh
# Every JavaScript module the build embeds must be in the service worker's cache lists. A module
# that is imported but never cached loads in 20 ms on the LAN and hangs for ever over a tunnel
# that is still waking up, and because app.js imports every page at start, one such module is
# the whole app failing to load. This was found the hard way over Tailscale.
cd "$(dirname "$0")/.." || exit 1
rc=0
for f in $(tr ' ' '\n' < CMakeLists.txt | grep -E '^(pages/[a-z]+\.js|[a-z]+\.js)$' | grep -v '^sw\.js$' | sort -u); do
	grep -q "'/$f'" web/sw.js || { echo "web/sw.js does not cache $f"; rc=1; }
done
exit $rc
