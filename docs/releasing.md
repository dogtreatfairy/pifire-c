# Cutting a release

1. Bump `project(pifire VERSION x.y.z)` in `CMakeLists.txt` (keeps `--version` right for local builds) and commit.
2. Tag and push: `git tag v1.2.3 && git push origin v1.2.3`.
3. `.github/workflows/release.yml` builds `arm64` and `armhf` binaries in Debian Bookworm containers (QEMU), packages `pifire-1.2.3-arm64.tar.gz` / `pifire-1.2.3-armhf.tar.gz` plus `SHA256SUMS`, and publishes a GitHub Release with auto-generated notes. Edit the notes afterwards if you like — the OTA updater shows the release body under "What's new".
4. Every grill whose `settings.update.repo` points at this repository will see the release on its next check and can install it from *More → System → Software*.

Asset names are a contract: the updater looks for `pifire-<tag without v>-<arch>.tar.gz` and `SHA256SUMS`. Pre-release tags (`v1.3.0-rc1`) sort below the final release and GitHub's `releases/latest` skips releases marked pre-release, so use that flag for test builds.

`.github/workflows/ci.yml` builds and runs the test suite on every push.
