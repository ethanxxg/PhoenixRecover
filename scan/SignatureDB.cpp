#include "SignatureDB.h"
#include "SignatureJson.h"
#include "UiTypeSignatureSupplement.h"
#include "PhotoRecSignatures.h"
#include "../log/Logger.h"
#include "../common/Utils.h"
#include <fstream>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <cstring>

// A1-9: no Qt headers here.  LoadFromFile() used to be gated behind
// PHOENIX_BUILD_GUI and returned -1 in headless builds because it depended on
// QFile/QJsonDocument.  It now uses std::ifstream + the zero-dependency
// SignatureJson.h parser, so GUI and non-GUI builds behave identically.

// ── Helper: check one pattern at a given offset ──────────────────────────
bool SignatureDB::MatchPattern(const uint8_t* buf, size_t bufLen,
                               const std::vector<uint8_t>& pattern, int offset)
{
    if (pattern.empty()) return true;
    if (offset < 0) return true;
    size_t off = static_cast<size_t>(offset);
    if (off + pattern.size() > bufLen) return false;
    return memcmp(buf + off, pattern.data(), pattern.size()) == 0;
}

// ── Exact-signature-set constructor (A1-10) ───────────────────────────────
// Deliberately does NOT populate the built-ins.  All matcher state is left at
// its default (m_matcherBuilt == false), so EnsureMatcher() builds the trie
// from exactly this set on first use — mirroring the serial path's DB.
SignatureDB::SignatureDB(const std::vector<FileSignature>& sigs)
    : m_sigs(sigs)
{
}

