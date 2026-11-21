#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// In-memory Compound File Binary (OLE2 / CFB) stream enumerator.
class CfbReader
{
public:
    struct StreamInfo
    {
        std::wstring name;
        uint64_t     offsetInFile = 0; // first payload byte in the host CFB file
        const uint8_t* data       = nullptr;
        size_t           size     = 0;
    };

    using StreamCallback = std::function<void(const StreamInfo& stream)>;

    // Returns true when |data| is a CFB container and at least one stream was visited.
    static bool EnumerateStreams(const uint8_t* data, size_t len, const StreamCallback& onStream);
};
