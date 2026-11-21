#include "RefsParser.h"

#include "../../common/Utils.h"
#include "../../log/Logger.h"

#include <algorithm>
#include <cstring>
#include <set>

using phoenix::refs::RefsDirEntry;
using phoenix::refs::RefsExtent;
using phoenix::refs::RefsStatus;
using phoenix::refs::RefsVolume;

// ---------------------------------------------------------------------------
// ReaderImage – bridges the product's SectorReader to the engine's
// IRefsImage abstraction.  Read-only by construction: the interface has no
// write method, so this adapter cannot damage the scan source.
// ---------------------------------------------------------------------------
class RefsParser::ReaderImage : public phoenix::refs::IRefsImage
{
public:
    ReaderImage(SectorReader& reader, uint64_t startByte, uint64_t size)
        : m_reader(&reader), m_start(startByte), m_size(size) {}

    uint64_t Size() const override { return m_size; }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (length == 0) return true;
        if (offset > m_size || length > m_size - offset) return false;
        return m_reader->ReadBytes(m_start + offset, dst, length);
    }

private:
    SectorReader* m_reader = nullptr;
    uint64_t      m_start  = 0;
    uint64_t      m_size   = 0;
};

RefsParser::RefsParser() = default;
RefsParser::~RefsParser() = default;

bool RefsParser::Open(SectorReader& reader, const Partition& partition)
{
    m_reader = &reader;
    m_part   = partition;
    m_valid  = false;
    m_files.clear();
    m_totalEntries = 0;
    m_inUseCount   = 0;
    m_nextId       = 1;
    m_lastError.clear();

    const uint32_t ss = reader.GetSectorSize();
    if (ss == 0) return false;

    const uint64_t startByte = partition.startSector * ss;
    uint64_t volumeBytes = partition.sizeBytes;
    if (volumeBytes == 0 && partition.totalSectors > 0)
        volumeBytes = partition.totalSectors * ss;

    // Header-only engine needs a sized byte source; without a size we cannot
    // bound reads, so refuse rather than guessing.
    if (volumeBytes == 0) {
        m_lastError = L"ReFS volume size unknown";
        return false;
    }

    m_image  = std::unique_ptr<ReaderImage>(new ReaderImage(reader, startByte, volumeBytes));
    m_volume = std::unique_ptr<RefsVolume>(new RefsVolume());

    const RefsStatus st = m_volume->Open(*m_image);
    if (st != RefsStatus::Ok || !m_volume->IsOpen()) {
        const std::string err = m_volume->LastError();
        m_lastError = L"ReFS open failed: " +
                      std::wstring(phoenix::refs::RefsStatusName(st),
                                   phoenix::refs::RefsStatusName(st) +
                                   std::strlen(phoenix::refs::RefsStatusName(st)));
        if (!err.empty())
            m_lastError += L" (" + Utils::Utf8ToWide(err) + L")";
        m_volume.reset();
        m_image.reset();
        return false;
    }

    m_valid = true;
    LOG_INFO(L"RefsParser::Open ok – volume " +
             std::to_wstring(m_volume->Info().geometry.clusterSize) + L" B clusters, " +
             std::to_wstring(m_volume->Info().geometry.totalClusters) + L" clusters");
    return true;
}

bool RefsParser::BuildNode(const RefsDirEntry& entry, uint64_t parentNodeId,
                           const std::wstring& path, FileNode& out,
                           bool deleted)
{
    out = FileNode{};
    out.id           = m_nextId++;
    out.parentId     = parentNodeId;
    out.name         = entry.name;
    out.originalPath = path + entry.name;
    out.isDirectory  = entry.isDirectory;
    out.size         = entry.size;
    out.createTime   = entry.createTime;
    out.modifyTime   = entry.modifyTime;
    out.accessTime   = entry.accessTime;
    out.status       = deleted ? FileStatus::Deleted : FileStatus::Normal;
    out.fsType       = L"ReFS";

    const size_t dot = entry.name.rfind(L'.');
    if (dot != std::wstring::npos && dot + 1 < entry.name.size()) {
        out.extension = entry.name.substr(dot + 1);
        for (wchar_t& c : out.extension)
            c = static_cast<wchar_t>(towlower(c));
    }

    if (out.isDirectory) {
        out.originalPath += L'\\';
        out.recoverability = RecoverabilityLevel::Excellent;
        return true;
    }

    if (!entry.dataAvailable) {
        // Split backing record (A1-5b) or an undecodable $DATA sub-record:
        // report the name but never pretend the content is recoverable.
        out.recoverability = RecoverabilityLevel::Lost;
        return true;
    }

    if (entry.dataInline) {
        const uint64_t take = (std::min)(static_cast<uint64_t>(entry.inlineData.size()),
                                        entry.size);
        out.residentData.assign(entry.inlineData.begin(),
                                entry.inlineData.begin() + static_cast<std::ptrdiff_t>(take));
        out.size = entry.size;
        out.recoverability = RecoverabilityLevel::Excellent;
        return true;
    }

    // Extents: vcn order is the file's logical order; VLCN must be translated
    // through the container table to a physical cluster number.
    std::vector<RefsExtent> extents = entry.extents;
    std::sort(extents.begin(), extents.end(),
              [](const RefsExtent& a, const RefsExtent& b) { return a.vcn < b.vcn; });

    bool translatedAll = true;
    for (const RefsExtent& ex : extents) {
        uint64_t plcn = 0;
        if (!m_volume->TranslateVlcn(ex.vlcn, plcn)) {
            translatedAll = false;
            continue;
        }
        for (uint32_t i = 0; i < ex.length; ++i)
            out.clusters.push_back(plcn + i);
    }

    if (out.clusters.empty())
        out.recoverability = RecoverabilityLevel::Lost;
    else if (translatedAll)
        out.recoverability = RecoverabilityLevel::Excellent;
    else
        out.recoverability = RecoverabilityLevel::Poor;

    return true;
}

