#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <expected>

namespace roost {

// The check of a downloaded release (design/17-updates.md): the minisign
// signature of SHA256SUMS, then the SHA-256 of the package against its line.
// Pure computation, no network; a failure is one text for the user.

// A minisign public key: "Ed", the key id (8 bytes), the Ed25519 key (32).
struct UpdateKey {
    std::array<std::uint8_t, 8> Id{};
    std::array<std::uint8_t, 32> Key{};
};

// A minisign public key as the file `minisign.pub` has it (an untrusted
// comment line, then the key line) or as the key line alone.
std::expected<UpdateKey, QString> ParseUpdateKey(const QByteArray& text);

// Checks `signature`, the text of a .minisig file, over `sums`: the signature
// itself, then the signature of the trusted comment, then that the trusted
// comment is exactly `trustedComment`. Both minisign formats are accepted: `ED`
// (Ed25519 over the BLAKE2b-512 of the file, the default) and `Ed` (over the
// file, legacy). The key id of the signature must be that of `key`.
std::expected<void, QString> VerifySums(const UpdateKey& key,
                                        const QByteArray& sums,
                                        const QByteArray& signature,
                                        const QString& trustedComment);

// The line of `name` in `sums` (`sha256sum` output: "<64 hex>  <name>" or
// "<64 hex> *<name>"): its digest, lowercase hex. A line that is no such line,
// no line of the name, and two lines of it are failures.
std::expected<QByteArray, QString> SumsDigest(const QByteArray& sums,
                                              const QString& name);

// Whether the SHA-256 of the file at `path`, read in a stream, is `digest`
// (hex, either case).
std::expected<void, QString> VerifyFileDigest(const QString& path,
                                              const QByteArray& digest);

}  // namespace roost
