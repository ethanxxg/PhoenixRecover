#include "ZipRepair.h"

#ifdef PHOENIX_HAVE_LIBZIP
#include <zip.h>
#endif

#include <QDataStream>
#include <QtEndian>
#include <cstring>

namespace {

inline uint16_t readLE16(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(u[0] | (u[1] << 8));
}

inline uint32_t readLE32(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(u[0] | (u[1] << 8) | (u[2] << 16) | (u[3] << 24));
}

void writeLE16(char* p, uint16_t v)
{
    auto* u = reinterpret_cast<uint8_t*>(p);
    u[0] = static_cast<uint8_t>(v);
    u[1] = static_cast<uint8_t>(v >> 8);
}

void writeLE32(char* p, uint32_t v)
{
    auto* u = reinterpret_cast<uint8_t*>(p);
    u[0] = static_cast<uint8_t>(v);
    u[1] = static_cast<uint8_t>(v >> 8);
    u[2] = static_cast<uint8_t>(v >> 16);
    u[3] = static_cast<uint8_t>(v >> 24);
}

constexpr uint32_t kLocalFileSig  = 0x04034b50;
constexpr uint32_t kCentralDirSig = 0x02014b50;
constexpr uint32_t kEocdSig       = 0x06054b50;
constexpr uint32_t kDataDescSig   = 0x08074b50;

} // namespace

QString ZipRepair::formatName() const
{
    return QStringLiteral("ZIP");
}

QStringList ZipRepair::supportedExtensions() const
{
    return {
        QStringLiteral("zip"),
        QStringLiteral("jar"),
        QStringLiteral("apk"),
    };
}

bool ZipRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 22) return false;
    if (header.size() < 4) return false;

    auto b0 = static_cast<uint8_t>(header[0]);
    auto b1 = static_cast<uint8_t>(header[1]);
    if (b0 != 'P' || b1 != 'K') return false;

    auto b2 = static_cast<uint8_t>(header[2]);
    auto b3 = static_cast<uint8_t>(header[3]);

    if (b2 == 0x03 && b3 == 0x04) return true;
    if (b2 == 0x05 && b3 == 0x06) return true;

    // ZIP64 not supported in v0
    if (b2 == 0x06 && b3 == 0x06) return false;

    return false;
}

std::vector<ZipRepair::LocalFileEntry> ZipRepair::scanLocalHeaders(QFile& input)
{
    std::vector<LocalFileEntry> entries;
    const qint64 fileSize = input.size();
    qint64 pos = 0;

    while (pos + 30 <= fileSize) {
        input.seek(pos);
        QByteArray sig = input.read(4);
        if (sig.size() < 4) break;

        if (readLE32(sig.constData()) != kLocalFileSig) {
            ++pos;
            continue;
        }

        QByteArray hdr = input.read(26);
        if (hdr.size() < 26) break;

        LocalFileEntry entry;
        entry.localHeaderOffset = pos;
        entry.versionNeeded    = readLE16(hdr.constData() + 0);
        entry.flags            = readLE16(hdr.constData() + 2);
        entry.method           = readLE16(hdr.constData() + 4);
        entry.modTime          = readLE16(hdr.constData() + 6);
        entry.modDate          = readLE16(hdr.constData() + 8);
        entry.crc32            = readLE32(hdr.constData() + 10);
        entry.compressedSize   = readLE32(hdr.constData() + 14);
        entry.uncompressedSize = readLE32(hdr.constData() + 18);
        uint16_t nameLen       = readLE16(hdr.constData() + 22);
        uint16_t extraLen      = readLE16(hdr.constData() + 24);

        entry.fileName   = input.read(nameLen);
        entry.extraField = input.read(extraLen);

        entry.dataOffset = pos + 30 + nameLen + extraLen;

        if (entry.flags & 0x0008) {
            // Data Descriptor: sizes are after data, search for next local header or descriptor
            qint64 searchPos = entry.dataOffset;
            bool found = false;

            while (searchPos + 4 <= fileSize) {
                input.seek(searchPos);
                QByteArray probe = input.read(4);
                if (probe.size() < 4) break;

                uint32_t probeSig = readLE32(probe.constData());

                if (probeSig == kDataDescSig) {
                    QByteArray desc = input.read(12);
                    if (desc.size() >= 12) {
                        entry.crc32          = readLE32(desc.constData() + 0);
                        entry.compressedSize = readLE32(desc.constData() + 4);
                        entry.uncompressedSize = readLE32(desc.constData() + 8);
                        entry.dataEndOffset  = searchPos + 16;
                        found = true;
                    }
                    break;
                }

                if (probeSig == kLocalFileSig || probeSig == kCentralDirSig) {
                    entry.compressedSize = static_cast<uint32_t>(searchPos - entry.dataOffset);
                    entry.dataEndOffset  = searchPos;
                    found = true;
                    break;
                }

                ++searchPos;
            }

            if (!found) {
                entry.compressedSize = static_cast<uint32_t>(fileSize - entry.dataOffset);
                entry.dataEndOffset  = fileSize;
            }
        } else {
            entry.dataEndOffset = entry.dataOffset + entry.compressedSize;
        }

        if (entry.dataEndOffset > fileSize)
            entry.dataEndOffset = fileSize;

        entries.push_back(std::move(entry));
        pos = entries.back().dataEndOffset;
    }

    return entries;
}

