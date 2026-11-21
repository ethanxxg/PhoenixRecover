// SignatureDB 签名匹配 单元测试
// 依赖 Qt（QFile, QJsonDocument），仅在 PHOENIX_BUILD_GUI=ON 时编译。
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "scan/SignatureDB.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) return 1; \
    } while (0)

int main()
{
    SignatureDB db;

    const auto& all = db.GetAll();
    CHECK(!all.empty());

    // ── JPEG: FF D8 FF ──────────────────────────────────────────────────────
    {
        uint8_t jpeg[16] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10,
                             0x4A, 0x46, 0x49, 0x46, 0x00, 0x01 };
        const FileSignature* sig = db.Match(jpeg, sizeof(jpeg));
        CHECK(sig != nullptr);
        CHECK(sig->extension == L"jpg" || sig->extension == L"jpeg");
    }

    // ── PNG: 89 50 4E 47 0D 0A 1A 0A ────────────────────────────────────────
    {
        uint8_t png[16] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52 };
        const FileSignature* sig = db.Match(png, sizeof(png));
        CHECK(sig != nullptr);
        CHECK(sig->extension == L"png");
    }

    // ── Unknown data → no match ──────────────────────────────────────────────
    {
        uint8_t noise[16];
        std::memset(noise, 0xAB, sizeof(noise));
        const FileSignature* sig = db.Match(noise, sizeof(noise));
        CHECK(sig == nullptr);
    }

    // ── ZIP: 50 4B 03 04 ─────────────────────────────────────────────────────
    {
        uint8_t zip[8] = { 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00 };
        const FileSignature* sig = db.Match(zip, sizeof(zip));
        CHECK(sig != nullptr);
        CHECK(sig->extension == L"zip" || sig->extension == L"docx" ||
              sig->extension == L"xlsx" || sig->extension == L"pptx" ||
              sig->extension == L"jar" || sig->extension == L"3mf" ||
              sig->extension == L"odg" || sig->extension == L"ott" ||
              sig->extension == L"numbers" || sig->extension == L"key" ||
              sig->extension == L"dotm" || sig->extension == L"dox" ||
              sig->extension == L"thmx" || sig->extension == L"xlf" ||
              sig->extension == L"xmind");
    }

    // ── PDF: 25 50 44 46 ─────────────────────────────────────────────────────
    {
        uint8_t pdf[8] = { 0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x34 };
        const FileSignature* sig = db.Match(pdf, sizeof(pdf));
        CHECK(sig != nullptr);
        CHECK(sig->extension == L"pdf" || sig->extension == L"ai");
    }

    return 0;
}