void RefsParser::EnumerateDirectory(uint64_t objectId, uint64_t parentNodeId,
                                    const std::wstring& path, int depth,
                                    std::atomic<bool>* cancelFlag)
{
    if (depth > 64) return;

    const std::vector<RefsDirEntry> entries = m_volume->ListDirectory(objectId);
    if (entries.empty())
        return;

    // A directory entry keeps the name, metadata and an entry ordinal, but the
    // child object id lives only in the parent-child table.  Rebuild the tree
    // from that table and bind names to ids in B+-tree order.
    std::vector<uint64_t> children;
    for (const auto& link : m_volume->ParentChildLinks()) {
        if (link.first != objectId) continue;
        if (link.second == objectId || link.second == 0) continue;
        if (std::find(children.begin(), children.end(), link.second) == children.end())
            children.push_back(link.second);
    }

    size_t dirCount = 0;
    for (const RefsDirEntry& e : entries)
        if (e.isDirectory) ++dirCount;

    // A named directory may own extra backing objects (index or stream
    // aliases) that the name table never mentions, so the child table can
    // declare more objects than there are directory names.  In that case drop
    // the objects whose B+-tree is empty: an object with no rows cannot be the
    // named directory, whereas the alias rows are always empty.  This is only
    // applied when it makes the counts agree, so an empty real subdirectory is
    // never discarded by accident.
    if (children.size() > dirCount) {
        std::vector<uint64_t> nonEmpty;
        nonEmpty.reserve(children.size());
        for (uint64_t c : children) {
            if (!m_volume->ListDirectory(c).empty()) nonEmpty.push_back(c);
        }
        if (nonEmpty.size() == dirCount)
            children.swap(nonEmpty);
    }

    std::vector<std::pair<uint64_t, std::pair<uint64_t, std::wstring>>> subdirs;
    size_t childCursor = 0;

    for (const RefsDirEntry& entry : entries) {
        if (cancelFlag && cancelFlag->load()) return;
        if (entry.name.empty() || entry.name == L"." || entry.name == L"..")
            continue;

        ++m_totalEntries;
        FileNode node;
        BuildNode(entry, parentNodeId, path, node);
        if (node.status == FileStatus::Normal) ++m_inUseCount;

        const uint64_t nodeId = node.id;
        const bool     isDir  = node.isDirectory;
        m_files.push_back(node);
        if (m_onFileFound) m_onFileFound(node);

        if (!isDir || childCursor >= children.size())
            continue;

        const uint64_t childOid = children[childCursor++];
        const std::wstring childPath = path + entry.name + L"\\";
        subdirs.emplace_back(childOid, std::make_pair(nodeId, childPath));
    }

    for (const auto& sub : subdirs)
        EnumerateDirectory(sub.first, sub.second.first, sub.second.second,
                           depth + 1, cancelFlag);
}

bool RefsParser::ScanMetadata(std::function<void(int)> onProgress,
                              std::atomic<bool>* cancelFlag)
{
    if (!m_valid || !m_volume) return false;

    EnumerateDirectory(RefsVolume::kRootDirectoryOid, 0, L"\\", 0, cancelFlag);

    // Deleted residue: rows that survive in MSB+ pages unreachable from the
    // live checkpoint roots.  Reuse BuildNode so name/extension/timestamps,
    // extent translation and resident-data handling stay identical to live
    // files; only the status differs (Deleted, never counted as in-use).
    m_volume->ForEachResidualDirRow(
        [&](uint64_t tableId, const uint8_t* key, size_t keyLen,
            const uint8_t* val, size_t valLen) {
            if (cancelFlag && cancelFlag->load()) return;
            RefsDirEntry entry;
            if (!m_volume->ParseResidualRow(tableId, key, keyLen, val, valLen, entry))
                return;
            if (entry.name.empty() || entry.isDirectory || entry.name == L"." ||
                entry.name == L"..") return;
            FileNode node;
            if (!BuildNode(entry, 0, L"\\", node, true)) return;
            ++m_totalEntries;
            m_files.push_back(node);
            if (m_onFileFound) m_onFileFound(node);
        });

    if (onProgress) onProgress(100);
    LOG_INFO(L"RefsParser::ScanMetadata – entries=" + std::to_wstring(m_totalEntries) +
             L"  nodes=" + std::to_wstring(m_files.size()));
    return true;
}
