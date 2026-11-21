// SqliteRecoverService — page-level recovery unit tests (no Qt).

#include "repair/SqliteRecoverService.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

namespace {

void WriteBE16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void WriteBE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t ReadBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void BuildSyntheticDb(std::vector<uint8_t>& out, uint32_t pageSize, uint32_t numPages)
{
    out.assign(static_cast<size_t>(pageSize) * numPages, 0);
    static const char kMagic[] = "SQLite format 3";
    memcpy(out.data(), kMagic, 16);
    const uint16_t psField = (pageSize == 65536u) ? 1u : static_cast<uint16_t>(pageSize / 256u);
    WriteBE16(out.data() + 16, psField);
    WriteBE32(out.data() + 28, numPages);
    out[100] = 0x0d; // leaf table (page 1)

    for (uint32_t p = 2; p <= numPages; ++p) {
        const size_t off = static_cast<size_t>(p - 1) * pageSize;
        out[off] = 0x0a; // leaf table
    }
}

} // namespace

int main()
{
    using SqliteRecoverService::IsSqliteMagic;
    using SqliteRecoverService::ReadPageSizeFromHeader;
    using SqliteRecoverService::RecoverDatabase;
    using SqliteRecoverService::RecoverOptions;

    CHECK(IsSqliteMagic(reinterpret_cast<const uint8_t*>("SQLite format 3\0"), 16));
    CHECK(!IsSqliteMagic(reinterpret_cast<const uint8_t*>("not a db"), 16));

    std::vector<uint8_t> db;
    BuildSyntheticDb(db, 512, 3);
    CHECK(ReadPageSizeFromHeader(db.data()) == 512);

    std::vector<uint8_t> out;
    auto report = RecoverDatabase(db.data(), db.size(), out, RecoverOptions{});
    CHECK(report.success);
    CHECK(report.pagesCopied == 3);
    CHECK(out.size() == 512u * 3u);
    CHECK(ReadBE32(out.data() + 28) == 3u);

    // Truncate tail: corrupt page 3
    db[2 * 512] = 0x00;
    out.clear();
    report = RecoverDatabase(db.data(), db.size(), out, RecoverOptions{});
    CHECK(report.success);
    CHECK(report.pagesCopied == 2);
    CHECK(out.size() == 512u * 2u);

    // Carved offset: 1 KiB leading junk
    std::vector<uint8_t> db3;
    BuildSyntheticDb(db3, 512, 3);
    std::vector<uint8_t> carved(1024 + db3.size(), 0xFF);
    memcpy(carved.data() + 1024, db3.data(), db3.size());
    out.clear();
    report = RecoverDatabase(carved.data(), carved.size(), out, RecoverOptions{});
    CHECK(report.success);
    CHECK(report.sourceOffset == 1024u);
    CHECK(report.pagesCopied == 3);

    if (g_failures == 0) {
        fprintf(stdout, "OK: SqliteRecoverService unit tests\n");
    }
    return g_failures ? 1 : 0;
}
