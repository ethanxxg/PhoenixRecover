#include "FormatPayloadScore.h"

#include <algorithm>
#include <cstring>

namespace {

static std::wstring ToLowerExt(std::wstring ext)
{
    for (auto& c : ext) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return ext;
}

static bool TailHas(const uint8_t* data, size_t len, const uint8_t* pat, size_t patLen)
{
    if (len < patLen) return false;
    const size_t tailScan = std::min<size_t>(len, 65536);
    const size_t start = len - tailScan;
    for (size_t i = start; i + patLen <= len; ++i) {
        if (memcmp(data + i, pat, patLen) == 0) return true;
    }
    return false;
}

static FormatPayloadScore ScoreJpeg(const uint8_t* data, size_t len)
{
    if (len < 4) return FormatPayloadScore::Weak;
    if (data[0] != 0xFF || data[1] != 0xD8) return FormatPayloadScore::Invalid;

    // Light segment walk on first 64 KiB
    const size_t scanLen = std::min<size_t>(len, 65536);
    size_t p = 2;
    while (p + 1 < scanLen) {
        if (data[p] != 0xFF) break;
        const uint8_t marker = data[p + 1];
        if (marker == 0xD9) {
            return (p + 2 >= len || p + 2 == len) ? FormatPayloadScore::Complete
                                                  : FormatPayloadScore::Partial;
        }
        if (marker == 0xD8) return FormatPayloadScore::Invalid;
        if (p + 4 > scanLen) break;
        const uint16_t segLen = static_cast<uint16_t>((data[p + 2] << 8) | data[p + 3]);
        if (segLen < 2 || p + segLen > scanLen) return FormatPayloadScore::Partial;
        p += segLen;
    }

    static const uint8_t eoi[2] = { 0xFF, 0xD9 };
    if (TailHas(data, len, eoi, 2)) return FormatPayloadScore::Complete;
    return FormatPayloadScore::Partial;
}

static FormatPayloadScore ScorePng(const uint8_t* data, size_t len)
{
    if (len < 24) return FormatPayloadScore::Weak;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(data, sig, 8) != 0) return FormatPayloadScore::Invalid;

    const uint32_t ihdrLen =
        (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) |
        (static_cast<uint32_t>(data[10]) << 8) | data[11];
    if (ihdrLen != 13 || memcmp(data + 12, "IHDR", 4) != 0) return FormatPayloadScore::Invalid;

    const uint32_t w =
        (static_cast<uint32_t>(data[16]) << 24) | (static_cast<uint32_t>(data[17]) << 16) |
        (static_cast<uint32_t>(data[18]) << 8) | data[19];
    const uint32_t h =
        (static_cast<uint32_t>(data[20]) << 24) | (static_cast<uint32_t>(data[21]) << 16) |
        (static_cast<uint32_t>(data[22]) << 8) | data[23];
    if (w == 0 || h == 0 || w > 100000 || h > 100000) return FormatPayloadScore::Invalid;

    static const uint8_t iend[4] = { 'I', 'E', 'N', 'D' };
    if (TailHas(data, len, iend, 4)) return FormatPayloadScore::Complete;
    return FormatPayloadScore::Partial;
}

// CFReDS DFR text files: "\nDFR\n" header, '+' filler, filename near EOF.
static FormatPayloadScore ScoreDfrTxt(const uint8_t* data, size_t len)
{
    if (len < 8) return FormatPayloadScore::Weak;
    if (memcmp(data, "\nDFR\n", 5) != 0) return FormatPayloadScore::Invalid;

    static const uint8_t hdr[] = "\nDFR\n";
    if (!TailHas(data, len, hdr, 5)) {
        // Single-cluster files repeat the DFR header only at start.
        if (len >= 512) {
            const size_t tailScan = std::min<size_t>(len, 512);
            bool sawPlus = false;
            for (size_t i = 5; i + 1 < tailScan; ++i) {
                if (data[i] == '+') { sawPlus = true; break; }
            }
            if (!sawPlus) return FormatPayloadScore::Partial;
        }
    }

    // Whole-file marker: printable name + optional NUL in last 256 bytes.
    if (len >= 64) {
        const size_t tailStart = len > 256 ? len - 256 : 0;
        bool hasNameChar = false;
        for (size_t i = tailStart; i < len; ++i) {
            const uint8_t c = data[i];
            if (c >= 0x20 && c <= 0x7E) hasNameChar = true;
            if (c == 0 && hasNameChar) return FormatPayloadScore::Complete;
        }
        if (hasNameChar && data[len - 1] == 0)
            return FormatPayloadScore::Complete;
    }

    return FormatPayloadScore::Partial;
}

static FormatPayloadScore ScorePdf(const uint8_t* data, size_t len)
{
    if (len < 5) return FormatPayloadScore::Weak;
    if (memcmp(data, "%PDF-", 5) != 0) return FormatPayloadScore::Invalid;
    static const uint8_t eof[5] = { '%', '%', 'E', 'O', 'F' };
    if (TailHas(data, len, eof, 5)) return FormatPayloadScore::Complete;
    return FormatPayloadScore::Partial;
}

