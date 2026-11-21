#include "Mp4Repair.h"
#include "RepairFfmpeg.h"
#include <QFileInfo>
#include <cstring>

namespace {

inline uint32_t readBE32(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>((u[0] << 24) | (u[1] << 16) | (u[2] << 8) | u[3]);
}

inline uint64_t readBE64(const char* p)
{
    uint64_t hi = readBE32(p);
    uint64_t lo = readBE32(p + 4);
    return (hi << 32) | lo;
}

void writeBE32(char* p, uint32_t v)
{
    auto* u = reinterpret_cast<uint8_t*>(p);
    u[0] = static_cast<uint8_t>(v >> 24);
    u[1] = static_cast<uint8_t>(v >> 16);
    u[2] = static_cast<uint8_t>(v >> 8);
    u[3] = static_cast<uint8_t>(v);
}

void writeBE64(char* p, uint64_t v)
{
    writeBE32(p, static_cast<uint32_t>(v >> 32));
    writeBE32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFF));
}

bool copyData(QFile& src, QFile& dst, qint64 offset, qint64 size, std::atomic<bool>* cancel)
{
    src.seek(offset);
    constexpr qint64 kChunkSize = 256 * 1024;
    qint64 remaining = size;
    while (remaining > 0) {
        if (cancel && cancel->load()) return false;
        qint64 toRead = qMin(remaining, kChunkSize);
        QByteArray chunk = src.read(toRead);
        if (chunk.isEmpty()) return false;
        dst.write(chunk);
        remaining -= chunk.size();
    }
    return true;
}

} // namespace

QString Mp4Repair::formatName() const
{
    return QStringLiteral("MP4");
}

QStringList Mp4Repair::supportedExtensions() const
{
    return {
        QStringLiteral("mp4"), QStringLiteral("mov"),
        QStringLiteral("m4v"), QStringLiteral("m4a")
    };
}

bool Mp4Repair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 24) return false;
    if (header.size() < 12) return false;

    const auto* h = reinterpret_cast<const uint8_t*>(header.constData());
    uint32_t boxSize = (h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];

    // Check for ftyp box
    if (boxSize >= 8 && boxSize <= static_cast<uint32_t>(fileSize)) {
        if (h[4] == 'f' && h[5] == 't' && h[6] == 'y' && h[7] == 'p')
            return true;
    }

    // Also accept if we can find mdat signature nearby
    if (header.size() >= 12) {
        for (int i = 0; i + 8 <= header.size() && i < 64; i += 4) {
            const auto* p = reinterpret_cast<const uint8_t*>(header.constData() + i + 4);
            if (p[0] == 'm' && p[1] == 'd' && p[2] == 'a' && p[3] == 't')
                return true;
        }
    }

    return false;
}

std::vector<Mp4Repair::BoxInfo> Mp4Repair::parseTopLevelBoxes(QFile& input)
{
    std::vector<BoxInfo> boxes;
    qint64 fileSize = input.size();
    qint64 pos = 0;

    while (pos + 8 <= fileSize) {
        input.seek(pos);
        QByteArray hdr = input.read(16);
        if (hdr.size() < 8) break;

        uint32_t size32 = readBE32(hdr.constData());
        QByteArray type = hdr.mid(4, 4);

        qint64 boxSize = size32;
        if (size32 == 1) {
            if (hdr.size() < 16) break;
            boxSize = static_cast<qint64>(readBE64(hdr.constData() + 8));
        } else if (size32 == 0) {
            boxSize = fileSize - pos;
        }

        if (boxSize < 8) break;

        BoxInfo box;
        box.type   = type;
        box.offset = pos;
        box.size   = boxSize;
        boxes.push_back(box);

        pos += boxSize;
    }

    return boxes;
}

bool Mp4Repair::adjustChunkOffsets(QByteArray& moovData, qint64 offsetDelta)
{
    // Recursively search for 'stco' and 'co64' boxes within moovData
    // moovData includes the 8-byte box header (size + type) at the start
    if (moovData.size() < 8) return false;

    // Search for stco (sample table chunk offset) and co64 atoms
    const char* data = moovData.constData();
    int len = moovData.size();

    for (int i = 0; i + 8 <= len; ++i) {
        if (std::memcmp(data + i + 4, "stco", 4) == 0) {
            // stco: size(4) type(4) version(1) flags(3) entryCount(4) entries(4*N)
            int boxStart = i;
            uint32_t boxSize = readBE32(data + boxStart);
            if (boxSize < 16 || boxStart + static_cast<int>(boxSize) > len)
                continue;

            int payloadStart = boxStart + 8; // skip size+type
            if (payloadStart + 8 > len) continue;

            uint32_t entryCount = readBE32(data + payloadStart + 4);
            int entriesStart = payloadStart + 8;

            if (entriesStart + static_cast<int>(entryCount) * 4 > len) continue;

            char* mutable_data = moovData.data();
            for (uint32_t e = 0; e < entryCount; ++e) {
                int off = entriesStart + e * 4;
                uint32_t oldOffset = readBE32(mutable_data + off);
                uint32_t newOffset = static_cast<uint32_t>(oldOffset + offsetDelta);
                writeBE32(mutable_data + off, newOffset);
            }
        } else if (std::memcmp(data + i + 4, "co64", 4) == 0) {
            int boxStart = i;
            uint32_t boxSize = readBE32(data + boxStart);
            if (boxSize < 16 || boxStart + static_cast<int>(boxSize) > len)
                continue;

            int payloadStart = boxStart + 8;
            if (payloadStart + 8 > len) continue;

            uint32_t entryCount = readBE32(data + payloadStart + 4);
            int entriesStart = payloadStart + 8;

            if (entriesStart + static_cast<int>(entryCount) * 8 > len) continue;

            char* mutable_data = moovData.data();
            for (uint32_t e = 0; e < entryCount; ++e) {
                int off = entriesStart + e * 8;
                uint64_t oldOffset = readBE64(mutable_data + off);
                uint64_t newOffset = oldOffset + static_cast<uint64_t>(offsetDelta);
                writeBE64(mutable_data + off, newOffset);
            }
        }
    }

    return true;
}

