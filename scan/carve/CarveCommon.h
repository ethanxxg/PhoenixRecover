#pragma once

#include <cstdint>
#include <string>

struct CarveResult
{
    bool ok = false;
    uint64_t endOffset = 0;      // absolute disk byte offset of end (exclusive)
    std::wstring extOverride;    // e.g. L"docx"
    int confidence = 0;          // 0..100
    std::wstring reason;         // short debug message
};

