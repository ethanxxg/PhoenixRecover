// Graph carve fragment stitch — shuffled JPEG spans on a synthetic image.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "recover/CarveFragmentBlock.h"
#include "recover/FileNode.h"
#include "recover/FragmentStitchPolicy.h"
#include "recover/FragmentStitchService.h"
#include "recover/FormatPayloadScore.h"
#include "recover/RecoverEngine.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
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

// Minimal valid JPEG (SOI … EOI), 119 bytes.
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
        std::wstring(tmp) + L"PhoenixRecover_graph_carve_" + std::to_wstring(GetTickCount64()) + L".img";
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

static FormatPayloadScore ScoreRecoveredFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return FormatPayloadScore::Invalid;
    }
    std::vector<uint8_t> buf(256 * 1024);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) {
        CloseHandle(h);
        return FormatPayloadScore::Invalid;
    }
    CloseHandle(h);
    return FormatPayloadScoreUtil::Score(L"jpg", buf.data(), static_cast<size_t>(got));
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

    // Shuffled: 2, 0, 1 (wrong order)
    std::vector<CarveFragmentBlock> blocks = {
        { off1, third },
        { off2, jpegLen - 2 * third },
        { off0, third },
    };

    FragmentStitchPolicy policy;
    policy.enabled = true;
    policy.strategy = FragmentStitchStrategy::GraphCarve;

    FileNode node;
    CHECK_MSG(FragmentStitchService::TryGraphStitchCarveBlocks(
                  reader, part, L"jpg", blocks, node, policy),
              "graph stitch");

    CHECK_MSG(node.rawFragments.size() == 3, "three spans");
    CHECK_MSG(node.size == jpegLen, "total size");
    CHECK_MSG(node.rawFragments[0].diskOffset == off0, "must start at header span");
    CHECK_MSG(node.rawFragments[0].size + node.rawFragments[1].size + node.rawFragments[2].size ==
                  jpegLen,
              "span sizes sum");

    wchar_t outDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, outDir);
    const std::wstring outRoot =
        std::wstring(outDir) + L"PhoenixRecover_graph_out_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(outRoot.c_str(), nullptr);

    node.name = L"stitched.jpg";
    node.extension = L"jpg";
    node.fsType = L"RAW";
    node.rawSize = node.size;

    RecoverEngine engine;
    RecoverOptions opts;
    opts.fragmentStitch = policy;
    engine.SetOptions(opts);

    const FileRecoverResult rr = engine.RecoverFile(node, outRoot, false, reader, part);
    CHECK_MSG(rr.status == RecoverStatus::Success, "RecoverFile success");

    const std::wstring outPath = rr.targetPath.empty() ? outRoot + L"\\stitched.jpg" : rr.targetPath;
    const FormatPayloadScore sc = ScoreRecoveredFile(outPath);
    fprintf(stdout, "Recovered JPEG score: %d\n", static_cast<int>(sc));
    CHECK_MSG(sc == FormatPayloadScore::Complete || sc == FormatPayloadScore::Partial,
              "jpeg structure score");

    reader.Close();
    DeleteFileW(imagePath.c_str());
    DeleteFileW(outPath.c_str());
    RemoveDirectoryW(outRoot.c_str());

    if (g_failures == 0) {
        fprintf(stdout, "OK: GraphCarveFragmentStitch\n");
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: GraphCarveFragmentStitch requires volume I/O platform\n");
    return 0;
}

#endif