bool ZipRepair::rebuildCentralDirectory(QFile& input, QFile& output,
                                         const std::vector<LocalFileEntry>& entries,
                                         std::atomic<bool>* cancel)
{
    // Copy all local file headers and data to output
    for (auto& entry : entries) {
        if (cancel && cancel->load()) return false;

        input.seek(entry.localHeaderOffset);
        qint64 localLen = entry.dataEndOffset - entry.localHeaderOffset;
        constexpr qint64 kChunkSize = 64 * 1024;
        qint64 remaining = localLen;

        while (remaining > 0) {
            qint64 toRead = qMin(remaining, kChunkSize);
            QByteArray chunk = input.read(toRead);
            if (chunk.isEmpty()) return false;
            output.write(chunk);
            remaining -= chunk.size();
        }
    }

    // Build Central Directory
    qint64 cdStart = output.pos();

    for (auto& entry : entries) {
        if (cancel && cancel->load()) return false;

        // Central Directory File Header: 46 bytes fixed + nameLen + extraLen
        char cdh[46]{};
        writeLE32(cdh + 0, kCentralDirSig);
        writeLE16(cdh + 4, 20);                         // version made by
        writeLE16(cdh + 6, entry.versionNeeded);        // version needed
        writeLE16(cdh + 8, entry.flags & ~0x0008);      // clear data descriptor flag
        writeLE16(cdh + 10, entry.method);
        writeLE16(cdh + 12, entry.modTime);
        writeLE16(cdh + 14, entry.modDate);
        writeLE32(cdh + 16, entry.crc32);
        writeLE32(cdh + 20, entry.compressedSize);
        writeLE32(cdh + 24, entry.uncompressedSize);
        writeLE16(cdh + 28, static_cast<uint16_t>(entry.fileName.size()));
        writeLE16(cdh + 30, static_cast<uint16_t>(entry.extraField.size()));
        writeLE16(cdh + 32, 0);                         // comment length
        writeLE16(cdh + 34, 0);                         // disk number start
        writeLE16(cdh + 36, 0);                         // internal attributes
        writeLE32(cdh + 38, 0);                         // external attributes
        writeLE32(cdh + 42, static_cast<uint32_t>(entry.localHeaderOffset));

        output.write(cdh, 46);
        output.write(entry.fileName);
        output.write(entry.extraField);
    }

    qint64 cdEnd = output.pos();
    qint64 cdSize = cdEnd - cdStart;

    // Build EOCD
    char eocd[22]{};
    writeLE32(eocd + 0, kEocdSig);
    writeLE16(eocd + 4, 0);                             // disk number
    writeLE16(eocd + 6, 0);                             // disk with CD
    writeLE16(eocd + 8, static_cast<uint16_t>(entries.size()));
    writeLE16(eocd + 10, static_cast<uint16_t>(entries.size()));
    writeLE32(eocd + 12, static_cast<uint32_t>(cdSize));
    writeLE32(eocd + 16, static_cast<uint32_t>(cdStart));
    writeLE16(eocd + 20, 0);                            // comment length

    output.write(eocd, 22);
    return true;
}

namespace {
bool verifyZipWithLibzip(const QString& path, QString* errOut)
{
#ifdef PHOENIX_HAVE_LIBZIP
    int ze = 0;
    zip_t* archive = zip_open(path.toUtf8().constData(), ZIP_RDONLY, &ze);
    if (!archive) {
        if (errOut) {
            *errOut = QStringLiteral("libzip 无法打开输出文件 (code %1)").arg(ze);
        }
        return false;
    }
    const zip_int64_t n = zip_get_num_entries(archive, 0);
    zip_close(archive);
    if (n <= 0) {
        if (errOut) {
            *errOut = QStringLiteral("libzip 未检测到任何条目");
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(errOut);
    return true;
#endif
}
} // namespace

RepairResult ZipRepair::repair(const QString& inputPath,
                                const QString& outputPath,
                                std::atomic<bool>* cancel)
{
    RepairResult result;

    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        result.message = QStringLiteral("无法打开输入文件");
        return result;
    }
    result.originalSize = input.size();

    auto entries = scanLocalHeaders(input);
    if (entries.empty()) {
        result.message = QStringLiteral("未找到任何有效的 ZIP 本地文件头");
        return result;
    }

    if (cancel && cancel->load()) {
        result.message = QStringLiteral("修复已取消");
        return result;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.message = QStringLiteral("无法创建输出文件");
        return result;
    }

    if (!rebuildCentralDirectory(input, output, entries, cancel)) {
        output.close();
        QFile::remove(outputPath);
        result.message = cancel && cancel->load()
            ? QStringLiteral("修复已取消")
            : QStringLiteral("重建中心目录失败");
        return result;
    }

    output.close();

    // Basic validation: output file should start with PK\x03\x04
    QFile verify(outputPath);
    if (verify.open(QIODevice::ReadOnly)) {
        QByteArray head = verify.read(4);
        if (head.size() < 4 || readLE32(head.constData()) != kLocalFileSig) {
            verify.close();
            QFile::remove(outputPath);
            result.message = QStringLiteral("输出文件验证失败");
            return result;
        }
        result.repairedSize = verify.size();
        verify.close();
    }

    QString libzipErr;
    if (!verifyZipWithLibzip(outputPath, &libzipErr)) {
        QFile::remove(outputPath);
        result.message = libzipErr.isEmpty()
            ? QStringLiteral("ZIP 归档验证失败")
            : libzipErr;
        return result;
    }

    result.success = true;
    result.message = QString("成功修复，包含 %1 个文件条目").arg(entries.size());
    return result;
}
