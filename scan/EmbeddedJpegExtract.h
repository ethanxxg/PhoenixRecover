#pragma once

#include "../recover/FileNode.h"
#include "../disk/Partition.h"

#include <cstdint>
#include <vector>

class SectorReader;

// One JPEG payload discovered inside a host file (container, OLE, ADS parent, etc.).
struct EmbeddedJpegSlice
{
    uint64_t              offsetInHost = 0;
    std::vector<uint8_t>  bytes;
    std::wstring          innerPath; // e.g. zip entry name or tar member
};

// Engine-side container / buffer JPEG extraction (DFTT #8 embed scenarios).
class EmbeddedJpegExtract
{
public:
    // Scan arbitrary bytes: raw SOI..EOI, ZIP, gzip/tar, OLE/CFB streams (recursive).
    static void CollectFromBuffer(const uint8_t* data, size_t len,
                                  std::vector<EmbeddedJpegSlice>& out);

    // After FS metadata scan: read each host file and emit embedded JPEG FileNodes.
    static std::vector<FileNode> ExpandFromHosts(
        SectorReader& reader,
        const Partition& partition,
        const std::vector<FileNode>& hosts,
        uint64_t& nextId,
        uint64_t maxHostBytes = 32u * 1024u * 1024u);
};
