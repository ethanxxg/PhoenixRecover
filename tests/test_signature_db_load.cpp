// A1-9 / task #59 — Qt-free custom signature loading (headless build).
//
// The old SignatureDB::LoadFromFile() was gated behind PHOENIX_BUILD_GUI and
// returned -1 unconditionally in non-GUI builds because it depended on
// QFile/QJsonDocument.  This test runs with PHOENIX_BUILD_GUI=0 and proves the
// replacement (std::ifstream + SignatureJson.h) works identically:
//
//   1. SignatureJson.h parser: RFC 8259 subset properties + strict rejection.
//   2. LoadFromFile() succeeds in a headless build and appends signatures.
//   3. Appended entries carry exactly the SIGNATURE_FORMAT.md fields.
//   4. Match()/ScanBatch() see the NEW signatures (AC matcher rebuilt lazily).
//   5. Negative: missing file / bad JSON / wrong root / missing array /
//      malformed entries skipped / invalid hex rejected.
//   6. The real testdata/custom_signatures.json sample loads as documented.
//
// Build: no Qt, no device I/O. PHOENIX_BUILD_GUI=0.

#include "scan/SignatureDB.h"
#include "scan/SignatureJson.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int  g_failures = 0;
int  g_checks   = 0;

void Check(bool ok, const char* msg)
{
    ++g_checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; }
    else     { std::fprintf(stdout, "ok:   %s\n", msg); }
}

