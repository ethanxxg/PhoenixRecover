#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <utility>

// ---------------------------------------------------------------------------
// BadSectorMap
//
// Tracks known bad (unreadable) sectors on a disk/volume using a compact,
// sorted list of LBA ranges.  Adjacent or overlapping ranges are automatically
// merged, so even disks with thousands of bad sectors use minimal memory.
//
// Thread safety: NOT thread-safe; caller must synchronize if needed.
// ---------------------------------------------------------------------------
class BadSectorMap
{
public:
    // A contiguous run of bad sectors.
    struct Range
    {
        uint64_t startLba;   // First bad sector (inclusive)
        uint64_t endLba;     // Last  bad sector (inclusive)
        uint64_t Count() const { return endLba - startLba + 1; }
    };

    // ── Mutation ──────────────────────────────────────────────────────────

    void MarkBad(uint64_t lba)
    {
        MarkBadRange(lba, 1);
    }

    // Mark [startLba, startLba + count) as bad.  Merges with neighbours.
    void MarkBadRange(uint64_t startLba, uint64_t count)
    {
        if (count == 0) return;

        uint64_t newEnd = startLba + count - 1;

        // Find first range that could overlap or be adjacent to [startLba, newEnd]
        // i.e. ranges whose end >= startLba - 1
        auto it = std::lower_bound(
            m_ranges.begin(), m_ranges.end(), startLba,
            [](const Range& r, uint64_t v) { return r.endLba + 1 < v; });

        if (it == m_ranges.end() || it->startLba > newEnd + 1)
        {
            // No overlap or adjacency — insert a new range
            m_count += count;
            Range r{ startLba, newEnd };
            m_ranges.insert(it, r);
            return;
        }

        // Merge all overlapping / adjacent ranges into one
        // 防止 Windows.h 里的 min/max 宏污染：必须用 (std::min)(...) 形式
        uint64_t mergedStart = (std::min)(it->startLba, startLba);
        uint64_t mergedEnd   = (std::max)(it->endLba,   newEnd);

        // Remove old count of first merged range
        m_count -= it->Count();
        auto end = std::next(it);

        while (end != m_ranges.end() && end->startLba <= mergedEnd + 1)
        {
            mergedEnd = (std::max)(mergedEnd, end->endLba);
            m_count  -= end->Count();
            ++end;
        }

        it->startLba = mergedStart;
        it->endLba   = mergedEnd;
        m_ranges.erase(std::next(it), end);

        m_count += it->Count();
    }

    // ── Query ─────────────────────────────────────────────────────────────

    bool IsBad(uint64_t lba) const
    {
        // Find last range with startLba <= lba
        auto it = std::upper_bound(
            m_ranges.begin(), m_ranges.end(), lba,
            [](uint64_t v, const Range& r) { return v < r.startLba; });

        if (it == m_ranges.begin()) return false;
        --it;
        return lba <= it->endLba;
    }

    // Total number of bad sectors recorded (not range count)
    uint64_t Count() const { return m_count; }

    bool IsEmpty() const { return m_count == 0; }

    // Number of distinct bad ranges
    size_t RangeCount() const { return m_ranges.size(); }

    const std::vector<Range>& Ranges() const { return m_ranges; }

    void Clear()
    {
        m_ranges.clear();
        m_count = 0;
    }

private:
    std::vector<Range> m_ranges; // sorted by startLba, non-overlapping
    uint64_t           m_count = 0;
};
