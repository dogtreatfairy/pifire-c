# Cutting a release

1. Bump `project(pifire VERSION x.y.z)` in `CMakeLists.txt` (keeps `--version` right for local builds) and commit.
2. Tag and push: `git tag v1.2.3 && git push origin v1.2.3`.
3. `.github/workflows/release.yml` builds `arm64` and `armhf` binaries in Debian Bookworm containers (QEMU), packages `pifire-1.2.3-arm64.tar.gz` / `pifire-1.2.3-armhf.tar.gz` plus `SHA256SUMS`, and publishes a GitHub Release with auto-generated notes. Edit the notes afterwards if you like — the OTA updater shows the release body under "What's new".
4. Every grill whose `settings.update.repo` points at this repository will see the release on its next check and can install it from *Settings → System → Software updates*.

Asset names are a contract: the updater looks for `pifire-<tag without v>-<arch>.tar.gz` and `SHA256SUMS`. Tags with a suffix (`v0.1.0-alpha.1`, `v1.3.0-rc.1`) are published as GitHub pre-releases automatically; they sort below the final release of the same number (`alpha.1 < alpha.2 < beta.1 < rc.1 < 1.3.0`). Grills with `update.include_prerelease` on (the default during alpha) see them; turn it off to receive only final releases.

`.github/workflows/ci.yml` builds and runs the test suite on every push.
