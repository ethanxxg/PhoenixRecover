#include "EmbeddedJpegExtract.h"
#include "CfbReader.h"
#include "third_party/miniz_tinfl.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstring>

namespace {

inline uint16_t LE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t LE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool InflateRawDeflate(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out)
{
    if (!in || inLen == 0)
        return false;

    size_t outLen = 0;
    void* heap =
        tinfl_decompress_mem_to_heap(in, inLen, &outLen, 0);
    if (!heap || outLen == 0) {
        if (heap)
            mz_free(heap);
        return false;
    }
    out.assign(static_cast<uint8_t*>(heap),
               static_cast<uint8_t*>(heap) + outLen);
    mz_free(heap);
    return true;
}

bool GzipDeflatePayload(const uint8_t* in, size_t inLen, const uint8_t** payload,
                        size_t* payloadLen)
{
    if (!in || inLen < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8)
        return false;

    size_t pos = 10;
    const uint8_t flg = in[3];
    if (flg & 4) {
        if (pos + 2 > inLen)
            return false;
        const uint16_t xlen = LE16(in + pos);
        pos += 2 + xlen;
    }
    if (flg & 8) {
        while (pos < inLen && in[pos] != 0)
            ++pos;
        if (pos >= inLen)
            return false;
        ++pos;
    }
    if (flg & 16) {
        while (pos < inLen && in[pos] != 0)
            ++pos;
        if (pos >= inLen)
            return false;
        ++pos;
    }
    if (flg & 2)
        pos += 2;
    if (pos + 8 >= inLen)
        return false;

    *payload    = in + pos;
    *payloadLen = inLen - pos - 8;
    return *payloadLen > 0;
}

bool InflateGzip(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out)
{
    const uint8_t* payload = nullptr;
    size_t payloadLen      = 0;
    if (!GzipDeflatePayload(in, inLen, &payload, &payloadLen))
        return false;
    return InflateRawDeflate(payload, payloadLen, out);
}

bool InflateZipDeflate(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out)
{
    return InflateRawDeflate(in, inLen, out);
}

void AppendUniqueSlice(std::vector<EmbeddedJpegSlice>& out,
                       uint64_t offsetInHost,
                       const uint8_t* data, size_t len,
                       const std::wstring& innerPath = L"")
{
    if (!data || len < 4)
        return;
    for (const EmbeddedJpegSlice& existing : out) {
        if (existing.offsetInHost == offsetInHost && existing.bytes.size() == len &&
            (len == 0 || memcmp(existing.bytes.data(), data, len) == 0))
            return;
    }
    EmbeddedJpegSlice slice;
    slice.offsetInHost = offsetInHost;
    slice.bytes.assign(data, data + len);
    slice.innerPath    = innerPath;
    out.push_back(std::move(slice));
}

void ScanJpegInBuffer(const uint8_t* data, size_t len, uint64_t baseOffset,
                      const std::wstring& innerPath,
                      std::vector<EmbeddedJpegSlice>& out)
{
    if (!data || len < 4)
        return;
    for (size_t i = 0; i + 4 <= len; ++i) {
        if (data[i] != 0xFF || data[i + 1] != 0xD8 || data[i + 2] != 0xFF)
            continue;
        for (size_t j = i + 2; j + 1 < len; ++j) {
            if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                AppendUniqueSlice(out, baseOffset + i, data + i, j + 2 - i, innerPath);
                i = j + 1;
                break;
            }
        }
    }
}

bool FindZipEocd(const uint8_t* data, size_t len, size_t& eocdOff)
{
    if (len < 22)
        return false;
    size_t i = len - 22;
    for (;;) {
        if (data[i] == 0x50 && data[i + 1] == 0x4B && data[i + 2] == 0x05 &&
            data[i + 3] == 0x06) {
            eocdOff = i;
            return true;
        }
        if (i == 0)
            break;
        --i;
    }
    return false;
}

std::wstring BytesToWide(const uint8_t* p, size_t n)
{
    std::wstring s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i)
        s.push_back(static_cast<wchar_t>(p[i]));
    return s;
}

