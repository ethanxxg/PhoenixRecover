// CarveResult 结构体默认值 单元测试
#include <cstdint>
#include <cstdlib>

#include "scan/carve/CarveCommon.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) return 1; \
    } while (0)

int main()
{
    // ── Default-constructed CarveResult ───────────────────────────────────────
    {
        CarveResult cr;
        CHECK(cr.ok == false);
        CHECK(cr.endOffset == 0);
        CHECK(cr.extOverride.empty());
        CHECK(cr.confidence == 0);
        CHECK(cr.reason.empty());
    }

    // ── Manual field assignment round-trip ────────────────────────────────────
    {
        CarveResult cr;
        cr.ok          = true;
        cr.endOffset   = 0x12345678ULL;
        cr.extOverride = L"mp4";
        cr.confidence  = 85;
        cr.reason      = L"ftyp box valid";

        CHECK(cr.ok == true);
        CHECK(cr.endOffset == 0x12345678ULL);
        CHECK(cr.extOverride == L"mp4");
        CHECK(cr.confidence == 85);
        CHECK(cr.reason == L"ftyp box valid");
    }

    // ── Copy semantics ───────────────────────────────────────────────────────
    {
        CarveResult a;
        a.ok         = true;
        a.endOffset  = 4096;
        a.confidence = 50;
        a.extOverride = L"zip";
        a.reason      = L"EOCD found";

        CarveResult b = a;
        CHECK(b.ok          == a.ok);
        CHECK(b.endOffset   == a.endOffset);
        CHECK(b.confidence  == a.confidence);
        CHECK(b.extOverride == a.extOverride);
        CHECK(b.reason      == a.reason);
    }

    return 0;
}