bool WriteFile(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

std::wstring W(const std::string& s)
{
    return std::wstring(s.begin(), s.end());   // test paths are ASCII
}

bool BytesEq(const std::vector<uint8_t>& v, std::initializer_list<int> expect)
{
    if (v.size() != expect.size()) return false;
    size_t i = 0;
    for (int e : expect)
        if (v[i++] != static_cast<uint8_t>(e)) return false;
    return true;
}

// ── 1. Parser properties ───────────────────────────────────────────────────
void TestJsonParser()
{
    using phoenix::JsonValue;
    using phoenix::JsonParser;

    auto parse = [](const std::string& s, JsonValue& out) -> bool {
        JsonParser p(s);
        return p.Parse(out);
    };
    auto reject = [&](const std::string& s, const char* label) {
        JsonValue v;
        bool ok = parse(s, v);
        Check(!ok, label);
    };

    JsonValue v;
    // Complex document: nested array/object, all literals, number forms,
    // string escapes, BMP \u and a surrogate pair (U+1F600).
    const std::string doc =
        R"({"a":1,"b":-2.5e2,"c":true,"d":false,"e":null,)"
        R"("f":[1,"x",{},[]],"g":"A\n\u0041\u00e9\ud83d\ude00","h":0})";
    Check(parse(doc, v), "parser: complex document accepted");
    Check(v.IsObject() && v.object.size() == 8, "parser: 8 top-level members");
    Check(v.Find("a") && v.Find("a")->AsNumber(-1) == 1.0,
          "parser: positive integer");
    Check(v.Find("b") && v.Find("b")->AsNumber(0) == -250.0,
          "parser: negative fraction + exponent");
    Check(v.Find("c") && v.Find("c")->type == JsonValue::Type::Bool &&
          v.Find("c")->boolean, "parser: true");
    Check(v.Find("d") && v.Find("d")->type == JsonValue::Type::Bool &&
          !v.Find("d")->boolean, "parser: false");
    Check(v.Find("e") && v.Find("e")->type == JsonValue::Type::Null,
          "parser: null");
    Check(v.Find("h") && v.Find("h")->AsNumber(-1) == 0.0,
          "parser: bare zero");
    Check(v.Find("f") && v.Find("f")->IsArray() &&
          v.Find("f")->array.size() == 4, "parser: nested array of 4");
    Check(v.Find("f") && v.Find("f")->array[2].IsObject() &&
          v.Find("f")->array[2].object.empty(), "parser: empty nested object");
    Check(v.Find("f") && v.Find("f")->array[3].IsArray() &&
          v.Find("f")->array[3].array.empty(), "parser: empty nested array");
    // "A" + \n + \u0041('A') + \u00e9(C3 A9) + U+1F600(F0 9F 98 80) = 9 bytes.
    Check(v.Find("g") &&
          v.Find("g")->AsString() ==
              std::string("A\nA\xC3\xA9\xF0\x9F\x98\x80", 9),
          "parser: escapes + BMP \\u + surrogate pair -> UTF-8");
    Check(v.Find("missing") == nullptr, "parser: absent key returns nullptr");

    // Strict rejection.
    reject("", "parser rejects: empty input");
    reject("   ", "parser rejects: whitespace only");
    reject("{\"a\":1,}", "parser rejects: trailing comma in object");
    reject("[1,]", "parser rejects: trailing comma in array");
    reject("{a:1}", "parser rejects: unquoted key");
    reject("{\"a\" 1}", "parser rejects: missing colon");
    reject("\"unterminated", "parser rejects: unterminated string");
    reject("\"\\q\"", "parser rejects: unknown escape");
    reject("\"\\uD800\"", "parser rejects: lone high surrogate");
    reject("\"\\uDC00\"", "parser rejects: lone low surrogate");
    reject("\"raw\ncontrol\"", "parser rejects: unescaped control char");
    reject("01", "parser rejects: leading zero");
    reject("1e", "parser rejects: malformed exponent");
    reject("1.", "parser rejects: malformed fraction");
    reject("{\"a\":1} garbage", "parser rejects: trailing garbage");
    reject("{\"a\":1", "parser rejects: unterminated object");
    reject("[1", "parser rejects: unterminated array");
    reject("{\"a\":tru}", "parser rejects: invalid literal");
    {
        std::string deep;
        for (int i = 0; i < 80; ++i) deep += '[';
        for (int i = 0; i < 80; ++i) deep += ']';
        reject(deep, "parser rejects: nesting beyond depth limit");
    }
    {
        JsonParser p("{\"a\":@}");
        Check(!p.Parse(v) && !p.Error().empty() && p.ErrorPos() == 5,
              "parser: error message + byte offset reported");
    }
}

// ── 2..4. LoadFromFile positive + AC rebuild ───────────────────────────────
void TestLoadPositive()
{
    const char* path = "sigdb_load_positive.json";
    // Unique magic ("PHOENIXTEST") that cannot collide with the built-in set.
    const std::string json = R"({
  "signatures": [
    {
      "extension": "xphx",
      "category": "Custom",
      "description": "Custom \u0046ormat\nwith escapes",
      "magic": "50484F454E495854455354",
      "magic_offset": 0,
      "footer": "454E44",
      "max_size": 10485760,
      "secondary_magic": "",
      "secondary_offset": -1
    },
    {
      "extension": "xyz",
      "magic": "58595A00",
      "magic_offset": 2,
      "footer": "454E44",
      "max_size": 10485760,
      "secondary_magic": "0102",
      "secondary_offset": 6
    },
    {
      "extension": "  low  ",
      "magic": "  abcd  ",
      "max_size": 4096
    },
    {
      "extension": "dflt",
      "magic": "DEADBEEF"
    }
  ]
})";
    Check(WriteFile(path, json), "positive: fixture written");

    static const uint8_t kMagic[] = {
        'P','H','O','E','N','I','X','T','E','S','T'
    };

    SignatureDB db;
    const size_t base = db.GetAll().size();
    Check(base > 0, "positive: built-in signature set is non-empty");

    // Force the AC matcher to be built from the ORIGINAL set first.  Without the
    // m_matcherBuilt=false invalidation in LoadFromFile(), the cached matcher
    // would stay stale and the new magic would never be found below.
    uint8_t probe[64] = {};
    std::memcpy(probe, kMagic, sizeof(kMagic));
    const FileSignature* before = db.Match(probe, sizeof(probe));
    Check(before == nullptr || before->extension != L"xphx",
          "positive: unique magic absent from built-in set");

    const int n = db.LoadFromFile(W(path));
    Check(n == 4, "positive: LoadFromFile returns 4 in a PHOENIX_BUILD_GUI=0 build");
    Check(db.GetAll().size() == base + 4,
          "positive: exactly 4 signatures appended");

    const auto& all = db.GetAll();
    // Entry 1: xphx, full fields.
    const FileSignature& a = all[base];
    Check(a.extension == L"xphx", "fields[0]: extension");
    Check(BytesEq(a.header, {0x50,0x48,0x4F,0x45,0x4E,0x49,0x58,0x54,0x45,0x53,0x54}),
          "fields[0]: header bytes decoded from hex");
    Check(a.headerOffset == 0, "fields[0]: magic_offset");
    Check(BytesEq(a.footer, {0x45,0x4E,0x44}), "fields[0]: footer bytes");
    Check(a.maxSize == 10485760u, "fields[0]: max_size");
    Check(a.secondary.empty() && a.secondaryOffset == -1,
          "fields[0]: no secondary");

    // Entry 2: xyz, non-zero offsets + secondary.
    const FileSignature& b = all[base + 1];
    Check(b.extension == L"xyz", "fields[1]: extension");
    Check(BytesEq(b.header, {0x58,0x59,0x5A,0x00}), "fields[1]: header bytes");
    Check(b.headerOffset == 2, "fields[1]: magic_offset = 2");
    Check(BytesEq(b.footer, {0x45,0x4E,0x44}), "fields[1]: footer bytes");
    Check(b.maxSize == 10485760u, "fields[1]: max_size");
    Check(BytesEq(b.secondary, {0x01,0x02}) && b.secondaryOffset == 6,
          "fields[1]: secondary magic + offset");

    // Entry 3: whitespace-trimmed extension/magic, lowercase hex.
    const FileSignature& c = all[base + 2];
    Check(c.extension == L"low", "fields[2]: extension trimmed");
    Check(BytesEq(c.header, {0xAB,0xCD}),
          "fields[2]: lowercase hex + trimmed magic");
    Check(c.maxSize == 4096u, "fields[2]: max_size");
    Check(c.headerOffset == 0 && c.footer.empty() && c.secondary.empty(),
          "fields[2]: defaults applied");

    // Entry 4: default max_size.
    const FileSignature& d = all[base + 3];
    Check(d.extension == L"dflt", "fields[3]: extension");
    Check(BytesEq(d.header, {0xDE,0xAD,0xBE,0xEF}), "fields[3]: header bytes");
    Check(d.maxSize == 50u * 1024u * 1024u,
          "fields[3]: default max_size 50 MiB");

    // AC matcher must have been rebuilt with the enlarged signature set.
    const FileSignature* after = db.Match(probe, sizeof(probe));
    Check(after != nullptr && after->extension == L"xphx",
          "Match() sees the loaded signature (AC matcher rebuilt lazily)");

    std::vector<uint8_t> buf(1024, 0);
    std::memcpy(buf.data() + 512, kMagic, sizeof(kMagic));
    std::vector<SignatureDB::ScanCandidate> cands;
    db.ScanBatch(buf.data(), buf.size(), 0x10000ull, 512, cands);
    bool found = false;
    for (const auto& cand : cands)
        if (cand.fileStart == 0x10000ull + 512 && cand.sig &&
            cand.sig->extension == L"xphx") { found = true; break; }
    Check(found, "ScanBatch emits the loaded signature at the expected disk offset");

    std::remove(path);
}

