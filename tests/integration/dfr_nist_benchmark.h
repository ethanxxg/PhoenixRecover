#pragma once

#include <cstdint>
#include <string>

// NIST CFTT FTK v3.3.0.33124 — recovery expectations for dfr-test-images.
// deletedMin/Max: observable deleted file count (Intact/Meta band when NIST Del is larger).
// fullMin: NIST "Full" — recoverOk must be >= this.
struct DfrNistExpect {
    uint32_t deletedMin = 0;
    uint32_t deletedMax = 0; // 0 => no upper bound (only check deletedMin)
    uint32_t fullMin    = 0;
    bool     hasExpect  = false;
};

inline std::wstring DfrDatasetToNistKey(const std::wstring& datasetId)
{
    if (datasetId.size() < 8 || datasetId.compare(0, 4, L"dfr-") != 0)
        return L"";
    const std::wstring rest = datasetId.substr(4);
    const size_t dash = rest.rfind(L'-');
    if (dash == std::wstring::npos || dash + 1 >= rest.size())
        return L"";
    const std::wstring fs = rest.substr(dash + 1);
    if (fs != L"fat" && fs != L"ntfs" && fs != L"xfat")
        return L"";
    return fs + L"-" + rest.substr(0, dash);
}

inline DfrNistExpect DfrNistLookup(const std::wstring& datasetId)
{
    const std::wstring key = DfrDatasetToNistKey(datasetId);
    if (key.empty())
        return {};

    auto hit = [&](const wchar_t* k, uint32_t dMin, uint32_t dMax, uint32_t full) {
        DfrNistExpect e;
        if (key == k) {
            e.hasExpect  = true;
            e.deletedMin = dMin;
            e.deletedMax = dMax;
            e.fullMin    = full;
        }
        return e;
    };

#define NIST_CASE(k, dMin, dMax, full) \
    do { \
        DfrNistExpect e = hit(k, dMin, dMax, full); \
        if (e.hasExpect) return e; \
    } while (0)

    // §4.3 — no overwrite (Del == Intact/Meta for these cases)
    NIST_CASE(L"fat-01", 3, 3, 3);
    NIST_CASE(L"fat-02", 3, 3, 3);
    NIST_CASE(L"fat-03", 3, 3, 3);
    NIST_CASE(L"fat-05", 6, 6, 6);
    NIST_CASE(L"fat-05-braid", 6, 6, 6);
    NIST_CASE(L"fat-05-nest", 6, 6, 6);
    NIST_CASE(L"fat-06", 3, 3, 3);
    NIST_CASE(L"fat-09", 780, 780, 780);
    NIST_CASE(L"fat-11", 9, 9, 9);
    NIST_CASE(L"xfat-01", 1, 1, 1);
    NIST_CASE(L"xfat-02", 1, 1, 1);
    NIST_CASE(L"xfat-03", 1, 1, 1);
    NIST_CASE(L"xfat-05", 2, 2, 2);
    NIST_CASE(L"xfat-05-braid", 2, 2, 2);
    NIST_CASE(L"xfat-05-nest", 2, 2, 2);
    NIST_CASE(L"xfat-06", 1, 1, 1);
    NIST_CASE(L"xfat-09", 253, 260, 253);
    NIST_CASE(L"xfat-11", 3, 3, 0);
    NIST_CASE(L"ntfs-01", 1, 1, 1);
    NIST_CASE(L"ntfs-02", 1, 1, 1);
    NIST_CASE(L"ntfs-03", 1, 1, 1);
    NIST_CASE(L"ntfs-05", 2, 2, 2);
    NIST_CASE(L"ntfs-05-braid", 2, 2, 2);
    NIST_CASE(L"ntfs-05-nest", 2, 2, 2);
    NIST_CASE(L"ntfs-06", 1, 1, 1);
    NIST_CASE(L"ntfs-09", 260, 260, 260);
    NIST_CASE(L"ntfs-11", 3, 3, 3);

    // §4.5 — overwrite (deleted band = Intact/Meta .. total Del when wider)
    NIST_CASE(L"fat-04", 36, 36, 36);
    NIST_CASE(L"fat-07", 6, 15, 6);
    NIST_CASE(L"fat-07-one", 3, 9, 3);
    NIST_CASE(L"fat-07-two", 0, 6, 0);
    NIST_CASE(L"fat-08", 30, 74, 30);
    NIST_CASE(L"fat-10", 780, 2340, 780);
    NIST_CASE(L"fat-12", 27, 36, 9);
    NIST_CASE(L"fat-13", 270, 414, 57);
    NIST_CASE(L"fat-14", 12, 12, 12);
    NIST_CASE(L"xfat-04", 12, 12, 12);
    NIST_CASE(L"xfat-07", 1, 5, 1);
    NIST_CASE(L"xfat-07-one", 0, 3, 0);
    NIST_CASE(L"xfat-07-two", 0, 2, 0);
    NIST_CASE(L"xfat-08", 11, 25, 11);
    NIST_CASE(L"xfat-10", 247, 780, 247);
    NIST_CASE(L"xfat-12", 9, 12, 3);
    NIST_CASE(L"xfat-13", 90, 138, 13);
    NIST_CASE(L"xfat-14", 8, 10, 8);
    NIST_CASE(L"ntfs-04", 12, 12, 12);
    NIST_CASE(L"ntfs-07", 2, 5, 2);
    NIST_CASE(L"ntfs-07-one", 1, 3, 1);
    NIST_CASE(L"ntfs-07-two", 0, 2, 0);
    NIST_CASE(L"ntfs-08", 10, 25, 10);
    NIST_CASE(L"ntfs-10", 260, 780, 260);
    NIST_CASE(L"ntfs-12", 9, 12, 9);
    NIST_CASE(L"ntfs-13", 87, 118, 92);
    NIST_CASE(L"ntfs-14", 8, 10, 8);

    NIST_CASE(L"fat-15", 0, 0, 0);
    NIST_CASE(L"fat-16", 0, 0, 0);
    NIST_CASE(L"fat-17", 0, 0, 0);
    NIST_CASE(L"ntfs-15", 0, 0, 0);
    NIST_CASE(L"ntfs-16", 0, 0, 0);
    NIST_CASE(L"ntfs-17", 0, 0, 0);
    NIST_CASE(L"xfat-15", 0, 0, 0);
    NIST_CASE(L"xfat-16", 0, 0, 0);
    NIST_CASE(L"xfat-17", 0, 0, 0);

#undef NIST_CASE
    return {};
}

inline bool DfrNistDeletedInRange(size_t deleted, const DfrNistExpect& exp)
{
    if (exp.deletedMin == 0 && exp.deletedMax == 0)
        return deleted == 0;
    if (deleted < exp.deletedMin)
        return false;
    if (exp.deletedMax > 0 && deleted > exp.deletedMax)
        return false;
    return true;
}