void ScanZipPayload(const uint8_t* payload, size_t payloadLen, uint16_t method,
                    uint64_t payloadOffset, const std::wstring& entryName,
                    std::vector<EmbeddedJpegSlice>& out)
{
    if (method == 0) {
        ScanJpegInBuffer(payload, payloadLen, payloadOffset, entryName, out);
        return;
    }
    if (method != 8)
        return;

    std::vector<uint8_t> inflated;
    if (InflateZipDeflate(payload, payloadLen, inflated)) {
        ScanJpegInBuffer(inflated.data(), inflated.size(), payloadOffset, entryName, out);
        return;
    }
    size_t outLen = 0;
    void* heap = tinfl_decompress_mem_to_heap(payload, payloadLen, &outLen,
                                              TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (heap && outLen > 0) {
        ScanJpegInBuffer(static_cast<uint8_t*>(heap), outLen, payloadOffset, entryName, out);
        mz_free(heap);
    }
}

void ScanZipViaCentralDirectory(const uint8_t* data, size_t len, uint64_t baseOffset,
                                std::vector<EmbeddedJpegSlice>& out)
{
    size_t eocdOff = 0;
    if (!FindZipEocd(data, len, eocdOff))
        return;

    const uint8_t* eocd = data + eocdOff;
    const uint16_t entriesTotal = LE16(eocd + 10);
    const uint32_t cdSize       = LE32(eocd + 12);
    const uint32_t cdOff        = LE32(eocd + 16);
    if (entriesTotal == 0 || cdSize == 0 || cdOff >= eocdOff || cdOff + cdSize > eocdOff)
        return;

    const uint8_t* cd    = data + cdOff;
    size_t pos           = 0;
    uint16_t parsed      = 0;
    while (pos + 46 <= cdSize && parsed < entriesTotal) {
        if (cd[pos] != 0x50 || cd[pos + 1] != 0x4B || cd[pos + 2] != 0x01 ||
            cd[pos + 3] != 0x02)
            break;

        const uint16_t method    = LE16(cd + pos + 10);
        const uint32_t compSize  = LE32(cd + pos + 20);
        const uint16_t nameLen   = LE16(cd + pos + 28);
        const uint16_t extraLen  = LE16(cd + pos + 30);
        const uint16_t commentLen = LE16(cd + pos + 32);
        const uint32_t localOff  = LE32(cd + pos + 42);
        const size_t nameOff     = pos + 46;
        const size_t nextPos     = nameOff + nameLen + extraLen + commentLen;
        if (nextPos > cdSize)
            break;

        const std::wstring entryName = BytesToWide(cd + nameOff, nameLen);
        pos                          = nextPos;
        ++parsed;

        if (localOff + 30 > len || compSize == 0)
            continue;
        const uint8_t* lh = data + localOff;
        if (lh[0] != 0x50 || lh[1] != 0x4B || lh[2] != 0x03 || lh[3] != 0x04)
            continue;

        const uint16_t localNameLen = LE16(lh + 26);
        const uint16_t localExtraLen = LE16(lh + 28);
        const size_t payloadOff = localOff + 30 + localNameLen + localExtraLen;
        if (payloadOff + compSize > len)
            continue;

        ScanZipPayload(data + payloadOff, compSize, method, baseOffset + payloadOff, entryName,
                       out);
    }
}

void ScanZipLocalEntries(const uint8_t* data, size_t len, uint64_t baseOffset,
                         std::vector<EmbeddedJpegSlice>& out)
{
    for (size_t i = 0; i + 30 <= len; ++i) {
        if (data[i] != 0x50 || data[i + 1] != 0x4B || data[i + 2] != 0x03 ||
            data[i + 3] != 0x04)
            continue;

        const uint16_t gpFlags  = LE16(data + i + 6);
        const uint16_t method   = LE16(data + i + 8);
        uint32_t compSize       = LE32(data + i + 18);
        const uint16_t nameLen  = LE16(data + i + 26);
        const uint16_t extraLen = LE16(data + i + 28);
        const size_t hdrEnd     = i + 30 + nameLen + extraLen;
        if (hdrEnd > len)
            continue;

        const std::wstring entryName = BytesToWide(data + i + 30, nameLen);

        if ((gpFlags & 0x08) != 0 || compSize == 0) {
            // Sizes live in the central directory / data descriptor.
            continue;
        }
        if (compSize > len - hdrEnd)
            continue;

        ScanZipPayload(data + hdrEnd, compSize, method, baseOffset + hdrEnd, entryName, out);
        i = hdrEnd + compSize - 1;
    }
}

uint64_t ParseTarOctal(const uint8_t* field, size_t fieldLen)
{
    uint64_t v = 0;
    for (size_t i = 0; i < fieldLen; ++i) {
        const uint8_t c = field[i];
        if (c == 0 || c == ' ')
            break;
        if (c < '0' || c > '7')
            return 0;
        v = (v << 3) + (c - '0');
    }
    return v;
}

void ScanTarMembers(const uint8_t* data, size_t len, uint64_t baseOffset,
                    std::vector<EmbeddedJpegSlice>& out)
{
    size_t off = 0;
    while (off + 512 <= len) {
        bool allZero = true;
        for (size_t z = 0; z < 512; ++z) {
            if (data[off + z] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero)
            break;

        const uint8_t typeFlag = (off + 156 < len) ? data[off + 156] : 0;
        const uint64_t fileSize =
            ParseTarOctal(data + off + 124, 12);
        std::wstring memberName;
        for (size_t n = 0; n < 100 && data[off + n] != 0; ++n)
            memberName.push_back(
                static_cast<wchar_t>(static_cast<unsigned char>(data[off + n])));

        const size_t dataOff = off + 512;
        if ((typeFlag == 0 || typeFlag == '0' || typeFlag == '7') && fileSize > 0 &&
            dataOff + fileSize <= len)
        {
            ScanJpegInBuffer(data + dataOff, static_cast<size_t>(fileSize),
                             baseOffset + dataOff, memberName, out);
        }

        const size_t padded = ((static_cast<size_t>(fileSize) + 511) / 512) * 512;
        off = dataOff + padded;
    }
}

void CollectFromBufferRecursive(const uint8_t* data, size_t len, uint64_t baseOffset,
                                const std::wstring& innerPrefix, int depth,
                                std::vector<EmbeddedJpegSlice>& out);

void ScanCfbStreams(const uint8_t* data, size_t len, uint64_t baseOffset,
                    const std::wstring& innerPrefix, int depth,
                    std::vector<EmbeddedJpegSlice>& out)
{
    if (!data || len < 512 || depth > 4)
        return;

    CfbReader::EnumerateStreams(data, len, [&](const CfbReader::StreamInfo& stream) {
        if (!stream.data || stream.size < 4)
            return;

        std::wstring path = innerPrefix;
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
            path += L"\\";
        path += stream.name;

        ScanJpegInBuffer(stream.data, stream.size, baseOffset + stream.offsetInFile, path, out);
        CollectFromBufferRecursive(stream.data, stream.size, baseOffset + stream.offsetInFile, path,
                                   depth + 1, out);
    });
}

void CollectFromBufferRecursive(const uint8_t* data, size_t len, uint64_t baseOffset,
                                const std::wstring& innerPrefix, int depth,
                                std::vector<EmbeddedJpegSlice>& out)
{
    if (!data || len < 4 || depth > 4)
        return;

    ScanJpegInBuffer(data, len, baseOffset, innerPrefix, out);
    ScanZipViaCentralDirectory(data, len, baseOffset, out);
    ScanZipLocalEntries(data, len, baseOffset, out);
    ScanCfbStreams(data, len, baseOffset, innerPrefix, depth, out);

    if (len >= 2 && data[0] == 0x1F && data[1] == 0x8B) {
        std::vector<uint8_t> inflated;
        if (InflateGzip(data, len, inflated)) {
            CollectFromBufferRecursive(inflated.data(), inflated.size(), baseOffset, innerPrefix,
                                       depth + 1, out);
            ScanTarMembers(inflated.data(), inflated.size(), baseOffset, out);
        }
    }
}

} // namespace

void EmbeddedJpegExtract::CollectFromBuffer(const uint8_t* data, size_t len,
                                            std::vector<EmbeddedJpegSlice>& out)
{
    CollectFromBufferRecursive(data, len, 0, L"", 0, out);
}
