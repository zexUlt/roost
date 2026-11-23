#!/bin/sh
# Makes the signed release files that tests/core/update_verify_test.cpp and
# tests/roost/updates_test.cpp read, with the real minisign. Makes two key
# pairs in a scratch directory, signs, copies the public keys and the signed
# files here and deletes the scratch directory with the secret keys: a secret
# key is never kept. Run it again to renew everything; the tests read the
# public keys from here, so nothing else is to be edited.
#
#   tests/fixtures/updates/make_fixtures.sh
#
# The fake release is roost v9.9.9. SHA256SUMS lists package-macos.bin under the
# name of each macOS package and asset.bin under each Linux one (both
# architectures); its signature is what the release workflow makes: the
# default (prehashed) one with the trusted comment `roost <tag> SHA256SUMS`.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
tag=v9.9.9

printf 'roost %s: a fake package for the tests of the update check.\n' "$tag" >"$scratch/asset.bin"
digest=$(shasum -a 256 "$scratch/asset.bin" | cut -d' ' -f1)

# The macOS package of the install test: a .dmg with a roost.app that is as
# much of a bundle as the installer looks at (Info.plist with the version, the
# window and the daemon executables) and a file that says it is the new one.
# Needs hdiutil: run this script on a Mac.
app="$scratch/dmg-root/roost.app/Contents"
mkdir -p "$app/MacOS" "$app/Resources"
cat >"$app/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>roost</string>
<key>CFBundleShortVersionString</key><string>${tag#v}</string>
</dict></plist>
PLIST
for executable in roost roostd; do
    printf '#!/bin/sh\nexit 0\n' >"$app/MacOS/$executable"
    chmod +x "$app/MacOS/$executable"
done
printf 'new' >"$app/Resources/marker.txt"
hdiutil create -quiet -volname roost -srcfolder "$scratch/dmg-root" -fs HFS+ -format UDZO -ov "$scratch/package.dmg"
dmg_digest=$(shasum -a 256 "$scratch/package.dmg" | cut -d' ' -f1)

: >"$scratch/SHA256SUMS"
for name in roost-9.9.9-macos-arm64.dmg roost-9.9.9-macos-x86_64.dmg; do
    printf '%s  %s\n' "$dmg_digest" "$name" >>"$scratch/SHA256SUMS"
done
for name in roost-x86_64.AppImage roost-aarch64.AppImage; do
    printf '%s  %s\n' "$digest" "$name" >>"$scratch/SHA256SUMS"
done

minisign -G -W -p "$scratch/key.pub" -s "$scratch/key.sec" >/dev/null
minisign -G -W -p "$scratch/other.pub" -s "$scratch/other.sec" >/dev/null
comment="roost $tag SHA256SUMS"

minisign -S -s "$scratch/key.sec" -t "$comment" -x "$scratch/SHA256SUMS.minisig" -m "$scratch/SHA256SUMS" >/dev/null
minisign -S -l -s "$scratch/key.sec" -t "$comment" -x "$scratch/SHA256SUMS.legacy.minisig" -m "$scratch/SHA256SUMS" >/dev/null
minisign -S -s "$scratch/other.sec" -t "$comment" -x "$scratch/SHA256SUMS.other.minisig" -m "$scratch/SHA256SUMS" >/dev/null

# What the files claim, checked by minisign itself.
minisign -V -p "$scratch/key.pub" -x "$scratch/SHA256SUMS.minisig" -m "$scratch/SHA256SUMS" >/dev/null
minisign -V -p "$scratch/key.pub" -x "$scratch/SHA256SUMS.legacy.minisig" -m "$scratch/SHA256SUMS" >/dev/null
minisign -V -p "$scratch/other.pub" -x "$scratch/SHA256SUMS.other.minisig" -m "$scratch/SHA256SUMS" >/dev/null

cp "$scratch/key.pub" "$here/test.pub"
cp "$scratch/other.pub" "$here/other.pub"
# .bin: the repository ignores *.dmg.
cp "$scratch/package.dmg" "$here/package-macos.bin"
cp "$scratch/asset.bin" "$scratch/SHA256SUMS" "$scratch/SHA256SUMS.minisig" \
    "$scratch/SHA256SUMS.legacy.minisig" "$scratch/SHA256SUMS.other.minisig" "$here/"
