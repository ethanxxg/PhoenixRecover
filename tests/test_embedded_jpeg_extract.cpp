#include "scan/EmbeddedJpegExtract.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static std::vector<uint8_t> MakeMinimalJpeg()
{
    return {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xFF, 0xD9,
    };
}

static void AppendLe16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void AppendLe32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static std::vector<uint8_t> MakeStoredZipWithJpeg(const char* entryName,
                                                  const std::vector<uint8_t>& jpeg)
{
    std::vector<uint8_t> zip;
    zip.insert(zip.end(), {'P', 'K', 0x03, 0x04});
    AppendLe16(zip, 20); // version
    AppendLe16(zip, 0);  // flags
    AppendLe16(zip, 0);  // stored
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe32(zip, static_cast<uint32_t>(jpeg.size()));
    AppendLe32(zip, static_cast<uint32_t>(jpeg.size()));
    const uint16_t nameLen = static_cast<uint16_t>(strlen(entryName));
    AppendLe16(zip, nameLen);
    AppendLe16(zip, 0);
    zip.insert(zip.end(), entryName, entryName + nameLen);
    zip.insert(zip.end(), jpeg.begin(), jpeg.end());

    const uint32_t cdOff = static_cast<uint32_t>(zip.size());
    zip.insert(zip.end(), {'P', 'K', 0x01, 0x02});
    AppendLe16(zip, 20);
    AppendLe16(zip, 20);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe32(zip, static_cast<uint32_t>(jpeg.size()));
    AppendLe32(zip, static_cast<uint32_t>(jpeg.size()));
    AppendLe16(zip, nameLen);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe32(zip, 0);
    AppendLe32(zip, 0);
    zip.insert(zip.end(), entryName, entryName + nameLen);

    const uint32_t cdSize = static_cast<uint32_t>(zip.size() - cdOff);
    zip.insert(zip.end(), {'P', 'K', 0x05, 0x06});
    AppendLe16(zip, 0);
    AppendLe16(zip, 0);
    AppendLe16(zip, 1);
    AppendLe16(zip, 1);
    AppendLe32(zip, cdSize);
    AppendLe32(zip, cdOff);
    AppendLe16(zip, 0);
    return zip;
}

int main()
{
    const std::vector<uint8_t> jpeg = MakeMinimalJpeg();

    {
        std::vector<EmbeddedJpegSlice> slices;
        EmbeddedJpegExtract::CollectFromBuffer(jpeg.data(), jpeg.size(), slices);
        CHECK(slices.size() == 1);
        CHECK(slices[0].bytes.size() == jpeg.size());
    }

    {
        const std::vector<uint8_t> zip = MakeStoredZipWithJpeg("file8.jpg", jpeg);
        std::vector<EmbeddedJpegSlice> slices;
        EmbeddedJpegExtract::CollectFromBuffer(zip.data(), zip.size(), slices);
        CHECK(!slices.empty());
        bool found = false;
        for (const EmbeddedJpegSlice& s : slices) {
            if (s.bytes.size() == jpeg.size() &&
                memcmp(s.bytes.data(), jpeg.data(), jpeg.size()) == 0)
                found = true;
        }
        CHECK(found);
    }

    {
        std::vector<uint8_t> dat = {'p', 'r', 'e', 'f', 'i', 'x'};
        dat.insert(dat.end(), jpeg.begin(), jpeg.end());
        std::vector<EmbeddedJpegSlice> slices;
        EmbeddedJpegExtract::CollectFromBuffer(dat.data(), dat.size(), slices);
        CHECK(slices.size() == 1);
        CHECK(slices[0].offsetInHost == 6);
    }

    if (g_failures != 0) {
        fprintf(stderr, "embedded_jpeg_extract: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: embedded_jpeg_extract unit checks\n");
    return 0;
}
