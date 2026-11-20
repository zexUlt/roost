#include "roost/bug_report.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <unistd.h>

#include "core/protocol.hpp"
#include "core/store.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

constexpr quint64 ZIP_MAX = std::numeric_limits<quint32>::max();
constexpr quint16 FLAG_UTF8 = 1U << 11;
constexpr quint16 METHOD_STORED = 0;
constexpr quint16 METHOD_DEFLATE = 8;
constexpr quint16 VERSION_NEEDED = 20;
// Made by Unix (3), so that the external attributes are a Unix mode.
constexpr quint16 VERSION_MADE_BY = (3U << 8) | VERSION_NEEDED;
constexpr quint32 UNIX_FILE_MODE = 0100644U << 16;
constexpr qsizetype ZLIB_PREFIX_LENGTH = 4;  // qCompress: the size, big-endian
constexpr qsizetype ZLIB_HEADER_LENGTH = 2;
constexpr qsizetype ZLIB_TRAILER_LENGTH = 4;  // adler32
constexpr int MAX_NUMBERED_NAMES = 1000;

const std::array<QLatin1StringView, 5> LOG_NAMES = {
    QLatin1StringView("roostd.log"), QLatin1StringView("roostd.log.1"),
    QLatin1StringView("roostd.start.log"), QLatin1StringView("roost.log"),
    QLatin1StringView("roost.log.1")};

const QString KEYMAP_FILE = QStringLiteral("keymap.json");