// ── 5. LoadFromFile negatives ──────────────────────────────────────────────
void TestLoadNegative()
{
    struct Case { const char* path; const char* json; const char* label; };
    const Case cases[] = {
        { "sigdb_bad_syntax.json",   "{\"signatures\": [",                       "malformed JSON" },
        { "sigdb_bad_root.json",     "[1,2,3]",                                 "root is not an object" },
        { "sigdb_no_array.json",     "{\"other\": 1}",                          "missing signatures array" },
        { "sigdb_wrong_array.json",  "{\"signatures\": {}}",                    "signatures is not an array" },
    };
    for (const auto& c : cases) {
        Check(WriteFile(c.path, c.json), c.label);
        SignatureDB db;
        Check(db.LoadFromFile(W(c.path)) == -1, c.label);
        std::remove(c.path);
    }

    SignatureDB db;
    Check(db.LoadFromFile(W("sigdb_does_not_exist_9f3a.json")) == -1,
          "missing file returns -1");

    // Mixed valid / invalid entries: bad ones skipped, good ones appended.
    const char* path = "sigdb_mixed.json";
    const std::string mixed = R"({"signatures": [
        42,
        {"extension":"n1","magic":"41424344"},
        {"magic":"41424344"},
        {"extension":"n2"},
        {"extension":"n3","magic":"41424"},
        {"extension":"n4","magic":"4142ZZ44"},
        {"extension":"n5","magic":"41424344","footer":"ZZ"},
        {"extension":"n6","magic":"41424344","secondary_magic":"0G"},
        {"extension":"n7","magic":"41424344","max_size":-5},
        {"extension":"n8","magic":"41424344"}
    ]})";
    Check(WriteFile(path, mixed), "mixed: fixture written");
    const size_t base = db.GetAll().size();
    const int n = db.LoadFromFile(W(path));
    Check(n == 3, "mixed: 3 valid entries loaded, 7 invalid skipped");
    Check(db.GetAll().size() == base + 3, "mixed: DB grew by exactly 3");
    if (n == 3) {
        const auto& all = db.GetAll();
        Check(all[base].extension == L"n1", "mixed: first valid entry appended");
        Check(all[base + 2].extension == L"n8", "mixed: last valid entry appended");
        // Non-positive max_size must fall back to the documented default.
        Check(all[base + 1].extension == L"n7" &&
              all[base + 1].maxSize == 50u * 1024u * 1024u,
              "mixed: non-positive max_size guarded back to 50 MiB default");
    }
    std::remove(path);

    // UTF-8 BOM + escaped extension still accepted.
    const char* bomPath = "sigdb_bom.json";
    std::string bom;
    bom.push_back(static_cast<char>(0xEF));
    bom.push_back(static_cast<char>(0xBB));
    bom.push_back(static_cast<char>(0xBF));
    bom += R"({"signatures":[{"extension":"xph\u0079","magic":"41424344"}]})";
    Check(WriteFile(bomPath, bom), "BOM: fixture written");
    SignatureDB db2;
    const int m = db2.LoadFromFile(W(bomPath));
    Check(m == 1, "BOM: UTF-8 BOM stripped, file loads");
    Check(m == 1 && db2.GetAll().back().extension == L"xphy",
          "BOM: \\u escape in extension decoded");
    std::remove(bomPath);
}

