#include "fragment_fat_structured_fixture.h"

#include "../../fs/fat/FatStructs.h"

#include <algorithm>
#include <cstring>

namespace {

static void WriteLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void WriteLe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

static void WriteAt(uint8_t* c2, uint8_t* c3, uint32_t sectorSize, uint32_t off,
                    const void* data, size_t n)
{
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (n > 0) {
        if (off < sectorSize) {
            const size_t chunk = std::min(n, static_cast<size_t>(sectorSize - off));
            memcpy(c2 + off, src, chunk);
            src += chunk;
            off += static_cast<uint32_t>(chunk);
            n -= chunk;
        } else {
            const uint32_t off3 = off - sectorSize;
            const size_t chunk = std::min(n, static_cast<size_t>(sectorSize - off3));
            memcpy(c3 + off3, src, chunk);
            src += chunk;
            off += static_cast<uint32_t>(chunk);
            n -= chunk;
        }
    }
}

static void WriteDeletedEntry(uint8_t* rootSector,
                              const char* ext3,
                              uint32_t firstCluster,
                              uint32_t fileSize)
{
    auto* e = reinterpret_cast<FAT_DIR_ENTRY*>(rootSector);
    memset(e, 0, sizeof(FAT_DIR_ENTRY));
    e->name[0] = FAT_DELETED_MARKER;
    memcpy(e->name + 1, "HOENIX  ", 7);
    memcpy(e->ext, ext3, 3);
    e->attr        = FAT_ATTR_ARCHIVE;
    e->clusterLow  = static_cast<uint16_t>(firstCluster & 0xFFFF);
    e->clusterHigh = 0;
    e->fileSize    = fileSize;
}

static void WriteJpegPayload(uint8_t* c2, uint8_t* c3, uint32_t fileSize, uint32_t sectorSize)
{
    memset(c2, 0xCC, sectorSize);
    memset(c3, 0xCC, sectorSize);
    c2[0] = 0xFF;
    c2[1] = 0xD8;
    const uint8_t eoi[2] = { 0xFF, 0xD9 };
    WriteAt(c2, c3, sectorSize, fileSize - 2, eoi, 2);
}

static void WritePngPayload(uint8_t* c2, uint8_t* c3, uint32_t fileSize, uint32_t sectorSize)
{
    memset(c2, 0, sectorSize);
    memset(c3, 0, sectorSize);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    WriteAt(c2, c3, sectorSize, 0, sig, sizeof(sig));
    const uint8_t ihdr[] = {
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    WriteAt(c2, c3, sectorSize, 8, ihdr, sizeof(ihdr));
    const uint8_t iend[] = { 0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D',
                             0xAE, 0x42, 0x60, 0x82 };
    WriteAt(c2, c3, sectorSize, fileSize - sizeof(iend), iend, sizeof(iend));
}

static void WritePdfPayload(uint8_t* c2, uint8_t* c3, uint32_t fileSize, uint32_t sectorSize)
{
    memset(c2, 0x20, sectorSize);
    memset(c3, 0x20, sectorSize);
    WriteAt(c2, c3, sectorSize, 0, "%PDF-1.4\n", 9);
    WriteAt(c2, c3, sectorSize, fileSize - 6, "%%EOF\n", 6);
}

static void WriteZipPayload(uint8_t* c2, uint8_t* c3, uint32_t fileSize, uint32_t sectorSize)
{
    memset(c2, 0, sectorSize);
    memset(c3, 0, sectorSize);

    constexpr uint32_t kEocdSize = 22;
    constexpr uint32_t kNameLen    = 4;
    constexpr uint32_t kLocalHdr  = 30;
    const uint32_t eocdOff = fileSize - kEocdSize;
    const uint32_t dataSize = eocdOff - (kLocalHdr + kNameLen);

    uint8_t local[kLocalHdr + kNameLen]{};
    local[0] = 'P';
    local[1] = 'K';
    local[2] = 0x03;
    local[3] = 0x04;
    WriteLe32(local + 12, dataSize);
    WriteLe32(local + 16, dataSize);
    WriteLe16(local + 20, kNameLen);
    memcpy(local + kLocalHdr, "DATA", kNameLen);
    WriteAt(c2, c3, sectorSize, 0, local, sizeof(local));

    uint8_t eocd[kEocdSize]{};
    eocd[0] = 'P';
    eocd[1] = 'K';
    eocd[2] = 0x05;
    eocd[3] = 0x06;
    WriteLe16(eocd + 8, 1);
    WriteLe16(eocd + 10, 1);
    WriteAt(c2, c3, sectorSize, eocdOff, eocd, sizeof(eocd));
}

static void WritePayload(FragmentFatStructuredFixture::StructuredFormat format,
                         uint8_t* c2,
                         uint8_t* c3,
                         uint32_t fileSize,
                         uint32_t sectorSize)
{
    using FragmentFatStructuredFixture::StructuredFormat;
    switch (format) {
    case StructuredFormat::Jpeg:
        WriteJpegPayload(c2, c3, fileSize, sectorSize);
        break;
    case StructuredFormat::Png:
        WritePngPayload(c2, c3, fileSize, sectorSize);
        break;
    case StructuredFormat::Pdf:
        WritePdfPayload(c2, c3, fileSize, sectorSize);
        break;
    case StructuredFormat::Zip:
        WriteZipPayload(c2, c3, fileSize, sectorSize);
        break;
    }
}

} // namespace

namespace FragmentFatStructuredFixture {

const wchar_t* FormatExtension(StructuredFormat format)
{
    switch (format) {
    case StructuredFormat::Jpeg: return L"jpg";
    case StructuredFormat::Png:  return L"png";
    case StructuredFormat::Pdf:  return L"pdf";
    case StructuredFormat::Zip:  return L"zip";
    }
    return L"";
}

void BuildFat16StructuredVolume(std::vector<uint8_t>& image,
                                StructuredFormat format,
                                FatChainMode mode)
{
    constexpr uint32_t kSectorSize   = 512;
    constexpr uint32_t kTotalSectors  = 4200;
    constexpr uint32_t kReserved     = 1;
    constexpr uint32_t kNumFats      = 2;
    constexpr uint32_t kFatSectors   = 1;
    constexpr uint32_t kRootEntries  = 16;
    constexpr uint32_t kSpc          = 1;
    constexpr uint32_t kRootSector   = kReserved + kNumFats * kFatSectors;
    constexpr uint32_t kDataStart    = kRootSector + 1;
    constexpr uint32_t kFirstCluster = 2;

    const char* ext3 = "JPG";
    switch (format) {
    case StructuredFormat::Jpeg: ext3 = "JPG"; break;
    case StructuredFormat::Png:  ext3 = "PNG"; break;
    case StructuredFormat::Pdf:  ext3 = "PDF"; break;
    case StructuredFormat::Zip:  ext3 = "ZIP"; break;
    }

    image.assign(static_cast<size_t>(kTotalSectors) * kSectorSize, 0);

    uint8_t* boot = image.data();
    boot[0] = 0xEB;
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    WriteLe16(boot + 11, static_cast<uint16_t>(kSectorSize));
    boot[13] = static_cast<uint8_t>(kSpc);
    WriteLe16(boot + 14, static_cast<uint16_t>(kReserved));
    boot[16] = static_cast<uint8_t>(kNumFats);
    WriteLe16(boot + 17, static_cast<uint16_t>(kRootEntries));
    WriteLe16(boot + 19, static_cast<uint16_t>(kTotalSectors));
    boot[21] = 0xF8;
    WriteLe16(boot + 22, static_cast<uint16_t>(kFatSectors));
    boot[36] = 0x29;
    memcpy(boot + 54, "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    auto writeFat = [&](uint32_t fatSectorIndex) {
        uint8_t* fat = image.data() + fatSectorIndex * kSectorSize;
        fat[0] = 0xF8;
        fat[1] = 0xFF;
        WriteLe16(fat + 2, 0xFFFF);
        if (mode == FatChainMode::Intact) {
            WriteLe16(fat + 4, 0x0003);
            WriteLe16(fat + 6, 0xFFFF);
            WriteLe16(fat + 8, 0x0000);
        } else {
            WriteLe16(fat + 4, 0xFFFF);
            WriteLe16(fat + 6, 0x0000);
            WriteLe16(fat + 8, 0x0000);
        }
    };
    writeFat(1);
    writeFat(2);

    WriteDeletedEntry(image.data() + kRootSector * kSectorSize, ext3, kFirstCluster,
                      kStructuredFileSize);

    const auto clusterSector = [&](uint32_t cluster) -> uint8_t* {
        return image.data() + static_cast<size_t>(kDataStart + (cluster - 2) * kSpc) * kSectorSize;
    };

    WritePayload(format, clusterSector(2), clusterSector(3), kStructuredFileSize, kSectorSize);
    if (mode == FatChainMode::BrokenWithDecoy) {
        memset(clusterSector(4), 0x00, kSectorSize);
    }
}

} // namespace FragmentFatStructuredFixture