// ── Built-in signatures ───────────────────────────────────────────────────
// PhotoRec reference: https://www.cgsecurity.org/wiki/File_Formats_Recovered_By_PhotoRec
// We cover all major formats (200+) with precise magic bytes.
// ─────────────────────────────────────────────────────────────────────────
SignatureDB::SignatureDB()
{
    // Builder helpers
    auto mk = [](const wchar_t* ext,
                 const char*    hdrHex,
                 const char*    ftrHex    = nullptr,
                 int            hdrOff    = 0,
                 uint64_t       maxMB     = 50,
                 const char*    sec2Hex   = nullptr,
                 int            sec2Off   = -1) -> FileSignature
    {
        FileSignature s;
        s.extension      = ext;
        s.header         = HexToBytes(hdrHex);
        s.headerOffset   = hdrOff;
        if (ftrHex)  s.footer          = HexToBytes(ftrHex);
        if (sec2Hex) s.secondary        = HexToBytes(sec2Hex);
        s.secondaryOffset = sec2Off;
        s.maxSize         = maxMB * 1024 * 1024;
        return s;
    };

    // RIFF sub-type helper: checks "RIFF" at 0 + 4cc at offset 8
    auto riff = [&mk](const wchar_t* ext, const char* fmt4cc,
                      uint64_t maxMB = 500) -> FileSignature
    {
        // Primary: "RIFF" at offset 0; Secondary: specific 4CC at offset 8
        return mk(ext, "52494646", nullptr, 0, maxMB, fmt4cc, 8);
    };

    // ftyp-based container helper (MP4/MOV/3GP etc.): "ftyp" at offset 4
    // brand is optional extra 4 bytes at offset 8
    auto ftyp = [&mk](const wchar_t* ext, const char* brand4cc,
                      uint64_t maxMB = 4096) -> FileSignature
    {
        return mk(ext, "66747970", nullptr, 4, maxMB, brand4cc, 8);
    };

    // CFB (Compound File Binary = OLE2): used by DOC/XLS/PPT/MSG/MDB etc.
    const char* kCFB = "D0CF11E0A1B11AE1";

    // ═══════════════════════════════════════════════════════════════════════
    // ── IMAGES ─────────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // JPEG / JFIF / EXIF
    m_sigs.push_back(mk(L"jpg",  "FFD8FFE0",        "FFD9",           0,  50));
    m_sigs.push_back(mk(L"jpg",  "FFD8FFE1",        "FFD9",           0,  50)); // EXIF
    m_sigs.push_back(mk(L"jpg",  "FFD8FFE2",        "FFD9",           0,  50));
    m_sigs.push_back(mk(L"jpg",  "FFD8FFE8",        "FFD9",           0,  50));
    m_sigs.push_back(mk(L"jpg",  "FFD8FFDB",        "FFD9",           0,  50)); // no APP
    m_sigs.push_back(mk(L"jpg",  "FFD8FFF0",        "FFD9",           0,  50)); // iPhone

    // JPEG 2000
    m_sigs.push_back(mk(L"jp2",  "0000000C6A502020","",               0,  50)); // JP2 box
    m_sigs.push_back(mk(L"j2k",  "FF4FFF51",        "FFD9",           0,  50)); // raw codestream

    // PNG
    m_sigs.push_back(mk(L"png",  "89504E470D0A1A0A","49454E44AE426082",0, 50));

    // GIF
    m_sigs.push_back(mk(L"gif",  "474946383961",    "003B",           0,  10));
    m_sigs.push_back(mk(L"gif",  "474946383761",    "003B",           0,  10));

    // BMP
    m_sigs.push_back(mk(L"bmp",  "424D",            nullptr,          0,  50));

    // TIFF (little-endian and big-endian)
    m_sigs.push_back(mk(L"tif",  "49492A00",        nullptr,          0, 200));
    m_sigs.push_back(mk(L"tif",  "4D4D002A",        nullptr,          0, 200));

    // WebP: RIFF....WEBP
    m_sigs.push_back(riff(L"webp","57454250",        20));

    // ICO / CUR
    m_sigs.push_back(mk(L"ico",  "00000100",        nullptr,          0,   1));
    m_sigs.push_back(mk(L"cur",  "00000200",        nullptr,          0,   1));

    // PSD / PSB (Photoshop)
    m_sigs.push_back(mk(L"psd",  "38425053",        nullptr,          0, 500));

    // XCF (GIMP native)
    m_sigs.push_back(mk(L"xcf",  "67696D702078636620", nullptr,       0, 200));

    // PCX
    m_sigs.push_back(mk(L"pcx",  "0A050108",        nullptr,          0,  10));

    // PNM family
    m_sigs.push_back(mk(L"pbm",  "5031",            nullptr,          0,  50));  // "P1"
    m_sigs.push_back(mk(L"pgm",  "5035",            nullptr,          0,  50));  // "P5"
    m_sigs.push_back(mk(L"ppm",  "5036",            nullptr,          0,  50));  // "P6"

    // TGA has no standard magic – skip (would cause too many false positives)

    // HEIC / HEIF (ISO base media, ftyp brand heic/heis/mif1)
    m_sigs.push_back(ftyp(L"heic","68656963", 200)); // 'heic'
    m_sigs.push_back(ftyp(L"heic","68656973", 200)); // 'heis'
    m_sigs.push_back(ftyp(L"heif","6D696631", 200)); // 'mif1'

    // AVIF
    m_sigs.push_back(ftyp(L"avif","61766966", 200)); // 'avif'

    // ─── Camera RAW formats ───────────────────────────────────────────────

    // Canon CR2 (TIFF LE + "CR\x00\x02" at offset 8)
    m_sigs.push_back(mk(L"cr2", "49492A00", nullptr, 0, 100, "43520002", 8));

    // Canon CR3 (ftyp brand 'crx ')
    m_sigs.push_back(ftyp(L"cr3", "63727820", 200));

    // Nikon NEF (TIFF LE – no unique secondary magic, use general TIFF match above)
    // Sony ARW – same TIFF LE base, distinguish by maker note if needed
    // DNG (Adobe Digital Negative) – TIFF LE
    m_sigs.push_back(mk(L"dng",  "49492A00", nullptr, 0, 200));

    // Olympus ORF
    m_sigs.push_back(mk(L"orf",  "49494F52", nullptr, 0,  50)); // "IIRO"
    m_sigs.push_back(mk(L"orf",  "4D4D4F52", nullptr, 0,  50)); // "MMOR"

    // Panasonic RW2
    m_sigs.push_back(mk(L"rw2",  "49495500", nullptr, 0,  50)); // "IIU\0"

    // Fuji RAF
    m_sigs.push_back(mk(L"raf",  "46554A4946494C4D", nullptr, 0, 50)); // "FUJIFILM"

    // Minolta MRW
    m_sigs.push_back(mk(L"mrw",  "004D524D", nullptr, 0,  50)); // "\0MRM"

    // Sigma X3F
    m_sigs.push_back(mk(L"x3f",  "464F566200000000", nullptr, 0, 50)); // "FOVb"

    // Kodak DCR / KDC (TIFF LE – covered by general TIFF above)

    // ─── Vector / Structured Images ──────────────────────────────────────

    // SVG (XML text)
    m_sigs.push_back(mk(L"svg",  "3C3F786D6C", nullptr, 0, 10)); // "<?xml"
    m_sigs.push_back(mk(L"svg",  "3C737667",   nullptr, 0, 10)); // "<svg"

    // EPS / PostScript (PDF-based Illustrator matches pdf signature below)
    m_sigs.push_back(mk(L"eps",  "25215053",   nullptr, 0, 50));  // "%!PS"
    m_sigs.push_back(mk(L"ps",   "25215053",   nullptr, 0, 50));  // "%!PS"
    m_sigs.push_back(mk(L"eps",  "C5D0D3C6",   nullptr, 0, 50));  // Windows EPS

    // ═══════════════════════════════════════════════════════════════════════
    // ── VIDEO ──────────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // AVI: RIFF....AVI
    m_sigs.push_back(riff(L"avi", "41564920", 4096)); // "AVI "

    // WAV: RIFF....WAVE
    m_sigs.push_back(riff(L"wav", "57415645", 500));  // "WAVE"

    // MPEG-1/2 Program Stream
    m_sigs.push_back(mk(L"mpg",  "000001BA", nullptr, 0, 4096));
    // MPEG Video Elementary Stream
    m_sigs.push_back(mk(L"mpg",  "000001B3", nullptr, 0, 4096));

    // MPEG Transport Stream (TS / MTS / M2TS)
    // TS: 0x47 sync byte every 188 bytes; M2TS: 0x47 every 192 bytes
    m_sigs.push_back(mk(L"ts",   "47400010", nullptr, 0, 4096));
    m_sigs.push_back(mk(L"mts",  "000000004740", nullptr, 0, 4096)); // M2TS 4-byte prefix

    // 3GP / 3G2
    m_sigs.push_back(ftyp(L"3gp",  "33677034", 1024)); // '3gp4'
    m_sigs.push_back(ftyp(L"3gp",  "33677035", 1024)); // '3gp5'
    m_sigs.push_back(ftyp(L"3gp",  "33677036", 1024)); // '3gp6'
    m_sigs.push_back(ftyp(L"3g2",  "33673261", 1024)); // '3g2a'
    m_sigs.push_back(ftyp(L"3g2",  "33673262", 1024)); // '3g2b'

    // MP4 / M4V / M4A (ISO Base Media)
    m_sigs.push_back(ftyp(L"mp4",  "69736F6D", 8192)); // 'isom'
    m_sigs.push_back(ftyp(L"mp4",  "6D703431", 8192)); // 'mp41'
    m_sigs.push_back(ftyp(L"mp4",  "6D703432", 8192)); // 'mp42'
    m_sigs.push_back(ftyp(L"mp4",  "61766331", 8192)); // 'avc1'
    m_sigs.push_back(ftyp(L"m4v",  "4D345620", 8192)); // 'M4V '
    m_sigs.push_back(ftyp(L"m4v",  "66347620", 4096)); // 'f4v '
    m_sigs.push_back(ftyp(L"m4a",  "4D344120", 500));  // 'M4A '
    m_sigs.push_back(ftyp(L"m4a",  "66347620", 500));  // 'f4v '

    // QuickTime MOV
    m_sigs.push_back(mk(L"mov",   "6D6F6F76", nullptr, 4, 8192)); // 'moov' at +4
    m_sigs.push_back(mk(L"mov",   "77696465", nullptr, 4, 8192)); // 'wide' at +4
    m_sigs.push_back(ftyp(L"mov", "71742020", 8192));             // 'qt  '

    // WMV / WMA / ASF (same header)
    m_sigs.push_back(mk(L"wmv",  "3026B2758E66CF11A6D900AA0062CE6C", nullptr, 0, 4096));

    // FLV (Flash Video)
    m_sigs.push_back(mk(L"flv",  "464C5601", nullptr, 0, 4096));

    // MKV / WebM (EBML)
    m_sigs.push_back(mk(L"mkv",  "1A45DFA3", nullptr, 0, 8192));
    m_sigs.push_back(mk(L"webm", "1A45DFA3", nullptr, 0, 8192)); // WebM is a subset of MKV

    // Real Media
    m_sigs.push_back(mk(L"rm",   "2E524D46", nullptr, 0, 2048)); // ".RMF"
    m_sigs.push_back(mk(L"rv",   "2E524D46", nullptr, 0, 2048)); // video variant

    // VOB (DVD Video Object, MPEG-PS)
    // same magic as MPG (000001BA) – covered above

    // DivX / DIVX – AVI-based, covered by AVI

    // OGG Video
    m_sigs.push_back(mk(L"ogv",  "4F676753", nullptr, 0, 2048)); // "OggS" – reuse OGG

    // F4V (Adobe Flash MP4)
    m_sigs.push_back(ftyp(L"f4v", "66347620", 4096));

    // SWF (Flash)
    m_sigs.push_back(mk(L"swf",  "465753",   nullptr, 0, 100)); // "FWS" uncompressed
    m_sigs.push_back(mk(L"swf",  "435753",   nullptr, 0, 100)); // "CWS" zlib compressed
    m_sigs.push_back(mk(L"swf",  "5A5753",   nullptr, 0, 100)); // "ZWS" LZMA compressed

    // ═══════════════════════════════════════════════════════════════════════
    // ── AUDIO ──────────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // MP3
    m_sigs.push_back(mk(L"mp3",  "494433",   nullptr, 0,  100)); // ID3 tag
    m_sigs.push_back(mk(L"mp3",  "FFFB",     nullptr, 0,  100)); // MP3 frame sync
    m_sigs.push_back(mk(L"mp3",  "FFFA",     nullptr, 0,  100)); // MP3 mono

    // FLAC
    m_sigs.push_back(mk(L"flac", "664C6143", nullptr, 0,  500)); // "fLaC"

    // OGG (Vorbis / Opus / Speex / FLAC in OGG)
    m_sigs.push_back(mk(L"ogg",  "4F676753", nullptr, 0,  200)); // "OggS"

    // AAC (ADTS)
    m_sigs.push_back(mk(L"aac",  "FFF1",     nullptr, 0,  100));
    m_sigs.push_back(mk(L"aac",  "FFF9",     nullptr, 0,  100));

    // AIFF / AIFF-C
    m_sigs.push_back(mk(L"aif",  "464F524D", nullptr, 0,  200)); // "FORM" (IFF)

    // MIDI
    m_sigs.push_back(mk(L"mid",  "4D546864", nullptr, 0,   10)); // "MThd"

    // Monkey's Audio (APE)
    m_sigs.push_back(mk(L"ape",  "4D414320", nullptr, 0,  500)); // "MAC "

    // WavPack
    m_sigs.push_back(mk(L"wv",   "7776706B", nullptr, 0,  500)); // "wvpk"

    // Musepack
    m_sigs.push_back(mk(L"mpc",  "4D502B07", nullptr, 0,  200)); // "MP+\x07"
    m_sigs.push_back(mk(L"mpc",  "4D50434B", nullptr, 0,  200)); // "MPCK" SV8

    // TrueAudio TTA
    m_sigs.push_back(mk(L"tta",  "5454417831", nullptr, 0, 500)); // "TTA1"

    // AMR (Adaptive Multi-Rate)
    m_sigs.push_back(mk(L"amr",  "2321414D520A", nullptr, 0, 20)); // "#!AMR\n"

    // AC-3 / Dolby Digital
    m_sigs.push_back(mk(L"ac3",  "0B77",     nullptr, 0,  100));

    // DTS
    m_sigs.push_back(mk(L"dts",  "7FFE8001", nullptr, 0,  200)); // core sync word
    m_sigs.push_back(mk(L"dts",  "1FFFE800", nullptr, 0,  200));

    // AU (Sun/NeXT audio)
    m_sigs.push_back(mk(L"au",   "2E736E64", nullptr, 0,  100)); // ".snd"

    // Ra (RealAudio)
    m_sigs.push_back(mk(L"ra",   "2E7261FD", nullptr, 0,  200)); // ".ra\xFD"
    m_sigs.push_back(mk(L"ra",   "2E524135", nullptr, 0,  200)); // ".RA5"

    // M4A iTunes (covered above by ftyp 'M4A ')

    // Opus in OGG container (covered by OGG above)

    // ═══════════════════════════════════════════════════════════════════════
    // ── DOCUMENTS ──────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // PDF
    m_sigs.push_back(mk(L"pdf",  "25504446", "2525454F46",     0, 500)); // "%PDF"…"%%EOF"
    m_sigs.push_back(mk(L"pdf",  "25504446", "2525454F460A",   0, 500));
    m_sigs.push_back(mk(L"pdf",  "25504446", "2525454F460D0A", 0, 500));

    // Microsoft Office legacy (CFB = OLE2)
    m_sigs.push_back(mk(L"doc",  kCFB, nullptr, 0, 100));
    m_sigs.push_back(mk(L"xls",  kCFB, nullptr, 0, 100));
    m_sigs.push_back(mk(L"ppt",  kCFB, nullptr, 0, 100));
    m_sigs.push_back(mk(L"mdb",  kCFB, nullptr, 0, 500)); // Access
    m_sigs.push_back(mk(L"msg",  kCFB, nullptr, 0,  50)); // Outlook message
    m_sigs.push_back(mk(L"pub",  kCFB, nullptr, 0, 100)); // Publisher
    m_sigs.push_back(mk(L"vsd",  kCFB, nullptr, 0, 100)); // Visio
    m_sigs.push_back(mk(L"msi",  kCFB, nullptr, 0, 500)); // Windows Installer

    // ZIP (PK\x03\x04) before OOXML aliases so RawScan carving labels archives as .zip
    // and runs structure-aware CarveZip (docx/xlsx still discoverable via metadata scan).
    m_sigs.push_back(mk(L"zip",  "504B0304", "504B0506", 0, 4096));
    m_sigs.push_back(mk(L"zip",  "504B0506", nullptr,    0, 4096)); // empty archive

    // Office Open XML (ZIP-based: docx/xlsx/pptx/odt/ods/odp/epub/apk etc.)
    m_sigs.push_back(mk(L"docx", "504B0304", nullptr, 0, 200));
    m_sigs.push_back(mk(L"xlsx", "504B0304", nullptr, 0, 200));
    m_sigs.push_back(mk(L"pptx", "504B0304", nullptr, 0, 500));
    m_sigs.push_back(mk(L"odt",  "504B0304", nullptr, 0, 100));
    m_sigs.push_back(mk(L"ods",  "504B0304", nullptr, 0, 100));
    m_sigs.push_back(mk(L"odp",  "504B0304", nullptr, 0, 200));
    m_sigs.push_back(mk(L"epub", "504B0304", nullptr, 0, 100));

    // RTF
    m_sigs.push_back(mk(L"rtf",  "7B5C72746631", nullptr, 0, 50)); // "{\rtf1"

    // CHM (Compiled HTML Help)
    m_sigs.push_back(mk(L"chm",  "49545346", nullptr, 0, 100)); // "ITSF"

    // DjVu
    m_sigs.push_back(mk(L"djvu", "464F524D", nullptr, 0, 50)); // "FORM" IFF container

    // DWG (AutoCAD) – many versions, all start "AC1"
    m_sigs.push_back(mk(L"dwg",  "41433130313500", nullptr, 0, 100)); // "AC1015" R2000
    m_sigs.push_back(mk(L"dwg",  "41433130313800", nullptr, 0, 100)); // "AC1018" R2004
    m_sigs.push_back(mk(L"dwg",  "41433130323100", nullptr, 0, 100)); // "AC1021" R2007
    m_sigs.push_back(mk(L"dwg",  "41433130323400", nullptr, 0, 100)); // "AC1024" R2010
    m_sigs.push_back(mk(L"dwg",  "41433130323700", nullptr, 0, 100)); // "AC1027" R2013

    // WordPerfect
    m_sigs.push_back(mk(L"wpd",  "FF575043", nullptr, 0, 50)); // "\xFF WPC"

    // WPS (Works) – CFB-based
    m_sigs.push_back(mk(L"wps",  kCFB, nullptr, 0, 50));

    // XPS (ZIP-based)
    m_sigs.push_back(mk(L"xps",  "504B0304", nullptr, 0, 50));

    // MOBI / PalmDB (Kindle)
    m_sigs.push_back(mk(L"mobi", "424F4F4B4D4F4249", nullptr, 60, 100)); // "BOOKMOBI" at +60

    // LIT (Microsoft eBook)
    m_sigs.push_back(mk(L"lit",  "49544F4C4954532F", nullptr, 0, 50)); // "ITOLITS/"

    // iCalendar / vCard
    m_sigs.push_back(mk(L"ics",  "424547494E3A5643414C454E444152", nullptr, 0, 5)); // "BEGIN:CALENDAR"
    m_sigs.push_back(mk(L"vcf",  "424547494E3A5643415244",         nullptr, 0, 5)); // "BEGIN:VCARD"

    // ═══════════════════════════════════════════════════════════════════════
    // ── ARCHIVES / COMPRESSION ─────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // RAR v4
    m_sigs.push_back(mk(L"rar",  "526172211A0700",   nullptr, 0, 4096));
    // RAR v5
    m_sigs.push_back(mk(L"rar",  "526172211A070100", nullptr, 0, 4096));

    // 7-Zip
    m_sigs.push_back(mk(L"7z",   "377ABCAF271C",     nullptr, 0, 4096));

    // GZip
    m_sigs.push_back(mk(L"gz",   "1F8B",             nullptr, 0, 4096));

    // BZip2
    m_sigs.push_back(mk(L"bz2",  "425A68",           nullptr, 0, 4096));

    // XZ
    m_sigs.push_back(mk(L"xz",   "FD377A585A00",     nullptr, 0, 4096));

    // Zstandard
    m_sigs.push_back(mk(L"zst",  "28B52FFD",         nullptr, 0, 4096));

    // LZ4
    m_sigs.push_back(mk(L"lz4",  "04224D18",         nullptr, 0, 4096));

    // LZMA
    m_sigs.push_back(mk(L"lzma", "5D000080",         nullptr, 0, 4096));

    // TAR ("ustar" at offset 257)
    m_sigs.push_back(mk(L"tar",  "7573746172",       nullptr, 257, 4096));

    // CAB (Windows Cabinet)
    m_sigs.push_back(mk(L"cab",  "4D534346",         nullptr, 0, 500)); // "MSCF"

    // LZH / LHA ("-lh" at offset 2)
    m_sigs.push_back(mk(L"lzh",  "2D6C68",           nullptr, 2, 500)); // "-lh"

    // ARJ
    m_sigs.push_back(mk(L"arj",  "60EA",             nullptr, 0, 500));

    // ACE (**ACE** at offset 7)
    m_sigs.push_back(mk(L"ace",  "2A2A4143452A2A",   nullptr, 7, 500));

    // Stuffit
    m_sigs.push_back(mk(L"sit",  "5374756666497420", nullptr, 0, 500)); // "StuffIt "
    m_sigs.push_back(mk(L"sitx", "5374756666497421", nullptr, 0, 500)); // "StuffIt!"

    // ISO 9660 (CD/DVD image) – "CD001" at sector 16 (byte 32769)
    m_sigs.push_back(mk(L"iso",  "4344303031",       nullptr, 32769, 8192*1024));

    // DMG (macOS disk image)
    m_sigs.push_back(mk(L"dmg",  "6B6F6C79",         nullptr, 0, 8192*1024)); // "koly"

    // ═══════════════════════════════════════════════════════════════════════
    // ── FONTS ──────────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    m_sigs.push_back(mk(L"ttf",   "0001000000", nullptr, 0,  20)); // TrueType
    m_sigs.push_back(mk(L"otf",   "4F54544F",   nullptr, 0,  20)); // "OTTO" OpenType/CFF
    m_sigs.push_back(mk(L"woff",  "774F4646",   nullptr, 0,  20)); // "wOFF"
    m_sigs.push_back(mk(L"woff2", "774F4632",   nullptr, 0,  20)); // "wOF2"
    m_sigs.push_back(mk(L"eot",   "4C500000",   nullptr, 8,  20)); // EOT magic at +8

    // ═══════════════════════════════════════════════════════════════════════
    // ── EXECUTABLES / CODE ─────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Windows PE (EXE / DLL / SYS / OCX)
    m_sigs.push_back(mk(L"exe",   "4D5A",       nullptr, 0, 200));
    m_sigs.push_back(mk(L"dll",   "4D5A",       nullptr, 0, 200));
    m_sigs.push_back(mk(L"sys",   "4D5A",       nullptr, 0, 200));

    // ELF (Linux/Android)
    m_sigs.push_back(mk(L"elf",   "7F454C46",   nullptr, 0, 100));

    // Mach-O (macOS) 32/64-bit little-endian and big-endian
    m_sigs.push_back(mk(L"macho", "CEFAEDFE",   nullptr, 0, 100)); // 32-bit LE
    m_sigs.push_back(mk(L"macho", "CFFAEDFE",   nullptr, 0, 100)); // 64-bit LE
    m_sigs.push_back(mk(L"macho", "FEEDFACE",   nullptr, 0, 100)); // 32-bit BE
    m_sigs.push_back(mk(L"macho", "FEEDFACF",   nullptr, 0, 100)); // 64-bit BE

    // Java Class
    m_sigs.push_back(mk(L"class", "CAFEBABE",   nullptr, 0,  10));

    // Android DEX
    m_sigs.push_back(mk(L"dex",   "6465780A",   nullptr, 0,  50)); // "dex\n"

    // Android APK (ZIP-based)
    m_sigs.push_back(mk(L"apk",   "504B0304",   nullptr, 0, 500));

    // iOS IPA (ZIP-based)
    m_sigs.push_back(mk(L"ipa",   "504B0304",   nullptr, 0, 500));

    // Windows Script/Batch – text-based, skip

    // Python PYC (Python 3.8+ magic 0x0D0D0A0A in LE)
    m_sigs.push_back(mk(L"pyc",   "0D0D0A0A",   nullptr, 0,  10));

    // ═══════════════════════════════════════════════════════════════════════
    // ── DATABASES ──────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // SQLite3
    m_sigs.push_back(mk(L"sqlite","53514C697465203320", nullptr, 0, 4096)); // "SQLite format 3"

    // Access MDB / ACCDB (CFB) – covered above by kCFB/mdb

    // MySQL FRM (table definition)
    m_sigs.push_back(mk(L"frm",   "FE01",   nullptr, 0, 10));

    // dBASE DBF
    m_sigs.push_back(mk(L"dbf",   "03",     nullptr, 0, 500)); // dBASE III+ (most common)
    m_sigs.push_back(mk(L"dbf",   "83",     nullptr, 0, 500)); // dBASE IV
    m_sigs.push_back(mk(L"dbf",   "F5",     nullptr, 0, 500)); // FoxPro

    // Firebird / InterBase GDB
    m_sigs.push_back(mk(L"gdb",   "010000000000000000000000", nullptr, 0, 4096));

    // ═══════════════════════════════════════════════════════════════════════
    // ── VIRTUAL DISKS ──────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // VHD (Virtual PC) – "conectix" footer at start of file (dynamic/differencing)
    m_sigs.push_back(mk(L"vhd",   "636F6E6563746978",   nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // VHDX
    m_sigs.push_back(mk(L"vhdx",  "76686478",           nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // VMDK sparse extent
    m_sigs.push_back(mk(L"vmdk",  "4B444D5600000001",   nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // QCOW / QCOW2 (QEMU)
    m_sigs.push_back(mk(L"qcow",  "514649FB",           nullptr, 0, static_cast<uint64_t>(8192)*1024));
    m_sigs.push_back(mk(L"qcow2", "514649FB",           nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // VDI (VirtualBox)
    m_sigs.push_back(mk(L"vdi",   "3C3C3C20566972745",  nullptr, 0, static_cast<uint64_t>(8192)*1024)); // "<<< Virt"

    // ═══════════════════════════════════════════════════════════════════════
    // ── EMAIL / PIM ────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // PST / OST (Outlook)
    m_sigs.push_back(mk(L"pst",   "2142444E",           nullptr, 0, 4096*4)); // "!BDN"
    m_sigs.push_back(mk(L"ost",   "2142444E",           nullptr, 0, 4096*4));

    // DBX (Outlook Express)
    m_sigs.push_back(mk(L"dbx",   "CFAD12FEC5FD746F",   nullptr, 0, 2048));

    // EML (MIME)
    m_sigs.push_back(mk(L"eml",   "4D494D452D56657273696F6E3A", nullptr, 0, 50)); // "MIME-Version:"
    m_sigs.push_back(mk(L"eml",   "52657475726E2D506174683A",   nullptr, 0, 50)); // "Return-Path:"

    // MBOX
    m_sigs.push_back(mk(L"mbox",  "46726F6D20",         nullptr, 0, 500)); // "From " (mbox separator)

    // ═══════════════════════════════════════════════════════════════════════
    // ── CRYPTO / CERTIFICATES ──────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // PEM (text)
    m_sigs.push_back(mk(L"pem",   "2D2D2D2D2D424547494E", nullptr, 0, 1)); // "-----BEGIN"

    // DER / CER / P12 / PFX (ASN.1 DER SEQUENCE)
    m_sigs.push_back(mk(L"der",   "308200",             nullptr, 0, 10));
    m_sigs.push_back(mk(L"p12",   "30820",              nullptr, 0, 10));

    // GPG / PGP
    m_sigs.push_back(mk(L"gpg",   "99",                 nullptr, 0, 100)); // OpenPGP packet
    m_sigs.push_back(mk(L"pgp",   "8501",               nullptr, 0, 100)); // PGP header

    // Bitcoin wallet.dat (BDB format)
    m_sigs.push_back(mk(L"wallet","00061562",           nullptr, 0, 100));

    // ═══════════════════════════════════════════════════════════════════════
    // ── 3D / CAD ───────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Blender (.blend)
    m_sigs.push_back(mk(L"blend", "424C454E444552",     nullptr, 0, 500)); // "BLENDER"

    // Autodesk FBX (binary)
    m_sigs.push_back(mk(L"fbx",   "4B617964617261204642580D0A1A0A", nullptr, 0, 500)); // "Kaydara FBX\r\n\x1a\n"

    // PLY (polygon file format)
    m_sigs.push_back(mk(L"ply",   "706C790A",           nullptr, 0, 100)); // "ply\n"
    m_sigs.push_back(mk(L"ply",   "706C790D0A",         nullptr, 0, 100)); // "ply\r\n"

    // 3DS (3D Studio)
    m_sigs.push_back(mk(L"3ds",   "4D4D",               nullptr, 0, 100)); // "MM" chunk

    // STL binary (no universal magic, first 80 bytes are ASCII header)
    // Skip – too many false positives

    // OBJ – text, no magic

    // ═══════════════════════════════════════════════════════════════════════
    // ── SCIENTIFIC / DATA ──────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // HDF5 (Hierarchical Data Format)
    m_sigs.push_back(mk(L"hdf5",  "894844460D0A1A0A",   nullptr, 0, 4096)); // "\x89HDF\r\n\x1a\n"

    // MATLAB .mat (v5 and v7.3)
    m_sigs.push_back(mk(L"mat",   "4D41544C4142",       nullptr, 0, 500)); // "MATLAB"

    // FITS (astronomy)
    m_sigs.push_back(mk(L"fits",  "53494D504C452020",   nullptr, 0, 500)); // "SIMPLE  "

    // NetCDF (v1 and v2)
    m_sigs.push_back(mk(L"nc",    "43444601",           nullptr, 0, 4096)); // "CDF\x01"
    m_sigs.push_back(mk(L"nc",    "43444602",           nullptr, 0, 4096)); // "CDF\x02"

    // ROOT (CERN high energy physics)
    m_sigs.push_back(mk(L"root",  "726F6F74",           nullptr, 0, 4096)); // "root"

    // ═══════════════════════════════════════════════════════════════════════
    // ── WINDOWS SYSTEM ─────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Windows Registry hive
    m_sigs.push_back(mk(L"dat",   "72656766",           nullptr, 0, 500));  // "regf" (hive)

    // Windows Event Log (EVTX)
    m_sigs.push_back(mk(L"evtx",  "456C6646696C6500",   nullptr, 0, 500));  // "ElfFile\0"

    // Windows Event Log (old .evt)
    m_sigs.push_back(mk(L"evt",   "30000000LfLe",       nullptr, 0, 100));  // rough match

    // Windows LNK (shortcut)
    m_sigs.push_back(mk(L"lnk",   "4C000000",           nullptr, 0,   5));  // "L\0\0\0"

    // Windows Prefetch
    m_sigs.push_back(mk(L"pf",    "534341",             nullptr, 0,   5));  // "SCA" (Prefetch v1)

    // Windows Crash Dump
    m_sigs.push_back(mk(L"dmp",   "5041474544554D5036340000", nullptr, 0, 4096*1024)); // "PAGEDUMP64"
    m_sigs.push_back(mk(L"dmp",   "4D444D5093A7",       nullptr, 0, 4096*1024)); // "MDMP" minidump

    // ═══════════════════════════════════════════════════════════════════════
    // ── MISC ───────────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // BitTorrent
    m_sigs.push_back(mk(L"torrent","6438",              nullptr, 0, 5));    // "d8" bencode dict

    // NES ROM
    m_sigs.push_back(mk(L"nes",   "4E45531A",           nullptr, 0, 1));   // "NES\x1a"

    // Game Boy ROM
    m_sigs.push_back(mk(L"gb",    "CEED6666CC0D000B",   nullptr, 0x104, 1)); // Nintendo logo at +0x104

    // SEGA Mega Drive / Genesis
    m_sigs.push_back(mk(L"md",    "5345474120",         nullptr, 0x100, 2)); // "SEGA " at +0x100

    // PCAP (network capture, Wireshark)
    m_sigs.push_back(mk(L"pcap",  "D4C3B2A1",          nullptr, 0, 500));  // little-endian
    m_sigs.push_back(mk(L"pcap",  "A1B2C3D4",          nullptr, 0, 500));  // big-endian
    m_sigs.push_back(mk(L"pcapng","0A0D0D0A",          nullptr, 0, 500));  // pcapng

    // Tcpdump / libpcap with nanosecond resolution
    m_sigs.push_back(mk(L"pcap",  "4D3CB2A1",          nullptr, 0, 500));

    // XML (generic)
    m_sigs.push_back(mk(L"xml",   "3C3F786D6C",        nullptr, 0, 20));   // "<?xml"

    // HTML
    m_sigs.push_back(mk(L"html",  "3C68746D6C",        nullptr, 0, 10));   // "<html"
    m_sigs.push_back(mk(L"html",  "3C48544D4C",        nullptr, 0, 10));   // "<HTML"
    m_sigs.push_back(mk(L"html",  "3C21444F4354595045", nullptr, 0, 10));  // "<!DOCTYPE"

    // Markdown / plain text – skip (no magic)

    // Android backup (.ab)
    m_sigs.push_back(mk(L"ab",    "414E44524F494420",   nullptr, 0, 4096)); // "ANDROID "

    // Lua bytecode
    m_sigs.push_back(mk(L"luac",  "1B4C7561",           nullptr, 0, 10));   // "\x1bLua"

    // WebAssembly
    m_sigs.push_back(mk(L"wasm",  "0061736D",           nullptr, 0, 100));  // "\0asm"

    // Java KeyStore
    m_sigs.push_back(mk(L"jks",   "FEEDFEED",           nullptr, 0, 100));

    // Apple Disk Image (sparse)
    m_sigs.push_back(mk(L"sparseimage", "737061727365696D616765", nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL IMAGES (PhotoRec extended) ──────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // OpenEXR (HDR film / VFX)
    m_sigs.push_back(mk(L"exr",   "762F3101",           nullptr, 0,  200)); // "v/1\x01"

    // Radiance HDR / RGBE
    m_sigs.push_back(mk(L"hdr",   "233F5241444941",     nullptr, 0,   50)); // "#?RADIA"

    // JNG (JPEG Network Graphics)
    m_sigs.push_back(mk(L"jng",   "8B4A4E470D0A1A0A",   nullptr, 0,   20));

    // MNG (Multiple-image Network Graphics)
    m_sigs.push_back(mk(L"mng",   "8A4D4E470D0A1A0A",   nullptr, 0,   50));

    // SGI / RGB image
    m_sigs.push_back(mk(L"sgi",   "01DA",               nullptr, 0,   50));

    // Apple ICNS (icon bundle)
    m_sigs.push_back(mk(L"icns",  "69636E73",           nullptr, 0,    5)); // "icns"

    // Windows Metafile (WMF) – placeable
    m_sigs.push_back(mk(L"wmf",   "D7CDC69A",           nullptr, 0,   10));
    // WMF standard (no placeable header)
    m_sigs.push_back(mk(L"wmf",   "0100090003",         nullptr, 0,   10));

    // Enhanced Metafile (EMF) – " EMF" at offset 40
    m_sigs.push_back(mk(L"emf",   "01000000",           nullptr, 0,   50, "454D4600", 40)); // "\x01\0\0\0"..."EMF\0" at +40

    // XPM (text-based pixmap, C header)
    m_sigs.push_back(mk(L"xpm",   "2F2A2058504D202A2F", nullptr, 0,   10)); // "/* XPM */"

    // PCD (Kodak Photo CD)
    m_sigs.push_back(mk(L"pcd",   "5050434400000100",   nullptr, 0x800, 100)); // "PPCD\0\0\x01\0" at sector boundary

    // FPX (FlashPix) – OLE2 CFB, covered by kCFB above
    // Already covered as generic OLE2 entry

    // Cineon / DPX (film scanning)
    m_sigs.push_back(mk(L"cin",   "802A5FD7",           nullptr, 0,  200)); // Cineon
    m_sigs.push_back(mk(L"dpx",   "53445058",           nullptr, 0,  200)); // "SDPX" big-endian
    m_sigs.push_back(mk(L"dpx",   "58504453",           nullptr, 0,  200)); // "XPDS" little-endian

    // CIFF / CRW (Canon RAW v1, older cameras)
    m_sigs.push_back(mk(L"crw",   "4949001000000000",   nullptr, 0,   50)); // CIFF LE

    // PGF (Progressive Graphics File)
    m_sigs.push_back(mk(L"pgf",   "50474600",           nullptr, 0,  100)); // "PGF\0"

    // Wireless Bitmap (WBMP)
    // Too generic – skip

    // ═══════════════════════════════════════════════════════════════════════
    // ── COREL FORMATS ──────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // CorelDRAW CDR – RIFF with "CDR" version tag at offset 8
    m_sigs.push_back(riff(L"cdr",  "43445200", 500)); // "CDR\0"
    m_sigs.push_back(riff(L"cdr",  "43445220", 500)); // "CDR " old versions
    // CDR vX4+ uses RIFF + "cdr6"/"cdr7" etc
    m_sigs.push_back(mk(L"cdr",   "52494646", nullptr, 0, 500, "636472", 8)); // any "cdr*" at +8

    // Corel Presentation Exchange (CMX)
    m_sigs.push_back(riff(L"cmx",  "434D5800", 500)); // "CMX\0"
    m_sigs.push_back(riff(L"cmx",  "434D5820", 500)); // "CMX "

    // Corel Photo-Paint (CPT)
    m_sigs.push_back(mk(L"cpt",   "434F5245",           nullptr, 0,  200)); // "CORE" header

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADOBE FORMATS ──────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Photoshop Brush (ABR)
    m_sigs.push_back(mk(L"abr",   "00010000",           nullptr, 0,   50)); // v1
    m_sigs.push_back(mk(L"abr",   "00020000",           nullptr, 0,   50)); // v2
    m_sigs.push_back(mk(L"abr",   "00060000",           nullptr, 0,   50)); // v6

    // Photoshop Color Swatch (ACO)
    m_sigs.push_back(mk(L"aco",   "00010000",           nullptr, 0,    5)); // v1 – but very weak
    // Note: ACO magic is too generic; skip separate entry – conflicts with ABR

    // Photoshop Gradient (GRD)
    m_sigs.push_back(mk(L"grd",   "47726473",           nullptr, 0,   50)); // "Grds"
    m_sigs.push_back(mk(L"grd",   "38427267",           nullptr, 0,   50)); // "8Brg" v5+

    // Photoshop Styles (ASL)
    m_sigs.push_back(mk(L"asl",   "38425374",           nullptr, 0,   10)); // "8BSt"

    // Photoshop Shapes (CSH)
    m_sigs.push_back(mk(L"csh",   "63757368",           nullptr, 0,   10)); // "cush"
    m_sigs.push_back(mk(L"csh",   "38427368",           nullptr, 0,   10)); // "8Bsh"

    // Photoshop Pattern (PAT)
    m_sigs.push_back(mk(L"pat",   "38425054",           nullptr, 0,   10)); // "8BPT"

    // Adobe InDesign (INDD)
    m_sigs.push_back(mk(L"indd",  "0606EDF5D81D46E5",   nullptr, 0,  500));

    // Adobe Illustrator legacy (AI – binary old format)
    // Modern AI is PDF-based (already covered). Legacy:
    m_sigs.push_back(mk(L"ai",    "C5D0D3C6",           nullptr, 0,  200)); // Windows EPS/AI binary

    // ═══════════════════════════════════════════════════════════════════════
    // ── TRACKER MUSIC / GAME AUDIO ─────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Impulse Tracker (IT)
    m_sigs.push_back(mk(L"it",    "494D504D",           nullptr, 0,   20)); // "IMPM"

    // FastTracker II (XM)
    m_sigs.push_back(mk(L"xm",    "457874656E64656420436F",nullptr,0,  20)); // "Extended Co"

    // Scream Tracker 3 (S3M) – "SCRM" at offset 0x2C
    m_sigs.push_back(mk(L"s3m",   "5343524D",           nullptr, 0x2C, 20)); // "SCRM"

    // Scream Tracker 2 (STM)
    m_sigs.push_back(mk(L"stm",   "212153637265616D",   nullptr, 0,   20)); // "!!Scream"

    // DigiTracker (MDL)
    m_sigs.push_back(mk(L"mdl",   "444946435448454B",   nullptr, 0,   20)); // "DIFCTHEK"

    // Oktalyzer (OKT)
    m_sigs.push_back(mk(L"okt",   "4F4B54415353414E47", nullptr, 0,   10)); // "OKTASANG"

    // VGM (Video Game Music)
    m_sigs.push_back(mk(L"vgm",   "56676D20",           nullptr, 0,   10)); // "Vgm "
    // VGZ = gzip-compressed VGM (covered by gz)

    // NSF (NES Sound Format)
    m_sigs.push_back(mk(L"nsf",   "4E45534D1A",         nullptr, 0,    5)); // "NESM\x1a"

    // NSFE (extended NSF)
    m_sigs.push_back(mk(L"nsfe",  "4E534645",           nullptr, 0,    5)); // "NSFE"

    // SPC (SNES SPC700 sound)
    m_sigs.push_back(mk(L"spc",   "534E45532D53504337",  nullptr, 0,   1)); // "SNES-SPC7"

    // GYM (Genesis YM2612)
    m_sigs.push_back(mk(L"gym",   "47594D58",           nullptr, 0,   10)); // "GYMX"

    // PSF (PlayStation Sound Format)
    m_sigs.push_back(mk(L"psf",   "5053460100000000",   nullptr, 0,   10)); // "PSF\x01"

    // SAP (Atari Sound)
    m_sigs.push_back(mk(L"sap",   "5341503D",           nullptr, 0,    5)); // "SAP="

    // SID (Commodore 64)
    m_sigs.push_back(mk(L"sid",   "50534944",           nullptr, 0,    5)); // "PSID"
    m_sigs.push_back(mk(L"sid",   "52534944",           nullptr, 0,    5)); // "RSID"

    // FLAC in OGG – covered by OGG above
    // SPX (Speex in OGG) – covered by OGG above

    // ═══════════════════════════════════════════════════════════════════════
    // ── GAME FORMATS ───────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Game Boy Advance ROM (GBA) – Nintendo logo data at +4
    m_sigs.push_back(mk(L"gba",   "2EFFD8E090E00E21", nullptr, 4, 32)); // GBA entry code at +4

    // SNES ROM (SFC / SMC)
    // First 512 bytes may be SMC header; actual cart data has checksum at 0x7FDC
    // No reliable magic – skip

    // Sega Master System / Game Gear (SMS/GG)
    // "TMR SEGA" at offset 0x7FF0 for some – too deep, skip

    // Nintendo 64 (Z64 big-endian)
    m_sigs.push_back(mk(L"z64",   "80371240",           nullptr, 0, 64)); // Z64 format
    // N64 byte-swapped (v64)
    m_sigs.push_back(mk(L"v64",   "37804012",           nullptr, 0, 64));
    // N64 little-endian (n64)
    m_sigs.push_back(mk(L"n64",   "40123780",           nullptr, 0, 64));

    // PlayStation PS1 save
    m_sigs.push_back(mk(L"mcr",   "4D432D5361726100",   nullptr, 0,  1)); // "MC-Sara\0"

    // GameCube / Wii ISO – magic at 0x1C
    m_sigs.push_back(mk(L"gcm",   "C2339F3D",           nullptr, 0x1C, 4096*1024));

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL ARCHIVES ────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // UNIX compress (.Z)
    m_sigs.push_back(mk(L"z",     "1F9D",               nullptr, 0, 500));
    // UNIX pack (.z)
    m_sigs.push_back(mk(L"z",     "1FA0",               nullptr, 0, 500));

    // LZO compressed
    m_sigs.push_back(mk(L"lzo",   "894C5A4F000D0A1A0A", nullptr, 0, 4096)); // "\x89LZO\r\n\x1a\n"

    // XAR (eXtensible ARchive, used by macOS packages)
    m_sigs.push_back(mk(L"xar",   "78617221",           nullptr, 0, 500)); // "xar!"

    // PAR2 (Parity Archive v2)
    m_sigs.push_back(mk(L"par2",  "5041523200000000",   nullptr, 0, 500)); // "PAR2\0\0\0\0"

    // PAR (Parity Archive v1)
    m_sigs.push_back(mk(L"par",   "504152490000000000000000", nullptr, 0, 500)); // "PARI\0..."

    // WIM (Windows Imaging Format)
    m_sigs.push_back(mk(L"wim",   "4D5357494D00000000D000000000", nullptr, 0, static_cast<uint64_t>(8192)*1024)); // "MSWIM\0..."

    // HAP (HAP archive)
    m_sigs.push_back(mk(L"hap",   "91334861",           nullptr, 0, 500));

    // ZPAQ
    m_sigs.push_back(mk(L"zpaq",  "7A504151",           nullptr, 0, 4096)); // "zPAQ"

    // RAR SFX / self-extracting – PE + RAR inside (covered by EXE)

    // Debian package (DEB)
    m_sigs.push_back(mk(L"deb",   "213C617263683E0A",   nullptr, 0, 500)); // "!<arch>\n"

    // RPM package
    m_sigs.push_back(mk(L"rpm",   "EDABEEDB",           nullptr, 0, 500));

    // ═══════════════════════════════════════════════════════════════════════
    // ── DOCUMENT / PRODUCTIVITY ────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // AbiWord document
    m_sigs.push_back(mk(L"abw",   "3C3F786D6C",         nullptr, 0,  20)); // "<?xml" (same as XML)
    m_sigs.push_back(mk(L"abw",   "3C61626977",         nullptr, 0,  20)); // "<abiw"

    // Windows Write (WRI)
    m_sigs.push_back(mk(L"wri",   "31BE0000",           nullptr, 0,  10)); // v1
    m_sigs.push_back(mk(L"wri",   "32BE0000",           nullptr, 0,  10)); // v2

    // Windows Help (HLP)
    m_sigs.push_back(mk(L"hlp",   "3F5F0300",           nullptr, 0,  50)); // "?_\x03\x00"

    // Windows Clipboard (CLP)
    m_sigs.push_back(mk(L"clp",   "C8C0",               nullptr, 0,   5));

    // QuarkXPress
    m_sigs.push_back(mk(L"qxd",   "00000049000000",     nullptr, 0,  200)); // QXPD v4
    m_sigs.push_back(mk(L"qxd",   "A0004900000000",     nullptr, 0,  200)); // QXPD BE

    // FrameMaker (FM / MIF)
    m_sigs.push_back(mk(L"fm",    "3C4D616B65724F7574707574", nullptr, 0, 100)); // "<MakerOutput"
    m_sigs.push_back(mk(L"mif",   "3C4D616B65724F7574707574", nullptr, 0, 100));

    // PageMaker
    m_sigs.push_back(mk(L"pm",    "414C445553",         nullptr, 0,  100)); // "ALDUS" (Aldus PageMaker)

    // Ventura Publisher (VPS / CHP / STY)
    // No reliable magic

    // Apple Pages / Keynote / Numbers (ZIP-based IWORK)
    m_sigs.push_back(mk(L"pages", "504B0304",           nullptr, 0,  200));
    m_sigs.push_back(mk(L"key",   "504B0304",           nullptr, 0,  200));
    m_sigs.push_back(mk(L"numbers","504B0304",          nullptr, 0,  200));

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL VIDEO ───────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Nullsoft Streaming Video (NSV)
    m_sigs.push_back(mk(L"nsv",   "4E53566600",         nullptr, 0, 1024)); // "NSVf\0"
    m_sigs.push_back(mk(L"nsv",   "4E535673",           nullptr, 0, 1024)); // "NSVs" chunk

    // General Exchange Format (GXF)
    m_sigs.push_back(mk(L"gxf",   "0000000001BC",       nullptr, 0, 4096));

    // MXF (Material eXchange Format)
    m_sigs.push_back(mk(L"mxf",   "060E2B3402050101",   nullptr, 0, 4096)); // SMPTE UL prefix

    // DIF / DV (Digital Video)
    m_sigs.push_back(mk(L"dif",   "1F070000",           nullptr, 0, 4096)); // DIF block header

    // Indeo Video (AVI-based, covered)

    // MPEG-4 Part 14 (generic ftyp fallback already covered)

    // ─── MPEG Variants ───
    // MPEG-2 Transport Stream with 192-byte packets (Blu-ray M2TS)
    // Already have mts

    // VC-1 Advanced Profile
    m_sigs.push_back(mk(L"vc1",   "000001",             nullptr, 0, 4096)); // start code prefix

    // ═══════════════════════════════════════════════════════════════════════
    // ── SECURITY / PASSWORD ────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // KeePass 2 (KDBX)
    m_sigs.push_back(mk(L"kdbx",  "03D9A29A",           nullptr, 0,   10));

    // KeePass 1 (KDB)
    m_sigs.push_back(mk(L"kdb",   "03D9A29A",           nullptr, 0,    5));

    // LastPass encrypted vault (local)
    // No standard magic

    // TrueCrypt / VeraCrypt container
    // Deliberately no magic – skip (by design of encrypted container)

    // SSH private key (OpenSSH new format)
    m_sigs.push_back(mk(L"key",   "6F70656E737368",     nullptr, 0,    5)); // "openssh"

    // ═══════════════════════════════════════════════════════════════════════
    // ── DISK / SYSTEM IMAGES ───────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Nero Burning ROM image (NRG)
    // NRG v1: "NERO" at end of file; v2: "NER5" at end. Not detectable at start – skip

    // BIN/CUE – raw sector dump
    // No universal magic at start

    // MDF (Alcohol 120% image) – MDFH header
    m_sigs.push_back(mk(L"mdf",   "4D454449412044455343", nullptr, 0, static_cast<uint64_t>(4096)*1024)); // "MEDIA DESC"

    // B5T (BlindWrite) – "BWT5" 
    m_sigs.push_back(mk(L"b5t",   "42575435",           nullptr, 0, static_cast<uint64_t>(4096)*1024)); // "BWT5"

    // Ghost image (GHO / GHS) – Symantec Ghost
    m_sigs.push_back(mk(L"gho",   "FEEFFEED",           nullptr, 0, static_cast<uint64_t>(8192)*1024));

    // Hyper-V AVHD (differencing disk)
    m_sigs.push_back(mk(L"avhd",  "636F6E6563746978",   nullptr, 0, static_cast<uint64_t>(8192)*1024)); // same magic as VHD

    // ═══════════════════════════════════════════════════════════════════════
    // ── MOBILE / SMARTPHONE ────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // iTunes Backup (Manifest.plist) – binary plist
    m_sigs.push_back(mk(L"plist", "62706C697374",       nullptr, 0,   10)); // "bplist"

    // BlackBerry IPD backup
    m_sigs.push_back(mk(L"ipd",   "496E74657253796E",   nullptr, 0,   10)); // "InterSyn"

    // Android sparse image
    m_sigs.push_back(mk(L"simg",  "3AFF26ED",           nullptr, 0, static_cast<uint64_t>(4096)*1024));

    // ═══════════════════════════════════════════════════════════════════════
    // ── WEB / NETWORK / DATA ───────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // WARC (Web ARChive)
    m_sigs.push_back(mk(L"warc",  "574152432F",         nullptr, 0,  500)); // "WARC/"

    // HAR – JSON text, skip

    // Parquet (columnar data, Hadoop)
    m_sigs.push_back(mk(L"parquet","5041521",           nullptr, 0, 4096)); // "PAR1"

    // Avro (data serialization)
    m_sigs.push_back(mk(L"avro",  "4F626A01",           nullptr, 0, 4096)); // "Obj\x01"

    // ORC (Optimized Row Columnar)
    m_sigs.push_back(mk(L"orc",   "4F524300",           nullptr, 0, 4096)); // "ORC\0"

    // Protocol Buffers – no magic

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL PROGRAMMING / DEV ───────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // .NET assembly / CLR (MZ PE with CLR header – covered by EXE)
    // Flash SWC (component – ZIP-based)
    m_sigs.push_back(mk(L"swc",   "504B0304",           nullptr, 0,  100));

    // GraalScript (GS2)  – no magic

    // Ruby RubyGems package
    m_sigs.push_back(mk(L"gem",   "4745432E302E30",     nullptr, 0,   10)); // "GEM.0.0" (old format)

    // Python wheel (ZIP-based)
    m_sigs.push_back(mk(L"whl",   "504B0304",           nullptr, 0,  200));

    // Node.js packed app
    m_sigs.push_back(mk(L"asar",  "41534152",           nullptr, 4,  500)); // "ASAR" at +4

    // ═══════════════════════════════════════════════════════════════════════
    // ── MISC (PhotoRec long-tail) ──────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Windows Registry Transaction Log
    m_sigs.push_back(mk(L"log",   "72656766",           nullptr, 0,  500)); // "regf" duplicate for LOG

    // Thumbs.db (OLE2) – covered by CFB above

    // Windows Restore Point (rp.log)
    m_sigs.push_back(mk(L"rp",    "52455354",           nullptr, 0,   10)); // "REST"

    // Apple DMG (already have koly)
    // Apple PKG – XAR-based (already have xar)

    // SANE scan data
    // TIFF already covers most scanner output

    // SRF (Sony RAW – Sony Alpha)
    m_sigs.push_back(mk(L"srf",   "49492A00",           nullptr, 0,  100, "2100000007000000", 8)); // TIFF LE + specific IFD at +8

    // SR2 (Sony RAW v2)
    m_sigs.push_back(mk(L"sr2",   "49492A00",           nullptr, 0,  100));

    // PEF (Pentax RAW)
    m_sigs.push_back(mk(L"pef",   "49492A00",           nullptr, 0,  100)); // TIFF LE

    // NRW (Nikon) – TIFF LE
    m_sigs.push_back(mk(L"nrw",   "49492A00",           nullptr, 0,  100));

    // ERF (Epson RAW)
    m_sigs.push_back(mk(L"erf",   "49492A00",           nullptr, 0,  100));

    // 3FR (Hasselblad RAW) – TIFF LE
    m_sigs.push_back(mk(L"3fr",   "49492A00",           nullptr, 0,  100));

    // IIQ (Phase One RAW)
    m_sigs.push_back(mk(L"iiq",   "49492A0000000000",   nullptr, 0,  100));

    // Leica RAW (DNG-based)
    m_sigs.push_back(mk(L"rwl",   "49492A00",           nullptr, 0,  100));

    // Aptus RAW (Mamiya / Leaf)
    m_sigs.push_back(mk(L"mos",   "49492A00",           nullptr, 0,  100));

    // Media RSS feed – XML, covered by XML

    // Google Chrome extension (CRX)
    m_sigs.push_back(mk(L"crx",   "43723234",           nullptr, 0,  200)); // "Cr24"

    // Firefox extension (XPI – ZIP-based, covered)

    // Thunderbird mail (MBOX) – already have "From "

    // Microsoft Outlook NK2 (nickname/autocomplete)
    m_sigs.push_back(mk(L"nk2",   "0D00000000000000",   nullptr, 0,   20));

    // Outlook autocomplete (DAT v16)
    m_sigs.push_back(mk(L"dat",   "0D00000000000000",   nullptr, 0,   20));

    // Windows CardSpace
    m_sigs.push_back(mk(L"crd",   "EFBBBF",             nullptr, 0,    5)); // UTF-8 BOM

    // BSON (Binary JSON)
    // No fixed magic; BSON is a length-prefixed binary format

    // MessagePack – no magic

    // Apache Arrow IPC
    m_sigs.push_back(mk(L"arrow", "4152524F5731",       nullptr, 0, 4096)); // "ARROW1"

    // Feather (Apache Arrow v1/v2)
    m_sigs.push_back(mk(L"feather","4645413100000000",  nullptr, 0, 4096)); // "FEA1\0\0\0\0"

    // Ethereum Wallet (JSON keystore) – JSON text, skip

    // Windows Thumbnail Cache (thumbcache)
    m_sigs.push_back(mk(L"db",    "434D4D4D",           nullptr, 0,  500)); // "CMMM" thumbcache

    // Windows Search Index (Extensible Storage Engine)
    m_sigs.push_back(mk(L"edb",   "EFCDAB89",           nullptr, 4, 4096*1024));

    // ═══════════════════════════════════════════════════════════════════════
    // ── MEDICAL / SCIENTIFIC ───────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // DICOM (Digital Imaging and Communications in Medicine)
    // "DICM" at byte offset 128 (after 128-byte preamble)
    m_sigs.push_back(mk(L"dcm",   "4449434D",           nullptr, 128, 200));

    // FITS (already have, add variant)
    // NetCDF (already have)

    // FCS (Flow Cytometry Standard)
    m_sigs.push_back(mk(L"fcs",   "464353332E30",       nullptr, 0,  50)); // "FCS3.0"
    m_sigs.push_back(mk(L"fcs",   "464353322E30",       nullptr, 0,  50)); // "FCS2.0"

    // NITF (National Imagery Transmission Format)
    m_sigs.push_back(mk(L"ntf",   "4E495446",           nullptr, 0, 500));  // "NITF"

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL AUDIO ───────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // CAF (Apple Core Audio Format)
    m_sigs.push_back(mk(L"caf",   "63616666",           nullptr, 0, 500));  // "caff"

    // AWB (AMR-WB wideband)
    m_sigs.push_back(mk(L"awb",   "2321414D522D57420A", nullptr, 0,  20));  // "#!AMR-WB\n"

    // QCP (QUALCOMM PureVoice) – RIFF+QLCM
    m_sigs.push_back(riff(L"qcp", "514C434D", 20));                         // "QLCM"

    // XMF (Extensible Music Format)
    m_sigs.push_back(mk(L"xmf",   "584D465F",           nullptr, 0,  10));  // "XMF_"

    // CMF (Creative Music Format)
    m_sigs.push_back(mk(L"cmf",   "4354 4D46",          nullptr, 0,  10));  // "CTMF"

    // PMP (Sony PSP audio)
    m_sigs.push_back(mk(L"pmp",   "00505350",           nullptr, 0,  50));  // "\0PSP"

    // SID (Commodore 64 – already have PSID/RSID above)

    // MED (Amiga MED/OctaMED module)
    m_sigs.push_back(mk(L"med",   "4D454434",           nullptr, 0,  10));  // "MED4"
    m_sigs.push_back(mk(L"med",   "4D4D4432",           nullptr, 0,  10));  // "MMD2"
    m_sigs.push_back(mk(L"med",   "4D4D4430",           nullptr, 0,  10));  // "MMD0"

    // Amiga ProTracker MOD (M.K. at offset 1080 = 0x438)
    m_sigs.push_back(mk(L"mod",   "4D2E4B2E",           nullptr, 0x438, 5)); // "M.K."
    m_sigs.push_back(mk(L"mod",   "4D214B21",           nullptr, 0x438, 5)); // "M!K!"

    // PSF v1 (Portable Sound Format – PlayStation)
    m_sigs.push_back(mk(L"psf",   "50534600",           nullptr, 0,  10));   // "PSF\0"

    // PC Screen Font v1
    m_sigs.push_back(mk(L"psf",   "3604",               nullptr, 0,   1));   // PSF1 magic

    // PC Screen Font v2
    m_sigs.push_back(mk(L"psf",   "864AB572",           nullptr, 0,   1));   // PSF2 magic

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL IMAGES ──────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // JPEG XR / HD Photo / WDP (Microsoft)
    m_sigs.push_back(mk(L"jxr",   "4949BC01",           nullptr, 0,  100)); // "II\xBC\x01"
    m_sigs.push_back(mk(L"wdp",   "4949BC01",           nullptr, 0,  100)); // same

    // JBIG2 standalone
    m_sigs.push_back(mk(L"jb2",   "974A423220 0D0A1A0A",nullptr, 0,  100)); // "\x97JB2 \r\n\x1a\n"

    // FLC (Autodesk FLIC animation) – magic at offset 4
    m_sigs.push_back(mk(L"flc",   "AF12",               nullptr, 4,  100)); // FLC
    m_sigs.push_back(mk(L"fli",   "AF11",               nullptr, 4,   10)); // FLI (older)

    // ANI (Windows Animated Cursor) – RIFF+ACON
    m_sigs.push_back(riff(L"ani", "41434F4E", 5));                           // "ACON"

    // Jasc Paint Shop Pro (PSP)
    m_sigs.push_back(mk(L"pspimage","7E424A490D0A1A0A", nullptr, 0,  200)); // "~BJI\r\n\x1a\n"

    // MLV (Magic Lantern Video)
    m_sigs.push_back(mk(L"mlv",   "4D4C5649",           nullptr, 0, 4096)); // "MLVI"

    // R3D (RED camera RAW)
    m_sigs.push_back(mk(L"r3d",   "52454420",           nullptr, 0,  500)); // "RED "

    // Cineon (already have) / DPX (already have)

    // CALS raster (CAL / RST)
    m_sigs.push_back(mk(L"cal",   "736363733A696D61676566696C65", nullptr, 0, 50)); // "srcs:imagefile"

    // Lightwave 3D Object (LWO)
    m_sigs.push_back(riff(L"lwo", "4C574F42", 100));  // "LWOB" LW5-
    m_sigs.push_back(riff(L"lwo", "4C574F32", 100));  // "LWO2" LW6+

    // ═══════════════════════════════════════════════════════════════════════
    // ── DVD / OPTICAL MEDIA ────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // DVD IFO (Video Manager / Video Title Set)
    m_sigs.push_back(mk(L"ifo",   "44564456494445 4F5F564D47", nullptr, 0, 1)); // "DVDVIDEO_VMG"
    m_sigs.push_back(mk(L"ifo",   "4456445649444F5F565453", nullptr, 0, 1));    // "DVDVIDEO_VTS"

    // CDA (CD Audio shortcut) – RIFF+CDDA
    m_sigs.push_back(riff(L"cda", "43444441", 1));  // "CDDA"

    // BUP (DVD backup of IFO)
    // Same magic as IFO – skip (already covered)

    // ═══════════════════════════════════════════════════════════════════════
    // ── MUSIC NOTATION ─────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Sibelius (SIB)
    m_sigs.push_back(mk(L"sib",   "0F5349 42454C495553", nullptr, 0,  100)); // "\x0fSIBELIUS"

    // Finale (MUS)
    m_sigs.push_back(mk(L"mus",   "454E4947 4D41",      nullptr, 0,  100)); // "ENIGMA" (old)

    // Guitar Pro (GP3/GP4/GP5)
    m_sigs.push_back(mk(L"gp3",   "EFBBBF",             nullptr, 0,   10)); // UTF-8 BOM (text-based)

    // MuseScore (MSCZ / MSCX)
    m_sigs.push_back(mk(L"mscz",  "504B0304",           nullptr, 0,  100)); // ZIP-based

    // ═══════════════════════════════════════════════════════════════════════
    // ── EMBROIDERY ─────────────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // PEC/PES (Brother embroidery)
    m_sigs.push_back(mk(L"pes",   "23504553",           nullptr, 0,    5)); // "#PES"
    m_sigs.push_back(mk(L"pec",   "23504543",           nullptr, 0,    5)); // "#PEC"

    // DST (Tajima embroidery) – no magic, skip
    // JEF (Janome embroidery)
    m_sigs.push_back(mk(L"jef",   "4A454600",           nullptr, 0,   10)); // "JEF\0"

    // HUS (Husqvarna Viking)
    m_sigs.push_back(mk(L"hus",   "5D FC C8 00",        nullptr, 0,   10));

    // ═══════════════════════════════════════════════════════════════════════
    // ── CAD / ENGINEERING ──────────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // PCB Design (Eagle BRD)
    m_sigs.push_back(mk(L"brd",   "3C 3F 78 6D 6C",     nullptr, 0,  100)); // XML-based Eagle v6+
    m_sigs.push_back(mk(L"brd",   "EFBBBF",             nullptr, 0,  100)); // BOM-prefixed XML

    // SKP (SketchUp) – binary format
    m_sigs.push_back(mk(L"skp",   "FFFE FF0E 53006B006500 74006300 68005500",nullptr, 0, 500)); // UTF-16LE "Sketchu"

    // IFC (Industry Foundation Classes, BIM)
    m_sigs.push_back(mk(L"ifc",   "49534F2D313030303120",nullptr, 0,  500)); // "ISO-10303-21"

    // STEP (ISO 10303)
    m_sigs.push_back(mk(L"stp",   "49534F2D313030303120",nullptr, 0,  500)); // "ISO-10303-21"

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL SYSTEM / OS ─────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // macOS .DS_Store (Apple desktop metadata)
    m_sigs.push_back(mk(L"ds_store","00000001427564647900000000", nullptr, 0, 10)); // "\x00\x00\x00\x01Buddy\x00..."

    // TZif (POSIX timezone database)
    m_sigs.push_back(mk(L"tzif",  "545A6966",           nullptr, 0,    5)); // "TZif"

    // Windows Registry text export (REGEDIT4 / Windows Registry Editor v5)
    m_sigs.push_back(mk(L"reg",   "52454745444954340A",  nullptr, 0,   50)); // "REGEDIT4\n"
    m_sigs.push_back(mk(L"reg",   "FFFE 57 00 69 00 6E 00 64 00 6F 00 77 00 73 00",nullptr, 0, 50)); // UTF-16LE BOM + "Windows"

    // macOS property list (binary bplist – already have)
    // KML (Google Earth)
    m_sigs.push_back(mk(L"kml",   "3C 6B 6D 6C",        nullptr, 0,  100)); // "<kml"
    m_sigs.push_back(mk(L"kml",   "3C 3F 78 6D 6C",     nullptr, 0,  100)); // "<?xml" + check content

    // GPX (GPS Exchange Format, XML)
    m_sigs.push_back(mk(L"gpx",   "3C67707800",         nullptr, 0,  100)); // "<gpx\0"

    // UIF (MagicISO image)
    m_sigs.push_back(mk(L"uif",   "4D616769634953 4F00", nullptr, 0, static_cast<uint64_t>(4096)*1024)); // "MagicISO\0"

    // BKF (Windows Backup, NTBackup)
    m_sigs.push_back(mk(L"bkf",   "54415045",           nullptr, 0,  4096)); // "TAPE"

    // Tape Archive MAM format
    m_sigs.push_back(mk(L"mam",   "4D414D20",           nullptr, 0,   100)); // "MAM "

    // ═══════════════════════════════════════════════════════════════════════
    // ── ADDITIONAL GAME FORMATS ────────────────────────────────────────────
    // ═══════════════════════════════════════════════════════════════════════

    // Nintendo DS ROM (NDS) – Nintendo logo at 0xC0
    m_sigs.push_back(mk(L"nds",   "24FFAE510699AA2221D729", nullptr, 0xC0, 64)); // Nintendo DS logo

    // PSP Homebrew (EBOOT.PBP)
    m_sigs.push_back(mk(L"pbp",   "00504250",           nullptr, 0,  100));  // "\0PBP"

    // GBA (Game Boy Advance) – Nintendo logo at +4
    // Already added in previous round

    // SEGA Dreamcast VMI/VMS/DCI – no reliable universal magic

    // ══ RIFF fallback ─────────────────────────────────────────────────────
    // Only used if none of the specific RIFF sub-types matched.
    // Produces generic ".riff" files – better than missing them.
    m_sigs.push_back(mk(L"riff",  "52494646",           nullptr, 0, 500));

    RegisterUiTypeSignatureSupplement(*this);

    // ── PhotoRec 7.2 declarative alignment ────────────────────────────────
    // Third-party formats PhotoRec can carve but we had no magic for.  Only
    // its declarative register_header_check() layer is reproduced (generated
    // from src/file_*.c by tools/gen_photorec_signatures.py); magics shorter
    // than 3 bytes are excluded because their random-data match density makes
    // carving unusable.  Appended AFTER the built-in table so every existing
    // signature keeps its lower pattern id and therefore wins ties.
    phoenix::RegisterPhotoRecAlignSignatures(*this);

    LOG_INFO(L"SignatureDB: loaded " + std::to_wstring(m_sigs.size()) + L" built-in signatures");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test-only impl switch
SignatureDB::MatchImpl SignatureDB::s_matchImpl = SignatureDB::MatchImpl::AC;

void SignatureDB::SetMatchImplForTesting(MatchImpl m) { s_matchImpl = m; }

// ─────────────────────────────────────────────────────────────────────────────
void SignatureDB::EnsureMatcher() const
{
    if (m_matcherBuilt) return;
    std::vector<phoenix::MatchPattern> pats;
    pats.reserve(m_sigs.size());
    m_matcherHasOmitted = false;
    for (int i = 0; i < static_cast<int>(m_sigs.size()); ++i) {
        const FileSignature& s = m_sigs[i];
        if (s.header.empty()) { m_matcherHasOmitted = true; continue; }
        phoenix::MatchPattern p;
        p.extension        = s.extension;
        p.header           = s.header;
        p.headerOffset     = s.headerOffset;
        p.secondary        = s.secondary;
        p.secondaryOffset  = s.secondaryOffset;
        p.footer           = s.footer;
        p.maxSize          = s.maxSize;
        p.priority         = i;          // preserve DB order as tie-break
        pats.push_back(std::move(p));
    }
    m_matcher.Build(std::move(pats));

    // Longest span needed to detect any header-at-headerOffset or secondary.
    m_maxScanLen = 0;
    for (const auto& s : m_sigs) {
        if (s.header.empty()) continue;
        size_t end = static_cast<size_t>(s.headerOffset) + s.header.size();
        if (end > m_maxScanLen) m_maxScanLen = end;
        if (!s.secondary.empty() && s.secondaryOffset >= 0) {
            size_t send = static_cast<size_t>(s.secondaryOffset) + s.secondary.size();
            if (send > m_maxScanLen) m_maxScanLen = send;
        }
    }
    if (m_maxScanLen == 0) m_maxScanLen = 16;

    m_matcherBuilt = true;
}

const FileSignature* SignatureDB::LinearMatch(const std::vector<FileSignature>& sigs,
                                              const uint8_t* buf, size_t bufLen)
{
    for (const auto& sig : sigs) {
        if (!MatchPattern(buf, bufLen, sig.header, sig.headerOffset))
            continue;
        if (sig.secondaryOffset >= 0 && !sig.secondary.empty()) {
            if (!MatchPattern(buf, bufLen, sig.secondary, sig.secondaryOffset))
                continue;
        }
        return &sig;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
const FileSignature* SignatureDB::Match(const uint8_t* buf,
                                        size_t bufLen) const
{
    if (s_matchImpl == MatchImpl::Linear)
        return LinearMatch(m_sigs, buf, bufLen);

    EnsureMatcher();

    if (m_matcher.PatternCount() > 0) {
        phoenix::MatchOptions opt;
        opt.alignment           = 1;
        opt.maxSize             = bufLen;
        opt.includeFooterSearch = false;
        // Point lookup: only scan the prefix needed to see a header at its
        // headerOffset (plus any secondary). Scanning the whole buffer would make
        // each call O(bufLen) and the caller's loop O(N²).
        const size_t scanLen = (bufLen < m_maxScanLen) ? bufLen : m_maxScanLen;
        m_matchScratch.clear();
        m_matcher.Scan(buf, scanLen, 0, opt, m_matchScratch);

        int best = -1;
        for (const auto& h : m_matchScratch) {
            if (h.fileStart != 0) continue;   // magic must sit at its headerOffset
            if (!h.secondaryOk) continue;     // sub-type disambiguation must hold
            const int pid = static_cast<int>(h.patternId);
            if (best < 0 || pid < best) best = pid;
        }
        if (best >= 0)
            return &m_sigs[static_cast<size_t>(best)];

        // Only signatures with empty headers were omitted from the trie.
        if (m_matcherHasOmitted)
            return LinearMatch(m_sigs, buf, bufLen);
        return nullptr;
    }

    // No trie-buildable signature at all — fall straight through to linear.
    return LinearMatch(m_sigs, buf, bufLen);
}

// ─────────────────────────────────────────────────────────────────────────────
void SignatureDB::ScanBatch(const uint8_t* buf, size_t bufLen, uint64_t baseOffset,
                            uint64_t alignment, std::vector<ScanCandidate>& out) const
{
    out.clear();

    // Faithful emulation of the legacy per-stride Match() loop. Used when the AC
    // backend is disabled (test toggle) or when no trie-buildable signature exists.
    auto linearEmulate = [&]() {
        for (size_t i = 0; i + 8 <= bufLen; i += alignment) {
            const FileSignature* sig = LinearMatch(m_sigs, buf + i, bufLen - i);
            if (sig) out.push_back({baseOffset + i, sig, true});
        }
    };

    if (s_matchImpl == MatchImpl::Linear) { linearEmulate(); return; }

    EnsureMatcher();
    if (m_matcher.PatternCount() == 0) { linearEmulate(); return; }

    // Single streaming Aho-Corasick pass over the whole buffer.
    //
    // IMPORTANT: we feed the buffer as a *self-contained local window* (the
    // caller has already prepended any cross-batch overlap, e.g. RawScan's
    // prevTail, to `buf`). We therefore pass baseOffset=0 so the matcher's
    // internal absPos stays in LOCAL buffer coordinates [0, bufLen). The
    // matcher's own `if (absPos < baseOffset) continue;` overlap-context filter
    // would otherwise discard every candidate once the real disk offset becomes
    // larger than the buffer length. We re-base to disk space only when emitting
    // (ac.push_back({baseOffset + fs, ...}) below).
    phoenix::MatchOptions opt;
    opt.alignment           = alignment;
    opt.maxSize             = bufLen;
    opt.includeFooterSearch = false;
    m_matchScratch.clear();
    auto& matcher = const_cast<phoenix::SignatureMatcher&>(m_matcher);
    matcher.Reset();
    matcher.Feed(buf, bufLen, 0, opt, m_matchScratch);
    matcher.Finish(opt, m_matchScratch);

    // Group by fileStart, keep the lowest patternId (== Match()'s tie-break),
    // and require the secondary sub-type check to hold (matches Match()).
    std::sort(m_matchScratch.begin(), m_matchScratch.end(),
              [](const phoenix::SignatureHit& a, const phoenix::SignatureHit& b) {
                  if (a.fileStart != b.fileStart) return a.fileStart < b.fileStart;
                  return a.patternId < b.patternId;
              });

    std::vector<ScanCandidate> ac;
    for (size_t k = 0; k < m_matchScratch.size(); ) {
        const uint64_t fs = m_matchScratch[k].fileStart;   // LOCAL: offset within buf[0]
        // Mirror SignatureDB::Match(): among every pattern that fired at this
        // fileStart, the winning extension is the LOWEST patternId *whose secondary
        // sub-type check actually held* (secondaryOk). A lower-priority pattern with
        // a failed secondary must NOT suppress a higher-priority one that matched.
        int bestPid = -1;
        size_t j = k;
        while (j < m_matchScratch.size() && m_matchScratch[j].fileStart == fs) {
            if (m_matchScratch[j].secondaryOk) {
                const int pid = static_cast<int>(m_matchScratch[j].patternId);
                if (bestPid < 0 || pid < bestPid) bestPid = pid;
            }
            ++j;
        }
        // Mirror the stride loop's minimum 8-byte probe window: a candidate is only
        // reported when at least 8 bytes remain from its file start. This keeps the
        // AC and linear-emulation candidate sets identical, including buffer tails.
        // The matcher reports fileStart in LOCAL buffer coordinates (it uses its own
        // overlap base, not the caller's baseOffset), so the tail filter is on `fs`
        // directly and we re-base to disk space when emitting.
        if (bestPid >= 0 && fs + 8 <= bufLen)
            ac.push_back({baseOffset + fs, &m_sigs[static_cast<size_t>(bestPid)], true});
        k = j;
    }

    if (!m_matcherHasOmitted) { out = std::move(ac); return; }

    // Some signatures (empty headers) were omitted from the trie. Merge the
    // linear emulation as a fallback, giving AC priority at each file start.
    std::vector<ScanCandidate> lin;
    for (size_t i = 0; i + 8 <= bufLen; i += alignment) {
        const FileSignature* sig = LinearMatch(m_sigs, buf + i, bufLen - i);
        if (sig) lin.push_back({baseOffset + i, sig, true});
    }
    auto byStart = [](const ScanCandidate& a, const ScanCandidate& b) {
        return a.fileStart < b.fileStart;
    };
    std::sort(lin.begin(), lin.end(), byStart);
    out = std::move(ac);
    for (const auto& l : lin) {
        bool dup = false;
        for (const auto& a : ac) if (a.fileStart == l.fileStart) { dup = true; break; }
        if (!dup) out.push_back(l);
    }
    std::sort(out.begin(), out.end(), byStart);
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> SignatureDB::HexToBytes(const std::string& hex)
{
    std::vector<uint8_t> result;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        char hi = hex[i], lo = hex[i + 1];
        auto unhex = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            return 0;
        };
        result.push_back(static_cast<uint8_t>((unhex(hi) << 4) | unhex(lo)));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load additional signatures from a JSON file (Qt-free, A1-9).
//
// Contract (unchanged): signatures are APPENDED to the built-in set; invalid
// entries are skipped with a warning; file/JSON/structure errors return -1;
// success returns the number of appended signatures.  m_matcherBuilt is cleared
// so the Aho-Corasick matcher is rebuilt lazily with the enlarged signature set.
// ─────────────────────────────────────────────────────────────────────────────
int SignatureDB::LoadFromFile(const std::wstring& path)
{
    m_matcherBuilt = false;   // signature set will change; rebuild AC matcher lazily

    const std::string utf8Path = Utils::WideToUtf8(path);
    std::ifstream in(utf8Path, std::ios::binary);
    if (!in.is_open()) {
        LOG_ERROR(L"SignatureDB::LoadFromFile – cannot open file: " + path);
        return -1;
    }
    std::string json((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // Strip a UTF-8 BOM (common in Windows-authored JSON files).
    if (json.size() >= 3 &&
        static_cast<unsigned char>(json[0]) == 0xEF &&
        static_cast<unsigned char>(json[1]) == 0xBB &&
        static_cast<unsigned char>(json[2]) == 0xBF)
        json.erase(0, 3);

    phoenix::JsonParser parser(std::move(json));
    phoenix::JsonValue  root;
    if (!parser.Parse(root)) {
        LOG_ERROR(L"SignatureDB::LoadFromFile – JSON parse error: "
                  + Utils::Utf8ToWide(parser.Error())
                  + L" at byte " + std::to_wstring(parser.ErrorPos()));
        return -1;
    }
    if (!root.IsObject()) {
        LOG_ERROR(L"SignatureDB::LoadFromFile – root element must be a JSON object");
        return -1;
    }
    const phoenix::JsonValue* sigArray = root.Find("signatures");
    if (!sigArray || !sigArray->IsArray()) {
        LOG_ERROR(L"SignatureDB::LoadFromFile – missing or invalid \"signatures\" array");
        return -1;
    }

    auto trim = [](const std::string& s) -> std::string {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    };
    // Per SIGNATURE_FORMAT.md: hex strings must be even-length and contain only
    // 0-9 / A-F / a-f.  The old Qt path silently fed invalid characters through
    // HexToBytes() (which maps them to 0), producing garbage matchers.
    auto validHex = [](const std::string& s) -> bool {
        if (s.empty() || (s.size() % 2) != 0) return false;
        for (char c : s) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    };
    auto fieldString = [&](const phoenix::JsonValue& obj, const char* key) -> std::string {
        const phoenix::JsonValue* v = obj.Find(key);
        return v ? trim(v->AsString()) : std::string();
    };
    auto fieldInt = [&](const phoenix::JsonValue& obj, const char* key, int def) -> int {
        const phoenix::JsonValue* v = obj.Find(key);
        return (v && v->IsNumber()) ? v->AsInt(def) : def;
    };

    constexpr double kDefaultMaxSize = 50.0 * 1024.0 * 1024.0;
    int loaded = 0;
    const auto& items = sigArray->array;

    for (size_t i = 0; i < items.size(); ++i) {
        const phoenix::JsonValue& obj = items[i];
        if (!obj.IsObject()) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] is not an object, skipped");
            continue;
        }

        const std::string ext   = fieldString(obj, "extension");
        const std::string magic = fieldString(obj, "magic");
        if (ext.empty() || magic.empty()) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] missing extension or magic, skipped");
            continue;
        }
        if (!validHex(magic)) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] invalid magic hex (odd length or non-hex char), skipped");
            continue;
        }
        const std::string footer = fieldString(obj, "footer");
        if (!footer.empty() && !validHex(footer)) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] invalid footer hex, skipped");
            continue;
        }
        const std::string sec = fieldString(obj, "secondary_magic");
        if (!sec.empty() && !validHex(sec)) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] invalid secondary_magic hex, skipped");
            continue;
        }

        FileSignature sig;
        sig.extension    = Utils::Utf8ToWide(ext);
        sig.header       = HexToBytes(magic);
        sig.headerOffset = fieldInt(obj, "magic_offset", 0);
        if (!footer.empty()) sig.footer = HexToBytes(footer);

        const phoenix::JsonValue* maxV = obj.Find("max_size");
        double maxSize = (maxV && maxV->IsNumber())
                             ? maxV->AsNumber(kDefaultMaxSize) : kDefaultMaxSize;
        if (!(maxSize > 0.0)) maxSize = kDefaultMaxSize;   // guard NaN / <= 0
        sig.maxSize = static_cast<uint64_t>(maxSize);

        if (!sec.empty()) {
            sig.secondary       = HexToBytes(sec);
            sig.secondaryOffset = fieldInt(obj, "secondary_offset", -1);
        }

        if (sig.header.empty()) {
            LOG_WARN(L"SignatureDB::LoadFromFile – signatures[" + std::to_wstring(i)
                     + L"] magic hex decoded to empty, skipped");
            continue;
        }

        m_sigs.push_back(std::move(sig));
        ++loaded;
    }

    LOG_INFO(L"SignatureDB::LoadFromFile – loaded " + std::to_wstring(loaded)
             + L" custom signature(s) from " + path);
    return loaded;
}