// ── 6. Real documented sample ──────────────────────────────────────────────
void TestRealSample()
{
#ifdef PHOENIX_SOURCE_DIR
    const std::wstring path =
        std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/custom_signatures.json";
    SignatureDB db;
    const size_t base = db.GetAll().size();
    const int n = db.LoadFromFile(path);
    Check(n == 3, "sample: testdata/custom_signatures.json loads 3 signatures");
    if (n != 3) return;
    Check(db.GetAll().size() == base + 3, "sample: 3 entries appended");
    const auto& all = db.GetAll();

    Check(all[base].extension == L"sqlite", "sample[0]: sqlite extension");
    Check(BytesEq(all[base].header,
                  {0x53,0x51,0x4C,0x69,0x74,0x65,0x20,0x66,0x6F,0x72,0x6D,0x61,0x74,0x20,0x33,0x00}),
          "sample[0]: SQLite magic 'SQLite format 3\\0'");
    Check(all[base].maxSize == 4294967296ull, "sample[0]: max_size 4 GiB");
    Check(all[base].footer.empty(), "sample[0]: no footer");

    Check(all[base + 1].extension == L"pdf", "sample[1]: pdf extension");
    Check(BytesEq(all[base + 1].header, {0x25,0x50,0x44,0x46}),
          "sample[1]: %PDF magic");
    Check(BytesEq(all[base + 1].footer, {0x25,0x25,0x45,0x4F,0x46}),
          "sample[1]: %%EOF footer");
    Check(all[base + 1].maxSize == 524288000ull, "sample[1]: max_size");

    Check(all[base + 2].extension == L"wav", "sample[2]: wav extension");
    Check(BytesEq(all[base + 2].header, {0x52,0x49,0x46,0x46}),
          "sample[2]: RIFF magic");
    Check(BytesEq(all[base + 2].secondary, {0x57,0x41,0x56,0x45}) &&
          all[base + 2].secondaryOffset == 8,
          "sample[2]: WAVE secondary at offset 8");
#else
    Check(false, "sample: PHOENIX_SOURCE_DIR must be defined");
#endif
}

} // namespace

int main()
{
    TestJsonParser();
    TestLoadPositive();
    TestLoadNegative();
    TestRealSample();

    std::fprintf(stdout, "\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0)
        std::fprintf(stdout, "ALL SIGNATUREDB-LOAD CHECKS PASSED\n");
    else
        std::fprintf(stderr, "%d SIGNATUREDB-LOAD CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
