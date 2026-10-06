#!/usr/bin/env bash
# Cut a DECK//OS release from main.
#
#   tools/release.sh 0.6.0 notes.md ["Release title"]
#
# Sets the version (PROJECT_VER + DECK_VERSION), commits, builds, makes the
# two images, tags vX.Y.Z and publishes a GitHub release with:
#   tab5-cyberdeck-vX.Y.Z-full.bin  USB / web flasher, written at 0x0
#   tab5-cyberdeck-vX.Y.Z-ota.bin   app image for over-the-air updates
# Publishing the release triggers the web flasher deploy (.github/workflows/pages.yml).
#
# The app is signed with the OTA signing key (RSA-3072, Secure Boot v2 format)
# before the images are made: a deck running a signed build only installs
# updates signed with the same key. Keep the key out of the repo and backed
# up; without it, signed decks can still be updated over USB, not over the air.
#   DECK_SIGNING_KEY  default ~/.config/deck-os/ota_signing_key.pem
#   --prerelease      as the 4th argument publishes a beta (version X.Y.Z-beta.N)
set -euo pipefail

ver="${1:?usage: $0 X.Y.Z notes.md}"
notes="${2:?usage: $0 X.Y.Z notes.md}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

pre="${4:-}"
key="${DECK_SIGNING_KEY:-$HOME/.config/deck-os/ota_signing_key.pem}"
if [[ "$pre" == "--prerelease" ]]; then
    [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9A-Za-z.]+$ ]] || { echo "beta version must be X.Y.Z-tag"; exit 1; }
else
    [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be X.Y.Z"; exit 1; }
    [[ "$(git branch --show-current)" == "main" ]] || { echo "release from main"; exit 1; }
fi
[[ -f "$key" ]] || { echo "no signing key at $key (set DECK_SIGNING_KEY)"; exit 1; }
[[ -z "$(git status --porcelain)" ]] || { echo "working tree not clean"; exit 1; }
[[ -f "$notes" ]] || { echo "no notes file: $notes"; exit 1; }

sed -i '' -E "s/set\(PROJECT_VER \"[^\"]*\"\)/set(PROJECT_VER \"$ver\")/" CMakeLists.txt
sed -i '' -E "s/#define DECK_VERSION \"[^\"]*\"/#define DECK_VERSION \"$ver\"/" components/deck_core/include/deck_shell.h
git diff --quiet || git commit -qam "v$ver"   # re-runs after a failed attempt keep the commit

# shellcheck disable=SC1091
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1
idf.py build >/dev/null
# Sign the app in place, then build the full image from the signed app.
espsecure.py sign_data --version 2 --keyfile "$key" --output build/app-signed.bin build/tab5_cyberdeck.bin >/dev/null
mv build/app-signed.bin build/tab5_cyberdeck.bin
espsecure.py verify_signature --version 2 --keyfile "$key" build/tab5_cyberdeck.bin >/dev/null
idf.py merge-bin -o "tab5-cyberdeck-v$ver-full.bin" >/dev/null

out="$(mktemp -d)"
cp "build/tab5-cyberdeck-v$ver-full.bin" "$out/"
cp build/tab5_cyberdeck.bin "$out/tab5-cyberdeck-v$ver-ota.bin"
# (grep without -q: with pipefail, -q exiting early makes the pipeline "fail")
strings "$out/tab5-cyberdeck-v$ver-ota.bin" | grep -x "$ver" >/dev/null || { echo "image does not carry version $ver"; exit 1; }

git push -q origin "$(git branch --show-current)"
git tag -a "v$ver" -m "v$ver"
git push -q origin "v$ver"
gh release create "v$ver" "$out"/*.bin --title "${3:-v$ver}" --notes-file "$notes" ${pre:+--prerelease}
echo "released v$ver"
