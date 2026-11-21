#pragma once

#include <string>
#include <vector>

// Single source of truth for UI type-tree extensions (图片/视频/文档/音频/压缩文件).
struct UiFileTypeCatalog
{
    struct Category
    {
        const wchar_t* name;
        std::vector<const wchar_t*> extensions;
    };

    static const std::vector<Category>& Categories();

    // Category resolution order when an extension appears in multiple lists.
    static const std::vector<const wchar_t*>& CategoryLookupOrder();

    struct DuplicateReport
    {
        std::wstring              extension;
        std::vector<std::wstring> categories;
    };

    static std::vector<std::wstring> AllExtensions();
    static std::vector<std::wstring> ExtensionsForCategory(const wchar_t* categoryName);
    static std::wstring CategoryForExtension(const std::wstring& extLower);

    // Returns cross-category duplicates (empty when catalog is clean).
    static std::vector<DuplicateReport> FindDuplicateExtensions();
};
