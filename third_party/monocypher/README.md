# Monocypher (vendored)

Version 4.0.3, https://monocypher.org/

Source: https://monocypher.org/download/monocypher-4.0.3.tar.gz

The tarball was checked against the checksums the project publishes next to it
(https://monocypher.org/download/monocypher-4.0.3.tar.gz.sha512 and
monocypher-4.0.3.tar.gz.blake2b):

- SHA-512  `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965`
- BLAKE2b  `5c586019e8e78ce6fb70f73f4ded10c852335138394b22fb704d0b4379edfc1adaea342f0e4c4b8c52fcf053df401d1fe47812ee5cb5062813de19fc51f8ae93`

Kept as shipped, unchanged: `src/monocypher.{c,h}` and
`src/optional/monocypher-ed25519.{c,h}` of the tarball (here `monocypher.{c,h}`
and `optional/`), and `LICENCE.md`. The optional file is the Ed25519 with
SHA-512 that minisign signatures are made with; the Ed25519 of the main file
uses BLAKE2b and is not the same signature scheme.

Update: take a new tarball, check it against the published checksums, replace
the files, and change the version here, in `THIRD_PARTY_LICENSES` and in
`docs/design/10-licenses.md`.
