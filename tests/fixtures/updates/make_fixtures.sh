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
# The fake release is roost v9.9.9. SHA256SUMS lists the same asset bytes under
# the name of each package the app can ask for (macOS .dmg and Linux AppImage,
# both architectures); its signature is what the release workflow makes: the
# default (prehashed) one with the trusted comment `roost <tag> SHA256SUMS`.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
tag=v9.9.9

printf 'roost %s: a fake package for the tests of the update check.\n' "$tag" >"$scratch/asset.bin"
digest=$(shasum -a 256 "$scratch/asset.bin" | cut -d' ' -f1)
: >"$scratch/SHA256SUMS"
for name in roost-9.9.9-macos-arm64.dmg roost-9.9.9-macos-x86_64.dmg \
    roost-x86_64.AppImage roost-aarch64.AppImage; do
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
cp "$scratch/asset.bin" "$scratch/SHA256SUMS" "$scratch/SHA256SUMS.minisig" \
    "$scratch/SHA256SUMS.legacy.minisig" "$scratch/SHA256SUMS.other.minisig" "$here/"