static FormatPayloadScore ScoreZipLike(const uint8_t* data, size_t len)
{
    if (len < 22) return FormatPayloadScore::Weak;
    if (data[0] != 'P' || data[1] != 'K' || data[2] != 0x03 || data[3] != 0x04)
        return FormatPayloadScore::Invalid;

    static const uint8_t eocd[4] = { 'P', 'K', 0x05, 0x06 };
    const size_t tailScan = std::min<size_t>(len, 65536);
    const size_t start = len - tailScan;
    for (size_t i = start; i + 22 <= len; ++i) {
        if (memcmp(data + i, eocd, 4) != 0) continue;
        const uint32_t cdSize =
            data[i + 12] | (data[i + 13] << 8) | (data[i + 14] << 16) | (data[i + 15] << 24);
        const uint32_t cdOff =
            data[i + 16] | (data[i + 17] << 8) | (data[i + 18] << 16) | (data[i + 19] << 24);
        if (static_cast<uint64_t>(cdOff) + cdSize <= len + 0x10000)
            return FormatPayloadScore::Complete;
        return FormatPayloadScore::Partial;
    }
    return FormatPayloadScore::Partial;
}

static FormatPayloadScore ScoreMp4Like(const uint8_t* data, size_t len)
{
    if (len < 12) return FormatPayloadScore::Weak;
    const uint32_t boxSize =
        (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) | data[3];
    if (boxSize < 8) return FormatPayloadScore::Invalid;
    if (memcmp(data + 4, "ftyp", 4) != 0 && memcmp(data + 4, "moov", 4) != 0 &&
        memcmp(data + 4, "mdat", 4) != 0)
        return FormatPayloadScore::Invalid;

    // mdat/moov present and size plausible
    if (boxSize <= len) return FormatPayloadScore::Partial;
    return FormatPayloadScore::Weak;
}

static FormatPayloadScore ScoreGif(const uint8_t* data, size_t len)
{
    if (len < 13) return FormatPayloadScore::Weak;
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)
        return FormatPayloadScore::Invalid;
    if (data[len - 1] == 0x3B) return FormatPayloadScore::Complete;
    return FormatPayloadScore::Partial;
}

} // namespace

namespace FormatPayloadScoreUtil {

FormatPayloadScore Score(const std::wstring& extensionLower,
                         const uint8_t* data,
                         size_t length)
{
    if (!data || length == 0) return FormatPayloadScore::Weak;

    const std::wstring ext = ToLowerExt(extensionLower);
    if (ext == L"jpg" || ext == L"jpeg") return ScoreJpeg(data, length);
    if (ext == L"png") return ScorePng(data, length);
    if (ext == L"pdf") return ScorePdf(data, length);
    if (ext == L"gif") return ScoreGif(data, length);
    if (ext == L"mp4" || ext == L"mov" || ext == L"m4v" || ext == L"3gp")
        return ScoreMp4Like(data, length);
    if (ext == L"zip" || ext == L"docx" || ext == L"xlsx" || ext == L"pptx" ||
        ext == L"odt" || ext == L"ods" || ext == L"odp" || ext == L"epub" ||
        ext == L"apk" || ext == L"jar" || ext == L"whl")
        return ScoreZipLike(data, length);
    if (ext == L"txt") return ScoreDfrTxt(data, length);

    return FormatPayloadScore::Weak;
}

bool SupportsFragmentStitch(const std::wstring& extensionLower)
{
    const std::wstring ext = ToLowerExt(extensionLower);
    return ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"pdf" ||
           ext == L"gif" || ext == L"mp4" || ext == L"mov" || ext == L"m4v" ||
           ext == L"3gp" || ext == L"zip" || ext == L"docx" || ext == L"xlsx" ||
           ext == L"pptx" || ext == L"odt" || ext == L"ods" || ext == L"odp" ||
           ext == L"epub" || ext == L"txt";
}

std::wstring ProbeExtension(const uint8_t* data, size_t length)
{
    if (!data || length < 4) return L"";
    if (data[0] == 0xFF && data[1] == 0xD8) return L"jpg";
    if (length >= 8 && data[0] == 0x89 && data[1] == 'P') return L"png";
    if (length >= 5 && memcmp(data, "%PDF", 4) == 0) return L"pdf";
    if (data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) return L"zip";
    if (length >= 6 && memcmp(data, "GIF8", 4) == 0) return L"gif";
    if (length >= 12 && memcmp(data + 4, "ftyp", 4) == 0) return L"mp4";
    if (length >= 5 && memcmp(data, "\nDFR\n", 5) == 0) return L"txt";
    return L"";
}

} // namespace FormatPayloadScoreUtil
