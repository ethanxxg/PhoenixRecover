#include "SqliteRecoverService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace SqliteRecoverService {

namespace {

uint16_t ReadBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t ReadBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteBE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

size_t FindHeaderOffset(const uint8_t* input, size_t inputLen, size_t maxSearch)
{
    static const char kMagic[] = "SQLite format 3";
    const size_t magicLen = 16;
    if (inputLen < magicLen) {
        return static_cast<size_t>(-1);
    }
    const size_t limit = std::min(inputLen - magicLen, maxSearch);
    for (size_t i = 0; i <= limit; ++i) {
        if (memcmp(input + i, kMagic, magicLen) == 0) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

uint8_t PageTypeAt(const uint8_t* pageStart, uint32_t pageSize, uint32_t pageNumber)
{
    if (pageNumber == 1) {
        if (pageSize <= kHeaderSize) {
            return 0;
        }
        return pageStart[kHeaderSize];
    }
    return pageStart[0];
}

bool ValidatePage(const uint8_t* pageStart, uint32_t pageSize, uint32_t pageNumber)
{
    if (!pageStart || pageSize < kHeaderSize) {
        return false;
    }
    if (pageNumber == 1) {
        if (!IsSqliteMagic(pageStart, kHeaderSize)) {
            return false;
        }
    }
    const uint8_t t = PageTypeAt(pageStart, pageSize, pageNumber);
    return IsValidPageType(t);
}

void PatchHeaderPageCount(uint8_t* header100, uint32_t pageCount)
{
    WriteBE32(header100 + 28, pageCount);
}

} // namespace

bool IsSqliteMagic(const uint8_t* data, size_t len)
{
    static const char kMagic[] = "SQLite format 3";
    return data && len >= 16 && memcmp(data, kMagic, 16) == 0;
}

uint32_t ReadPageSizeFromHeader(const uint8_t* header100)
{
    if (!header100) {
        return 0;
    }
    const uint16_t raw = ReadBE16(header100 + 16);
    if (raw == 1) {
        return 65536u;
    }
    if (raw == 0) {
        return 0;
    }
    return static_cast<uint32_t>(raw) * 256u;
}

bool IsValidPageType(uint8_t pageType)
{
    switch (pageType) {
    case 0x02: // interior index
    case 0x03: // leaf index
    case 0x05: // interior table
    case 0x0a: // leaf table
    case 0x0d: // freelist
    case 0x10: // overflow
        return true;
    default:
        return false;
    }
}

RecoverReport RecoverDatabase(const uint8_t* input,
                              size_t inputLen,
                              std::vector<uint8_t>& outputDb,
                              const RecoverOptions& opt)
{
    RecoverReport report;
    outputDb.clear();

    if (!input || inputLen < kHeaderSize) {
        report.message = L"Input too small for SQLite header";
        return report;
    }

    const size_t hdrOff = FindHeaderOffset(input, inputLen, opt.maxHeaderSearchBytes);
    if (hdrOff == static_cast<size_t>(-1)) {
        report.message = L"SQLite format 3 header not found";
        return report;
    }

    report.sourceOffset = hdrOff;
    const uint8_t* base = input + hdrOff;
    const size_t avail = inputLen - hdrOff;

    if (avail < kHeaderSize) {
        report.message = L"Truncated header after magic";
        return report;
    }

    const uint32_t pageSize = ReadPageSizeFromHeader(base);
    if (pageSize < 512 || (pageSize & (pageSize - 1)) != 0) {
        report.message = L"Invalid SQLite page size";
        return report;
    }

    report.pageSize = pageSize;
    const size_t totalPagesAvail = avail / pageSize;
    if (totalPagesAvail == 0) {
        report.message = L"No full pages in input";
        return report;
    }

    std::vector<std::vector<uint8_t>> pages;
    pages.reserve(totalPagesAvail);

    for (size_t i = 0; i < totalPagesAvail; ++i) {
        const uint32_t pageNum = static_cast<uint32_t>(i + 1);
        const uint8_t* pagePtr = base + i * pageSize;
        if (!ValidatePage(pagePtr, pageSize, pageNum)) {
            if (opt.stopAtFirstBadPage) {
                break;
            }
            ++report.pagesSkipped;
            continue;
        }

        pages.emplace_back(pagePtr, pagePtr + pageSize);
        ++report.pagesCopied;
    }

    if (pages.empty()) {
        report.message = L"No valid B-tree pages found";
        return report;
    }

    outputDb.clear();
    outputDb.reserve(pages.size() * static_cast<size_t>(pageSize));

    for (size_t i = 0; i < pages.size(); ++i) {
        auto& pg = pages[i];
        if (i == 0 && pg.size() >= kHeaderSize) {
            PatchHeaderPageCount(pg.data(), static_cast<uint32_t>(pages.size()));
        }
        outputDb.insert(outputDb.end(), pg.begin(), pg.end());
    }

    report.success = true;
    report.message = L"OK";
    return report;
}

RecoverReport RecoverFile(const std::wstring& inputPath,
                          const std::wstring& outputPath,
                          const RecoverOptions& opt)
{
    RecoverReport report;
    std::ifstream in(std::filesystem::path(inputPath), std::ios::binary);
    if (!in) {
        report.message = L"Cannot open input file";
        return report;
    }
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0) {
        report.message = L"Empty input file";
        return report;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(end));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(buf.data()), end);
    if (!in) {
        report.message = L"Failed to read input file";
        return report;
    }

    std::vector<uint8_t> out;
    report = RecoverDatabase(buf.data(), buf.size(), out, opt);
    if (!report.success) {
        return report;
    }

    std::ofstream outFile(std::filesystem::path(outputPath),
                          std::ios::binary | std::ios::trunc);
    if (!outFile || !outFile.write(reinterpret_cast<const char*>(out.data()),
                                   static_cast<std::streamsize>(out.size()))) {
        report.success = false;
        report.message = L"Cannot write output file";
        return report;
    }
    return report;
}

} // namespace SqliteRecoverService