bool Mp4Repair::relocateMoov(QFile& input, QFile& output, std::atomic<bool>* cancel)
{
    auto boxes = parseTopLevelBoxes(input);

    const BoxInfo* ftypBox = nullptr;
    const BoxInfo* moovBox = nullptr;
    const BoxInfo* mdatBox = nullptr;

    for (auto& box : boxes) {
        if (box.type == "ftyp") ftypBox = &box;
        else if (box.type == "moov") moovBox = &box;
        else if (box.type == "mdat") mdatBox = &box;
    }

    if (!moovBox || !mdatBox) return false;

    // If moov is already before mdat, file structure is fine
    if (moovBox->offset < mdatBox->offset) {
        // Just copy the file as-is (may need offset correction but structure is ok)
        input.seek(0);
        if (!copyData(input, output, 0, input.size(), cancel))
            return false;
        return true;
    }

    // moov is after mdat: relocate moov to before mdat
    input.seek(moovBox->offset);
    QByteArray moovData = input.read(moovBox->size);
    if (moovData.size() != moovBox->size) return false;

    // Calculate offset adjustment: mdat data offsets will increase by moov size
    qint64 offsetDelta = moovBox->size;
    adjustChunkOffsets(moovData, offsetDelta);

    // Write ftyp first
    if (ftypBox) {
        if (!copyData(input, output, ftypBox->offset, ftypBox->size, cancel))
            return false;
    }

    if (cancel && cancel->load()) return false;

    // Write relocated moov
    output.write(moovData);

    if (cancel && cancel->load()) return false;

    // Write mdat and any other boxes (excluding ftyp and moov)
    for (auto& box : boxes) {
        if (cancel && cancel->load()) return false;
        if (box.type == "ftyp" || box.type == "moov") continue;
        if (!copyData(input, output, box.offset, box.size, cancel))
            return false;
    }

    return true;
}

bool Mp4Repair::tryFfmpegRemux(const QString& inputPath, const QString& outputPath)
{
    return RepairFfmpeg::remuxVideo(inputPath, outputPath, QStringLiteral("mp4"));
}

RepairResult Mp4Repair::repair(const QString& inputPath,
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

    auto boxes = parseTopLevelBoxes(input);

    bool hasMoov = false;
    bool hasMdat = false;
    bool hasFtyp = false;

    for (auto& box : boxes) {
        if (box.type == "moov") hasMoov = true;
        if (box.type == "mdat") hasMdat = true;
        if (box.type == "ftyp") hasFtyp = true;
    }

    if (cancel && cancel->load()) {
        result.message = QStringLiteral("修复已取消");
        return result;
    }

    // Strategy A: moov exists, try relocate
    if (hasMoov) {
        QFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.message = QStringLiteral("无法创建输出文件");
            return result;
        }

        if (relocateMoov(input, output, cancel)) {
            output.close();
            result.success = true;
            result.repairedSize = QFileInfo(outputPath).size();
            result.message = QStringLiteral("moov atom 重定位成功");
            return result;
        }

        output.close();
        QFile::remove(outputPath);

        if (cancel && cancel->load()) {
            result.message = QStringLiteral("修复已取消");
            return result;
        }
    }

    input.close();

    // Strategy B: FFmpeg remux
    if (tryFfmpegRemux(inputPath, outputPath)) {
        result.success = true;
        result.repairedSize = QFileInfo(outputPath).size();
        result.message = QStringLiteral("FFmpeg remux 修复成功");
        return result;
    }

    // All strategies failed
    if (!hasMoov && !hasFtyp) {
        result.message = QStringLiteral("文件缺少 ftyp 和 moov，无法修复");
    } else if (!hasMoov) {
        result.message = QStringLiteral("moov atom 缺失且 FFmpeg 不可用或修复失败");
    } else {
        result.message = QStringLiteral("所有修复策略均失败");
    }

    return result;
}
