// The check of a downloaded release (design/17-updates.md): the minisign
// signature of SHA256SUMS and the SHA-256 of the package against its line. The
// files are made by the real minisign
// (tests/fixtures/updates/make_fixtures.sh); a case changes one thing in them
// and says what the check does with it.

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QList>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#include "core/update_verify.hpp"

using namespace roost;

namespace {

const QString FIXTURES = QStringLiteral(FIXTURES_DIR "/updates/");
const QString TAG_COMMENT = QStringLiteral("roost v9.9.9 SHA256SUMS");
const QString PACKAGE = QStringLiteral("roost-x86_64.AppImage");

QByteArray Read(const QString& name) {
    QFile file(FIXTURES + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The lines of a .minisig: comment, signature, trusted comment, global
// signature.
QList<QByteArray> SignatureLines(const QString& name) {
    return Read(name).split('\n');
}

QByteArray Join(const QList<QByteArray>& lines) { return lines.join('\n'); }

// A line of base64 with one byte of what it decodes to changed: bits of `mask`
// flipped.
QByteArray ChangeByte(const QByteArray& line, qsizetype at, int mask = 0x01) {
    QByteArray bytes = QByteArray::fromBase64(line);
    bytes[at] = static_cast<char>(bytes[at] ^ mask);
    return bytes.toBase64();
}

QByteArray Signature(const QString& name, int line, qsizetype byte) {
    QList<QByteArray> lines = SignatureLines(name);
    lines[line] = ChangeByte(lines[line], byte);
    return Join(lines);
}

}  // namespace

class UpdateVerifyTest : public QObject {
    Q_OBJECT

private:
    UpdateKey Key(const QString& name = "test.pub") {
        const auto key = ParseUpdateKey(Read(name));
        return key.value_or(UpdateKey{});
    }

    // The failure's text, empty for a success.
    QString Verify(const QByteArray& sums, const QByteArray& signature,
                   const QString& comment = TAG_COMMENT,
                   const QString& keyFile = "test.pub") {
        const auto result = VerifySums(Key(keyFile), sums, signature, comment);
        return result ? QString() : result.error();
    }

private slots:
    void TheKeyFilesParse() {
        QVERIFY(ParseUpdateKey(Read("test.pub")).has_value());
        // The key line alone is a key too, and a line end of Windows is no
        // part of it.
        const QByteArray line = Read("test.pub").split('\n').at(1);
        QCOMPARE(ParseUpdateKey(line)->Key, Key().Key);
        QCOMPARE(ParseUpdateKey(line + "\r\n")->Key, Key().Key);
        QVERIFY(Key().Key != Key("other.pub").Key);
        QVERIFY(Key().Id != Key("other.pub").Id);
    }

    void AKeyThatIsNoKeyIsRefused_data() {
        QTest::addColumn<QByteArray>("text");
        const QByteArray good = Read("test.pub").split('\n').at(1);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("only the comment")
            << QByteArray("untrusted comment: minisign public key X\n");
        QTest::newRow("not base64") << QByteArray("RWQ1q7RPPqQ8SF2K!!!!");
        QTest::newRow("cut") << good.left(good.size() - 8);
        QTest::newRow("longer") << good + "AAAA";
        QTest::newRow("another algorithm") << QByteArray(ChangeByte(good, 0));
        QTest::newRow("two keys") << good + "\n" + good + "\n";
    }

    void AKeyThatIsNoKeyIsRefused() {
        QFETCH(QByteArray, text);
        QVERIFY(!ParseUpdateKey(text).has_value());
    }

    // `minisign -S`: Ed25519 over the BLAKE2b-512 of the file ("ED").
    void AGoodSignatureIsAccepted() {
        QVERIFY(SignatureLines("SHA256SUMS.minisig").at(1).startsWith("RU"));
        QCOMPARE(Verify(Read("SHA256SUMS"), Read("SHA256SUMS.minisig")),
                 QString());
    }

    // `minisign -S -l`: Ed25519 over the file ("Ed").
    void AGoodLegacySignatureIsAccepted() {
        QVERIFY(
            SignatureLines("SHA256SUMS.legacy.minisig").at(1).startsWith("RW"));
        QCOMPARE(Verify(Read("SHA256SUMS"), Read("SHA256SUMS.legacy.minisig")),
                 QString());
    }

    void ASignatureThatIsNotRightIsRefused_data() {
        QTest::addColumn<QByteArray>("sums");
        QTest::addColumn<QByteArray>("signature");
        QTest::addColumn<QString>("reason");
        const QByteArray sums = Read("SHA256SUMS");
        const QString bad = "the signature of SHA256SUMS is not valid";
        const QString badComment =
            "the signature of the comment of SHA256SUMS is not valid";

        QByteArray changedSums = sums;
        changedSums[10] = changedSums[10] == 'a' ? 'b' : 'a';
        QTest::newRow("a byte of SHA256SUMS")
            << changedSums << Read("SHA256SUMS.minisig") << bad;
        QTest::newRow("a byte of SHA256SUMS, legacy")
            << changedSums << Read("SHA256SUMS.legacy.minisig") << bad;
        QTest::newRow("a line more in SHA256SUMS")
            << sums +
                   "0000000000000000000000000000000000000000000000000000000"
                   "000000000  other\n"
            << Read("SHA256SUMS.minisig") << bad;
        // 2 bytes of the algorithm and 8 of the key id come first.
        QTest::newRow("a byte of the signature")
            << sums << Signature("SHA256SUMS.minisig", 1, 20) << bad;
        QTest::newRow("a byte of the signature, legacy")
            << sums << Signature("SHA256SUMS.legacy.minisig", 1, 73) << bad;
        QTest::newRow("a byte of the global signature")
            << sums << Signature("SHA256SUMS.minisig", 3, 5) << badComment;
        // The algorithm of the one is the other's: the signature is not of
        // what the check then hashes.
        QList<QByteArray> swapped = SignatureLines("SHA256SUMS.minisig");
        swapped[1] = ChangeByte(swapped[1], 1, 0x20);  // "ED" to "Ed"
        QTest::newRow("the algorithm changed to the legacy one")
            << sums << Join(swapped) << bad;

        QList<QByteArray> comment = SignatureLines("SHA256SUMS.minisig");
        comment[2] = "trusted comment: roost v9.9.8 SHA256SUMS";
        QTest::newRow("the trusted comment changed")
            << sums << Join(comment) << badComment;
        QList<QByteArray> cut = SignatureLines("SHA256SUMS.minisig");
        cut[2] = "trusted comment: ";
        QTest::newRow("the trusted comment emptied")
            << sums << Join(cut) << badComment;

        QTest::newRow("another key") << sums << Read("SHA256SUMS.other.minisig")
                                     << "the signature is by another key";
        // The id of the key is checked first, and it is not the signature's.
        QTest::newRow("another key id in the signature")
            << sums << Signature("SHA256SUMS.minisig", 1, 4)
            << "the signature is by another key";
    }

    void ASignatureThatIsNotRightIsRefused() {
        QFETCH(QByteArray, sums);
        QFETCH(QByteArray, signature);
        QFETCH(QString, reason);
        const QString result = Verify(sums, signature);
        QVERIFY2(result.startsWith(reason),
                 qPrintable("got: '" + result + "'"));
    }

    // The signature is made by the key whose id it carries, and by no other:
    // a public key of another id refuses the good signature at the id.
    void AKeyOfAnotherIdIsRefused() {
        UpdateKey key = Key();
        key.Id[0] = static_cast<std::uint8_t>(key.Id[0] ^ 0x01);
        const auto result = VerifySums(key, Read("SHA256SUMS"),
                                       Read("SHA256SUMS.minisig"), TAG_COMMENT);
        QVERIFY(!result);
        QVERIFY(result.error().startsWith("the signature is by another key"));
        // And the right id with the key of the other pair: not a match.
        UpdateKey mixed = Key("other.pub");
        mixed.Id = Key().Id;
        const auto other = VerifySums(mixed, Read("SHA256SUMS"),
                                      Read("SHA256SUMS.minisig"), TAG_COMMENT);
        QVERIFY(!other);
        QVERIFY(other.error().startsWith("the signature of SHA256SUMS"));
    }

    // The trusted comment names the release: the signed files of another
    // release do not pass for this one.
    void ASignedCommentOfAnotherReleaseIsRefused() {
        const QString result =
            Verify(Read("SHA256SUMS"), Read("SHA256SUMS.minisig"),
                   "roost v9.9.8 SHA256SUMS");
        QVERIFY2(result.startsWith("the signed comment is "),
                 qPrintable(result));
        QVERIFY(result.contains("roost v9.9.9 SHA256SUMS"));
        QVERIFY(!Verify(Read("SHA256SUMS"), Read("SHA256SUMS.minisig"),
                        "roost v9.9.9 SHA256SUMS ")
                     .isEmpty());
        QVERIFY(!Verify(Read("SHA256SUMS"), Read("SHA256SUMS.minisig"), "")
                     .isEmpty());
    }

    void AFileThatIsNoSignatureIsRefused_data() {
        QTest::addColumn<QByteArray>("signature");
        const QByteArray good = Read("SHA256SUMS.minisig");
        const QList<QByteArray> lines = SignatureLines("SHA256SUMS.minisig");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("text") << QByteArray("hello\n");
        QTest::newRow("three lines")
            << Join({lines[0], lines[1], lines[2], QByteArray()});
        QTest::newRow("a line more") << good + "extra\n";
        QTest::newRow("no untrusted comment")
            << Join({"comment", lines[1], lines[2], lines[3], QByteArray()});
        QTest::newRow("no trusted comment")
            << Join({lines[0], lines[1], "comment", lines[3], QByteArray()});
        QTest::newRow("signature is no base64") << Join(
            {lines[0], "RUR9vyNZ!!!!", lines[2], lines[3], QByteArray()});
        QTest::newRow("signature is cut")
            << Join({lines[0], lines[1].left(lines[1].size() - 8), lines[2],
                     lines[3], QByteArray()});
        QTest::newRow("global signature is no base64")
            << Join({lines[0], lines[1], lines[2], "!!!!", QByteArray()});
        QTest::newRow("global signature is cut")
            << Join({lines[0], lines[1], lines[2],
                     lines[3].left(lines[3].size() - 8), QByteArray()});
        QList<QByteArray> unknown = lines;
        unknown[1] = ChangeByte(unknown[1], 0);
        QTest::newRow("an algorithm that is none") << Join(unknown);
    }

    void AFileThatIsNoSignatureIsRefused() {
        QFETCH(QByteArray, signature);
        QVERIFY(!Verify(Read("SHA256SUMS"), signature).isEmpty());
    }

    void TheLineOfAPackageIsFound() {
        const auto digest = SumsDigest(Read("SHA256SUMS"), PACKAGE);
        QVERIFY(digest.has_value());
        QCOMPARE(digest->size(), 64);
        QCOMPARE(*digest, QByteArray("ec10b1e2feadfd513e0bb9bb702d247adc5eb188"
                                     "a0b1f141467f64073252804f"));
    }

    void TheLineOfAPackageHasItsForms_data() {
        QTest::addColumn<QByteArray>("sums");
        const QByteArray hex(64, 'a');
        QTest::newRow("a binary line") << hex + " *" + PACKAGE.toUtf8() + "\n";
        QTest::newRow("no line end at the end")
            << hex + "  " + PACKAGE.toUtf8();
        QTest::newRow("a line end of Windows")
            << hex + "  " + PACKAGE.toUtf8() + "\r\n";
        QTest::newRow("another line before")
            << hex + "  other\n" + hex + "  " + PACKAGE.toUtf8() + "\n";
        QTest::newRow("blank lines")
            << "\n" + hex + "  " + PACKAGE.toUtf8() + "\n\n";
    }

    void TheLineOfAPackageHasItsForms() {
        QFETCH(QByteArray, sums);
        const auto digest = SumsDigest(sums, PACKAGE);
        QVERIFY(digest.has_value());
        QCOMPARE(*digest, QByteArray(64, 'a'));
    }

    // Capitals are the same digest.
    void ADigestOfCapitalsIsLowercased() {
        const auto digest = SumsDigest(QByteArray(64, 'A') + "  x\n", "x");
        QCOMPARE(digest.value_or(QByteArray()), QByteArray(64, 'a'));
    }

    void ALineThatIsNotRightFails_data() {
        QTest::addColumn<QByteArray>("sums");
        QTest::addColumn<QString>("reason");
        const QByteArray hex(64, 'b');
        const QByteArray name = PACKAGE.toUtf8();
        QTest::newRow("empty") << QByteArray() << "SHA256SUMS has no line of ";
        QTest::newRow("another name") << hex + "  other\n"
                                      << "SHA256SUMS has no line of ";
        // The name is the whole name: no prefix, no suffix, no other case.
        QTest::newRow("a longer name") << hex + "  " + name + ".sig\n"
                                       << "SHA256SUMS has no line of ";
        QTest::newRow("a name with a path") << hex + "  dir/" + name + "\n"
                                            << "SHA256SUMS has no line of ";
        QTest::newRow("another case") << hex + "  " + name.toUpper() + "\n"
                                      << "SHA256SUMS has no line of ";
        QTest::newRow("the same line twice")
            << hex + "  " + name + "\n" + hex + "  " + name + "\n"
            << "SHA256SUMS has more than one line of ";
        QTest::newRow("two lines, one binary")
            << hex + "  " + name + "\n" + QByteArray(64, 'c') + " *" + name +
                   "\n"
            << "SHA256SUMS has more than one line of ";
        QTest::newRow("a digest that is short")
            << QByteArray(63, 'b') + "  " + name + "\n"
            << "SHA256SUMS has a line that is not a digest";
        QTest::newRow("a digest that is long")
            << QByteArray(65, 'b') + "  " + name + "\n"
            << "SHA256SUMS has a line that is not a digest";
        QTest::newRow("a digest that is no hex")
            << QByteArray(63, 'b') + "g  " + name + "\n"
            << "SHA256SUMS has a line that is not a digest";
        QTest::newRow("one space")
            << hex + " " + name + "\n"
            << "SHA256SUMS has a line that is not a digest";
        QTest::newRow("a tab") << hex + "\t\t" + name + "\n"
                               << "SHA256SUMS has a line that is not a digest";
        QTest::newRow("no name")
            << hex + "  \n"
            << "SHA256SUMS has a line that is not a digest";
        // Not the line asked for, but a line of the file that is no line: the
        // file is not what the release workflow makes.
        QTest::newRow("a line that is no digest before the right one")
            << "garbage\n" + hex + "  " + name + "\n"
            << "SHA256SUMS has a line that is not a digest";
    }

    void ALineThatIsNotRightFails() {
        QFETCH(QByteArray, sums);
        QFETCH(QString, reason);
        const auto digest = SumsDigest(sums, PACKAGE);
        QVERIFY(!digest.has_value());
        QVERIFY2(digest.error().startsWith(reason), qPrintable(digest.error()));
    }

    // The digest of the file, in a stream: the bytes of the fixture match its
    // line, one changed byte (at the start, in the middle, at the end) does
    // not, and neither does a file that is shorter or longer.
    void TheDigestOfAFileIsCheckedInAStream() {
        const QByteArray asset = Read("asset.bin");
        const auto digest = SumsDigest(Read("SHA256SUMS"), PACKAGE).value();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto check = [&](const QByteArray& content) {
            QSaveFile file(dir.filePath("package"));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(content) != content.size() || !file.commit()) {
                return QStringLiteral("cannot write the file");
            }
            const auto result =
                VerifyFileDigest(dir.filePath("package"), digest);
            return result ? QString() : result.error();
        };
        QCOMPARE(check(asset), QString());
        for (const qsizetype at :
             {qsizetype{0}, asset.size() / 2, asset.size() - 1}) {
            QByteArray changed = asset;
            changed[at] = static_cast<char>(changed[at] ^ 0x01);
            QVERIFY2(check(changed).startsWith("the package does not match"),
                     qPrintable(QString::number(at)));
        }
        QVERIFY(check(asset.left(asset.size() - 1)).startsWith("the package"));
        QVERIFY(check(asset + "x").startsWith("the package"));
        QVERIFY(check(QByteArray()).startsWith("the package"));
        // A big one reads in pieces and gives the digest of all of it.
        QByteArray big(5 * 1024 * 1024 + 3, '\0');
        for (qsizetype i = 0; i < big.size(); ++i) {
            big[i] = static_cast<char>(i * 31);
        }
        QSaveFile file(dir.filePath("big"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(big);
        QVERIFY(file.commit());
        const QByteArray bigDigest =
            QCryptographicHash::hash(big, QCryptographicHash::Sha256).toHex();
        QVERIFY(VerifyFileDigest(dir.filePath("big"), bigDigest).has_value());
        QVERIFY(VerifyFileDigest(dir.filePath("big"), QByteArray(64, '0')) ==
                std::unexpected(QStringLiteral(
                    "the package does not match its line in SHA256SUMS")));
    }

    void AFileThatCannotBeReadFails() {
        const auto result =
            VerifyFileDigest(FIXTURES + "no-such-file", QByteArray(64, 'a'));
        QVERIFY(!result);
        QVERIFY(result.error().startsWith("cannot read the package"));
    }
};

QTEST_MAIN(UpdateVerifyTest)
#include "update_verify_test.moc"