std::array<quint32, 256> MakeCrcTable() {
    std::array<quint32, 256> table{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

quint32 Crc32(const QByteArray& data) {
    static const std::array<quint32, 256> TABLE = MakeCrcTable();
    quint32 c = 0xFFFFFFFFU;
    for (const char byte : data) {
        c = TABLE[(c ^ static_cast<quint8>(byte)) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

quint32 Adler32(const QByteArray& data) {
    constexpr quint32 mod = 65521;
    quint32 a = 1;
    quint32 b = 0;
    for (const char byte : data) {
        a = (a + static_cast<quint8>(byte)) % mod;
        b = (b + a) % mod;
    }
    return (b << 16) | a;
}

// The raw deflate stream of `data`, cut out of what qCompress makes: a 4-byte
// length, a zlib stream (2-byte header, deflate, 4-byte adler32). Empty if the
// result is not trustworthy or not smaller than the data: the caller stores.
QByteArray RawDeflate(const QByteArray& data) {
    if (data.isEmpty()) {
        return {};
    }
    const QByteArray z = qCompress(data, 9);
    if (z.size() <=
        ZLIB_PREFIX_LENGTH + ZLIB_HEADER_LENGTH + ZLIB_TRAILER_LENGTH) {
        return {};
    }
    const auto cmf = static_cast<quint8>(z[ZLIB_PREFIX_LENGTH]);
    const auto flg = static_cast<quint8>(z[ZLIB_PREFIX_LENGTH + 1]);
    // Method 8 (deflate), a valid header checksum and no preset dictionary.
    if ((cmf & 0x0FU) != 8 || ((cmf << 8) | flg) % 31 != 0 ||
        (flg & 0x20U) != 0) {
        return {};
    }
    const qsizetype end = z.size() - ZLIB_TRAILER_LENGTH;
    const auto* trailer = reinterpret_cast<const quint8*>(z.constData() + end);
    const quint32 stored = (static_cast<quint32>(trailer[0]) << 24) |
                           (static_cast<quint32>(trailer[1]) << 16) |
                           (static_cast<quint32>(trailer[2]) << 8) | trailer[3];
    if (stored != Adler32(data)) {
        return {};
    }
    QByteArray raw = z.mid(ZLIB_PREFIX_LENGTH + ZLIB_HEADER_LENGTH,
                           end - ZLIB_PREFIX_LENGTH - ZLIB_HEADER_LENGTH);
    return raw.size() < data.size() ? raw : QByteArray();
}

void Put16(QByteArray& out, quint16 value) {
    out.append(static_cast<char>(value & 0xFFU));
    out.append(static_cast<char>(value >> 8));
}

void Put32(QByteArray& out, quint32 value) {
    Put16(out, static_cast<quint16>(value & 0xFFFFU));
    Put16(out, static_cast<quint16>(value >> 16));
}

quint16 DosTime(const QDateTime& time) {
    const QTime t = time.time();
    return static_cast<quint16>((t.hour() << 11) | (t.minute() << 5) |
                                (t.second() / 2));
}

quint16 DosDate(const QDateTime& time) {
    const QDate d = time.date();
    if (d.year() < 1980) {
        return (1 << 5) | 1;
    }
    return static_cast<quint16>(((d.year() - 1980) << 9) | (d.month() << 5) |
                                d.day());
}

// A zip with no zip64, no encryption and no data descriptors, written to
// `out` entry by entry.
class ZipWriter {
public:
    ZipWriter(QIODevice& out, const QDateTime& time)
        : Out_(out), Time_(DosTime(time)), Date_(DosDate(time)) {}

    std::expected<void, QString> Add(const QString& name,
                                     const QByteArray& data) {
        const QByteArray nameBytes = name.toUtf8();
        if (static_cast<quint64>(data.size()) > ZIP_MAX) {
            return std::unexpected(
                QStringLiteral("%1 is over 4 GiB; zip64 is not supported")
                    .arg(name));
        }
        QByteArray body = RawDeflate(data);
        const quint16 method = body.isEmpty() ? METHOD_STORED : METHOD_DEFLATE;
        if (body.isEmpty()) {
            body = data;
        }
        const quint32 crc = Crc32(data);
        Entry entry{nameBytes,
                    crc,
                    static_cast<quint32>(body.size()),
                    static_cast<quint32>(data.size()),
                    method,
                    static_cast<quint32>(Written_)};
        if (Written_ + 30 + static_cast<quint64>(nameBytes.size()) +
                static_cast<quint64>(body.size()) >
            ZIP_MAX) {
            return std::unexpected(
                QStringLiteral("the report is over 4 GiB; zip64 is not "
                               "supported"));
        }
        QByteArray header;
        Put32(header, 0x04034B50U);
        Put16(header, VERSION_NEEDED);
        Put16(header, FLAG_UTF8);
        Put16(header, method);
        Put16(header, Time_);
        Put16(header, Date_);
        Put32(header, crc);
        Put32(header, entry.CompressedSize);
        Put32(header, entry.Size);
        Put16(header, static_cast<quint16>(nameBytes.size()));
        Put16(header, 0);
        header.append(nameBytes);
        if (const auto written = Write(header); !written) {
            return written;
        }
        if (const auto written = Write(body); !written) {
            return written;
        }
        Entries_.push_back(entry);
        return {};
    }

    std::expected<void, QString> Finish() {
        const quint64 directoryOffset = Written_;
        QByteArray directory;
        for (const Entry& e : Entries_) {
            Put32(directory, 0x02014B50U);
            Put16(directory, VERSION_MADE_BY);
            Put16(directory, VERSION_NEEDED);
            Put16(directory, FLAG_UTF8);
            Put16(directory, e.Method);
            Put16(directory, Time_);
            Put16(directory, Date_);
            Put32(directory, e.Crc);
            Put32(directory, e.CompressedSize);
            Put32(directory, e.Size);
            Put16(directory, static_cast<quint16>(e.Name.size()));
            Put16(directory, 0);  // extra
            Put16(directory, 0);  // comment
            Put16(directory, 0);  // disk
            Put16(directory, 0);  // internal attributes
            Put32(directory, UNIX_FILE_MODE);
            Put32(directory, e.Offset);
            directory.append(e.Name);
        }
        if (Written_ + static_cast<quint64>(directory.size()) + 22 > ZIP_MAX) {
            return std::unexpected(
                QStringLiteral("the report is over 4 GiB; zip64 is not "
                               "supported"));
        }
        QByteArray end;
        Put32(end, 0x06054B50U);
        Put16(end, 0);
        Put16(end, 0);
        Put16(end, static_cast<quint16>(Entries_.size()));
        Put16(end, static_cast<quint16>(Entries_.size()));
        Put32(end, static_cast<quint32>(directory.size()));
        Put32(end, static_cast<quint32>(directoryOffset));
        Put16(end, 0);
        if (const auto written = Write(directory); !written) {
            return written;
        }
        return Write(end);
    }

private:
    struct Entry {
        QByteArray Name;
        quint32 Crc;
        quint32 CompressedSize;
        quint32 Size;
        quint16 Method;
        quint32 Offset;
    };

    std::expected<void, QString> Write(const QByteArray& bytes) {
        if (Out_.write(bytes) != bytes.size()) {
            return std::unexpected(
                QStringLiteral("write failed: %1").arg(Out_.errorString()));
        }
        Written_ += static_cast<quint64>(bytes.size());
        return {};
    }

    QIODevice& Out_;
    quint16 Time_;
    quint16 Date_;
    quint64 Written_ = 0;
    std::vector<Entry> Entries_;
};

QByteArray CutMarker(qint64 cut) {
    return QByteArray("[roost: first ") + QByteArray::number(cut) +
           " bytes cut]\n";
}

// A log is read from `size - maxBytes` on, with the marker in front; what
// grew after `size` was taken is left out.
std::expected<QByteArray, QString> ReadFile(const BugReportFile& file,
                                            qint64 maxBytes) {
    QFile in(file.Path);
    if (!in.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("cannot read %1: %2")
                                   .arg(file.Path, in.errorString()));
    }
    const qint64 size = in.size();
    const qint64 cut = file.IsLog ? std::max<qint64>(0, size - maxBytes) : 0;
    if (static_cast<quint64>(size - cut) > ZIP_MAX ||
        size - cut > std::numeric_limits<int>::max()) {
        return std::unexpected(
            QStringLiteral("%1 is over 4 GiB; zip64 is not supported")
                .arg(file.Path));
    }
    if (cut > 0 && !in.seek(cut)) {
        return std::unexpected(QStringLiteral("cannot read %1: %2")
                                   .arg(file.Path, in.errorString()));
    }
    QByteArray data = file.IsLog ? in.read(size - cut) : in.readAll();
    if (in.error() != QFileDevice::NoError) {
        return std::unexpected(QStringLiteral("cannot read %1: %2")
                                   .arg(file.Path, in.errorString()));
    }
    if (cut > 0) {
        data.prepend(CutMarker(cut));
    }
    return data;
}

}  // namespace

QList<BugReportFile> LogFiles(const QString& logDirectory, qint64 maxBytes) {
    QList<BugReportFile> files;
    for (const QLatin1StringView name : LOG_NAMES) {
        const QFileInfo info(logDirectory + u'/' + name);
        if (info.isFile()) {
            const qint64 cut = std::max<qint64>(0, info.size() - maxBytes);
            const qint64 size =
                cut > 0 ? maxBytes + CutMarker(cut).size() : info.size();
            files.push_back({QStringLiteral("logs/") + name,
                             info.absoluteFilePath(), size, true});
        }
    }
    return files;
}

QList<BugReportFile> SettingsFiles(const QString& configDirectory) {
    QList<BugReportFile> files;
    if (configDirectory.isEmpty()) {
        return files;
    }
    const QDir dir(configDirectory);
    const QFileInfoList entries = dir.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& info : entries) {
        const QString id = info.completeBaseName();
        if (info.fileName() == KEYMAP_FILE || Store::IsValidPluginId(id)) {
            files.push_back({QStringLiteral("settings/") + info.fileName(),
                             info.absoluteFilePath(), info.size()});
        }
    }
    return files;
}

QString AboutText(const QString& socketPath, const QString& graphicsApi) {
    QString text;
    const auto line = [&text](const QString& key, const QString& value) {
        text += key + QStringLiteral(": ") + value + u'\n';
    };
    line(QStringLiteral("roost"), QString::fromStdString(VersionString()));
    line(QStringLiteral("Qt (runtime)"), QString::fromLatin1(qVersion()));
    line(QStringLiteral("OS"), QSysInfo::prettyProductName());
    line(QStringLiteral("Kernel"),
         QSysInfo::kernelType() + u' ' + QSysInfo::kernelVersion());
    line(QStringLiteral("CPU architecture"),
         QSysInfo::currentCpuArchitecture());
    line(QStringLiteral("QPA platform"), QGuiApplication::platformName());
    if (!graphicsApi.isEmpty()) {
        line(QStringLiteral("Graphics API"), graphicsApi);
    }
    line(QStringLiteral("Socket"), socketPath);
    return text;
}

QString DefaultOutputDirectory() {
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}

QString ReportName(const QDateTime& time) {
    return QStringLiteral("roost-bug-") +
           time.toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

QString IssueUrl(const QString& description, const QString& zipName) {
    const QString base = QStringLiteral(
        "https://github.com/zexUlt/roost/issues/new?title=&body=");
    const QString footer =
        zipName.isEmpty()
            ? QStringLiteral("\n\nPlease attach the bug report zip.")
            : QStringLiteral("\n\nPlease attach the bug report zip: ") +
                  zipName;
    const auto build = [&](qsizetype length) {
        QString body = description.left(length);
        if (length < description.size()) {
            if (!body.isEmpty() && body.back().isHighSurrogate()) {
                body.chop(1);
            }
            body += QChar(0x2026);
        }
        return base +
               QString::fromLatin1(QUrl::toPercentEncoding(body + footer));
    };
    QString url = build(description.size());
    if (url.size() <= MAX_ISSUE_URL_LENGTH) {
        return url;
    }
    qsizetype low = 0;
    qsizetype high = description.size();
    while (low < high) {
        const qsizetype mid = (low + high + 1) / 2;
        if (build(mid).size() <= MAX_ISSUE_URL_LENGTH) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return build(low);
}

std::expected<QString, QString> CreateBugReport(
    const BugReportRequest& request) {
    const QString name = ReportName(request.Time);
    QList<BugReportFile> files =
        LogFiles(request.LogDirectory, request.MaxLogBytes);
    if (request.IncludeSettings) {
        files += SettingsFiles(request.ConfigDirectory);
    }

    // Everything is read before anything is written.
    QList<std::pair<QString, QByteArray>> entries;
    entries.push_back({QStringLiteral("about.txt"), request.About.toUtf8()});
    entries.push_back(
        {QStringLiteral("description.txt"), request.Description.toUtf8()});
    for (const BugReportFile& file : files) {
        auto data = ReadFile(file, request.MaxLogBytes);
        if (!data) {
            return std::unexpected(data.error());
        }
        entries.push_back({file.Name, std::move(*data)});
    }

    const QString directory = request.OutputDirectory.isEmpty()
                                  ? DefaultOutputDirectory()
                                  : request.OutputDirectory;
    if (!QDir().mkpath(directory)) {
        return std::unexpected(
            QStringLiteral("cannot create %1").arg(directory));
    }
    const QString temporary = QStringLiteral("%1/.%2.%3.part")
                                  .arg(directory, name)
                                  .arg(static_cast<qint64>(::getpid()));
    QFile out(temporary);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return std::unexpected(QStringLiteral("cannot write %1: %2")
                                   .arg(temporary, out.errorString()));
    }
    // The logs are private.
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto fail = [&out](const QString& reason) {
        out.close();
        out.remove();
        return std::unexpected(reason);
    };

    ZipWriter zip(out, request.Time);
    for (const auto& [entryName, data] : entries) {
        if (const auto added = zip.Add(name + u'/' + entryName, data); !added) {
            return fail(added.error());
        }
    }
    if (const auto finished = zip.Finish(); !finished) {
        return fail(finished.error());
    }
    if (!out.flush()) {
        return fail(QStringLiteral("write failed: %1").arg(out.errorString()));
    }
    out.close();

    // QFile::rename does not replace a file: the first free name wins.
    for (int n = 1; n <= MAX_NUMBERED_NAMES; ++n) {
        const QString suffix =
            n == 1 ? QString() : QStringLiteral("-%1").arg(n);
        const QString target =
            QStringLiteral("%1/%2%3.zip").arg(directory, name, suffix);
        if (QFileInfo::exists(target)) {
            continue;
        }
        if (out.rename(target)) {
            return target;
        }
    }
    return fail(
        QStringLiteral("cannot name the zip in %1: all names are taken or "
                       "the rename failed")
            .arg(directory));
}

}  // namespace roost
