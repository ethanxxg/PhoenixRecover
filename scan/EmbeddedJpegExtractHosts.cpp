#include "EmbeddedJpegExtract.h"
#include "../recover/RecoverEngine.h"

#include <algorithm>
#include <cwctype>

namespace {

bool ShouldScanHost(const FileNode& host)
{
    if (host.isDirectory || host.size == 0)
        return false;
    if (host.originalPath.find(L"$EMBED\\") != std::wstring::npos)
        return false;

    if (host.name.find(L':') != std::wstring::npos)
        return true;

    std::wstring ext = host.extension;
    for (wchar_t& c : ext)
        c = static_cast<wchar_t>(towlower(c));

    if (ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" || ext == L"bmp" ||
        ext == L"webp" || ext == L"tif" || ext == L"tiff")
        return false;

    return true;
}

} // namespace

std::vector<FileNode> EmbeddedJpegExtract::ExpandFromHosts(
    SectorReader& reader,
    const Partition& partition,
    const std::vector<FileNode>& hosts,
    uint64_t& nextId,
    uint64_t maxHostBytes)
{
    std::vector<FileNode> extras;
    extras.reserve(16);

    for (const FileNode& host : hosts) {
        if (!ShouldScanHost(host) || host.size > maxHostBytes)
            continue;

        const uint32_t toRead = static_cast<uint32_t>(
            (std::min)(host.size, maxHostBytes));
        std::vector<uint8_t> buf;
        if (!RecoverEngine::ReadFileDataPrefix(reader, partition, host, toRead, buf))
            continue;

        std::vector<EmbeddedJpegSlice> slices;
        CollectFromBuffer(buf.data(), buf.size(), slices);

        int index = 0;
        for (const EmbeddedJpegSlice& slice : slices) {
            if (slice.bytes.size() < 4)
                continue;

            FileNode node;
            node.id             = nextId++;
            node.parentId       = host.id;
            node.name           = host.name + L"_embed_" + std::to_wstring(index) + L".jpg";
            node.extension      = L"jpg";
            node.originalPath   = host.originalPath + L"\\$EMBED\\";
            if (!slice.innerPath.empty())
                node.originalPath += slice.innerPath;
            else
                node.originalPath += node.name;
            node.size           = slice.bytes.size();
            node.rawSize        = slice.bytes.size();
            node.residentData   = slice.bytes;
            node.rawOffset      = host.rawOffset + slice.offsetInHost;
            node.status         = host.status;
            node.fsType         = host.fsType;
            node.recoverability = RecoverabilityLevel::Good;
            ++index;
            extras.push_back(std::move(node));
        }
    }

    return extras;
}
