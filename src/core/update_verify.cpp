#include "core/update_verify.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QList>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>

#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

namespace roost {

namespace {

constexpr qsizetype SIGNATURE_SIZE = 64;
constexpr qsizetype KEY_ID_SIZE = 8;
constexpr qsizetype KEY_SIZE = 32;
constexpr qsizetype ALGORITHM_SIZE = 2;
constexpr qsizetype DIGEST_HEX_SIZE = 64;
constexpr QByteArrayView UNTRUSTED_PREFIX = "untrusted comment:";
constexpr QByteArrayView TRUSTED_PREFIX = "trusted comment: ";

const std::uint8_t* Bytes(const QByteArray& data) {
    return reinterpret_cast<const std::uint8_t*>(data.constData());
}

// The lines of a text, without their line ends; the empty line after the last
// line end is not a line.
QList<QByteArray> Lines(const QByteArray& text) {
    QList<QByteArray> lines = text.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    for (QByteArray& line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
    }
    return lines;
}

// The bytes of a base64 line, if it is base64 of exactly `size` bytes.
std::optional<QByteArray> Decode(const QByteArray& line, qsizetype size) {
    const auto result = QByteArray::fromBase64Encoding(
        line,
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.size() != size) {
        return std::nullopt;
    }
    return result.decoded;
}

QString Hex(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex().toUpper());
}

bool Matches(const std::uint8_t* signature, const UpdateKey& key,
             const QByteArray& message) {
    return crypto_ed25519_check(signature, key.Key.data(), Bytes(message),
                                static_cast<std::size_t>(message.size())) == 0;
}

}  // namespace

std::expected<UpdateKey, QString> ParseUpdateKey(const QByteArray& text) {
    QByteArray keyLine;
    for (const QByteArray& line : Lines(text)) {
        if (line.isEmpty() || line.startsWith(UNTRUSTED_PREFIX)) {
            continue;
        }
        if (!keyLine.isEmpty()) {
            return std::unexpected(
                QStringLiteral("the public key has more than one key line"));
        }
        keyLine = line;
    }
    const auto bytes = Decode(keyLine, ALGORITHM_SIZE + KEY_ID_SIZE + KEY_SIZE);
    if (!bytes || !bytes->startsWith("Ed")) {
        return std::unexpected(QStringLiteral("the public key is not valid"));
    }
    UpdateKey key;
    std::copy_n(Bytes(*bytes) + ALGORITHM_SIZE, KEY_ID_SIZE, key.Id.begin());
    std::copy_n(Bytes(*bytes) + ALGORITHM_SIZE + KEY_ID_SIZE, KEY_SIZE,
                key.Key.begin());
    return key;
}

std::expected<void, QString> VerifySums(const UpdateKey& key,
                                        const QByteArray& sums,
                                        const QByteArray& signature,
                                        const QString& trustedComment) {
    const auto malformed = [] {
        return std::unexpected(
            QStringLiteral("the signature file is not a minisign signature"));
    };
    const QList<QByteArray> lines = Lines(signature);
    if (lines.size() != 4 || !lines[0].startsWith(UNTRUSTED_PREFIX) ||
        !lines[2].startsWith(TRUSTED_PREFIX)) {
        return malformed();
    }
    const auto head =
        Decode(lines[1], ALGORITHM_SIZE + KEY_ID_SIZE + SIGNATURE_SIZE);
    const auto global = Decode(lines[3], SIGNATURE_SIZE);
    if (!head || !global) {
        return malformed();
    }
    const QByteArray algorithm = head->left(ALGORITHM_SIZE);
    const QByteArray keyId = head->mid(ALGORITHM_SIZE, KEY_ID_SIZE);
    const QByteArray signatureBytes = head->mid(ALGORITHM_SIZE + KEY_ID_SIZE);
    const bool prehashed = algorithm == "ED";
    if (!prehashed && algorithm != "Ed") {
        return std::unexpected(
            QStringLiteral("the signature has an algorithm that is not "
                           "supported"));
    }
    if (std::memcmp(keyId.constData(), key.Id.data(), KEY_ID_SIZE) != 0) {
        return std::unexpected(
            QStringLiteral("the signature is by another key (%1)")
                .arg(Hex(keyId)));
    }

    QByteArray message = sums;
    if (prehashed) {
        message = QByteArray(64, '\0');
        crypto_blake2b(reinterpret_cast<std::uint8_t*>(message.data()),
                       static_cast<std::size_t>(message.size()), Bytes(sums),
                       static_cast<std::size_t>(sums.size()));
    }
    if (!Matches(Bytes(signatureBytes), key, message)) {
        return std::unexpected(
            QStringLiteral("the signature of SHA256SUMS is not valid"));
    }
    const QByteArray comment = lines[2].mid(TRUSTED_PREFIX.size());
    if (!Matches(Bytes(*global), key, signatureBytes + comment)) {
        return std::unexpected(QStringLiteral(
            "the signature of the comment of SHA256SUMS is not valid"));
    }
    if (comment != trustedComment.toUtf8()) {
        return std::unexpected(
            QStringLiteral("the signed comment is \"%1\", not \"%2\"")
                .arg(QString::fromUtf8(comment), trustedComment));
    }
    return {};
}

std::expected<QByteArray, QString> SumsDigest(const QByteArray& sums,
                                              const QString& name) {
    const QByteArray wanted = name.toUtf8();
    std::optional<QByteArray> digest;
    qsizetype number = 0;
    for (const QByteArray& line : Lines(sums)) {
        ++number;
        if (line.isEmpty()) {
            continue;
        }
        // "<64 hex>", a space, and a space or '*' (the binary mode), the name.
        const QByteArray hex = line.left(DIGEST_HEX_SIZE);
        const bool formed =
            line.size() > DIGEST_HEX_SIZE + 2 &&
            QByteArray::fromHex(hex).size() == DIGEST_HEX_SIZE / 2 &&
            std::ranges::all_of(hex,
                                [](char c) {
                                    return (c >= '0' && c <= '9') ||
                                           (c >= 'a' && c <= 'f') ||
                                           (c >= 'A' && c <= 'F');
                                }) &&
            line[DIGEST_HEX_SIZE] == ' ' &&
            (line[DIGEST_HEX_SIZE + 1] == ' ' ||
             line[DIGEST_HEX_SIZE + 1] == '*');
        if (!formed) {
            return std::unexpected(
                QStringLiteral("SHA256SUMS has a line that is not a digest "
                               "(line %1)")
                    .arg(number));
        }
        if (line.mid(DIGEST_HEX_SIZE + 2) != wanted) {
            continue;
        }
        if (digest) {
            return std::unexpected(
                QStringLiteral("SHA256SUMS has more than one line of %1")
                    .arg(name));
        }
        digest = hex.toLower();
    }
    if (!digest) {
        return std::unexpected(
            QStringLiteral("SHA256SUMS has no line of %1").arg(name));
    }
    return *digest;
}

std::expected<void, QString> VerifyFileDigest(const QString& path,
                                              const QByteArray& digest) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("cannot read the package: %1")
                                   .arg(file.errorString()));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError) {
        return std::unexpected(QStringLiteral("cannot read the package: %1")
                                   .arg(file.errorString()));
    }
    if (hash.result().toHex() != digest.toLower()) {
        return std::unexpected(
            QStringLiteral("the package does not match its line in "
                           "SHA256SUMS"));
    }
    return {};
}

}  // namespace roost
