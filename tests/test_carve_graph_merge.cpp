// CarveGraphMergeService — cluster shuffled RAW FileNodes and graph-stitch.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "recover/FileNode.h"
#include "recover/FragmentStitchPolicy.h"
#include "scan/CarveGraphMergeService.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static const uint8_t kMinimalJpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
    0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
    0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20,
    0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29, 0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27,
    0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F,
    0x00, 0xFE, 0xFF, 0xD9,
};

static std::wstring WriteTempImage(const std::vector<uint8_t>& image)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path =
        std::wstring(tmp) + L"PhoenixRecover_carve_merge_" + std::to_wstring(GetTickCount64()) + L".img";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return L"";
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) &&
                    written == image.size();
    CloseHandle(h);
    return ok ? path : L"";
}

static FileNode MakeRawNode(uint64_t offset, uint64_t size, const wchar_t* name, const wchar_t* ext)
{
    FileNode n;
    n.name = name;
    n.extension = ext;
    n.originalPath = std::wstring(L"\\$RAW\\") + name;
    n.size = size;
    n.rawOffset = offset;
    n.rawSize = size;
    n.fsType = L"RAW";
    n.status = FileStatus::Lost;
    n.recoverability = RecoverabilityLevel::Poor;
    return n;
}

int main()
{
    const size_t jpegLen = sizeof(kMinimalJpeg);
    const size_t third = jpegLen / 3;
    const uint64_t off0 = 4096;
    const uint64_t off1 = 8192;
    const uint64_t off2 = 12288;
    const uint64_t imageSize = 16384;

    std::vector<uint8_t> image(imageSize, 0xFF);
    memcpy(image.data() + off0, kMinimalJpeg, third);
    memcpy(image.data() + off1, kMinimalJpeg + third, third);
    memcpy(image.data() + off2, kMinimalJpeg + 2 * third, jpegLen - 2 * third);

    const std::wstring imagePath = WriteTempImage(image);
    CHECK_MSG(!imagePath.empty(), "temp image");

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");

    Partition part;
    part.startSector = 0;
    part.totalSectors = imageSize / 512;
    part.sizeBytes = imageSize;
    part.fsType = L"RAW";

    std::vector<FileNode> candidates;
    candidates.push_back(MakeRawNode(off1, third, L"RECOV_0000000000002000.jpg", L"jpg"));
    candidates.push_back(MakeRawNode(off2, jpegLen - 2 * third, L"RECOV_0000000000003000.jpg", L"jpg"));
    candidates.push_back(MakeRawNode(off0, third, L"RECOV_0000000000001000.jpg", L"jpg"));
    candidates.push_back(MakeRawNode(14000, 200, L"RECOV_00000000000036B0.pdf", L"pdf"));

    FragmentStitchPolicy policy;
    policy.enabled = true;
    policy.graphMergeCarvedSpans = true;

    const uint32_t merged =
        CarveGraphMergeService::MergeRawCarveCandidates(reader, part, candidates, policy);
    CHECK_MSG(merged == 1, "one jpg merge");
    CHECK_MSG(candidates.size() == 2, "pdf + merged jpg");

    bool foundGraph = false;
    for (const auto& n : candidates) {
        if (n.name.find(L"RECOV_GRAPH_") != std::wstring::npos) {
            foundGraph = true;
            CHECK_MSG(n.rawFragments.size() == 3, "three spans");
            CHECK_MSG(n.size == jpegLen, "merged size");
        }
    }
    CHECK_MSG(foundGraph, "RECOV_GRAPH node");

    reader.Close();
    DeleteFileW(imagePath.c_str());

    if (g_failures == 0) {
        fprintf(stdout, "OK: CarveGraphMergeService\n");
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CarveGraphMerge requires volume I/O platform\n");
    return 0;
}

#endif
