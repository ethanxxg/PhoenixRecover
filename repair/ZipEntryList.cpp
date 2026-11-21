#include "ZipEntryList.h"

#include <QFile>
#include <cstring>

namespace {

constexpr uint32_t kLocalFileSig  = 0x04034b50;
constexpr uint32_t kCentralDirSig = 0x02014b50;
constexpr uint32_t kDataDescSig   = 0x08074b50;

uint32_t readLE32(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(u[0] | (u[1] << 8) | (u[2] << 16) | (u[3] << 24));
}

uint16_t readLE16(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(u[0] | (u[1] << 8));
}

} // namespace

QStringList listZipLocalEntryNames(QFile& input)
{
    QStringList names;
    const qint64 fileSize = input.size();
    qint64 pos = 0;

    while (pos + 30 <= fileSize) {
        input.seek(pos);
        QByteArray sig = input.read(4);
        if (sig.size() < 4) {
            break;
        }

        if (readLE32(sig.constData()) != kLocalFileSig) {
            ++pos;
            continue;
        }

        QByteArray hdr = input.read(26);
        if (hdr.size() < 26) {
            break;
        }

        const uint16_t flags = readLE16(hdr.constData() + 2);
        uint32_t compressedSize = readLE32(hdr.constData() + 14);
        const uint16_t nameLen = readLE16(hdr.constData() + 22);
        const uint16_t extraLen = readLE16(hdr.constData() + 24);

        const QByteArray fileName = input.read(nameLen);
        input.read(extraLen);

        qint64 dataOffset = pos + 30 + nameLen + extraLen;
        qint64 dataEndOffset = dataOffset + compressedSize;

        if (flags & 0x0008) {
            qint64 searchPos = dataOffset;
            bool found = false;
            while (searchPos + 4 <= fileSize) {
                input.seek(searchPos);
                QByteArray probe = input.read(4);
                if (probe.size() < 4) {
                    break;
                }
                const uint32_t probeSig = readLE32(probe.constData());
                if (probeSig == kDataDescSig) {
                    dataEndOffset = searchPos + 16;
                    found = true;
                    break;
                }
                if (probeSig == kLocalFileSig || probeSig == kCentralDirSig) {
                    compressedSize = static_cast<uint32_t>(searchPos - dataOffset);
                    dataEndOffset = searchPos;
                    found = true;
                    break;
                }
                ++searchPos;
            }
            if (!found) {
                dataEndOffset = fileSize;
            }
        }

        if (dataEndOffset > fileSize) {
            dataEndOffset = fileSize;
        }

        names.append(QString::fromUtf8(fileName));
        pos = dataEndOffset;
    }

    return names;
}

QStringList listZipLocalEntryNamesFromPath(const QString& zipPath)
{
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return listZipLocalEntryNames(f);
}
