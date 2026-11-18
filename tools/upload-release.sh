#!/usr/bin/env bash
# Attach the packaged binaries to the GitHub release.
#
# The release and its assets cannot be created from this machine without a
# credential, and Actions on this account are blocked by a billing lock, so
# the workflow that would have published them cannot run. This is what is
# left: run it once with a logged-in gh.
#
#   gh auth login                  # once, interactive
#   ./tools/upload-release.sh      # afterwards, any time
set -euo pipefail

VERSION="${1:-1.0.0}"
TAG="v$VERSION"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS=(
  "$ROOT/release/devinfo-$VERSION-windows-x64.zip"
  "$ROOT/release/devinfo-$VERSION-windows-x86.zip"
  "$ROOT/release/devinfo-$VERSION-macos-universal.dmg"
  "$ROOT/release/devinfo-$VERSION-macos-universal.zip"
  "$ROOT/release/devinfo-$VERSION-linux-x86_64.tar.gz"
  "$ROOT/release/devinfo-$VERSION-linux-aarch64.tar.gz"
)

command -v gh >/dev/null || { echo "gh is not installed: https://cli.github.com"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "not logged in - run: gh auth login"; exit 1; }

for f in "${ASSETS[@]}"; do
  [ -f "$f" ] || { echo "missing: $f"; exit 1; }
done

REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"

# The release usually already exists and is empty, so edit it rather than
# failing on "already exists".
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "release $TAG exists, updating"
  gh release edit "$TAG" --repo "$REPO" \
    --title "devinfo $VERSION" \
    --notes-file "$ROOT/release/RELEASE_BODY.md"
else
  echo "creating release $TAG"
  gh release create "$TAG" --repo "$REPO" \
    --title "devinfo $VERSION" \
    --notes-file "$ROOT/release/RELEASE_BODY.md"
fi

echo "uploading ${#ASSETS[@]} assets"
gh release upload "$TAG" "${ASSETS[@]}" --repo "$REPO" --clobber

echo
gh release view "$TAG" --repo "$REPO" --json assets \
  -q '.assets[] | "  \(.name)  \(.size) bytes"'
