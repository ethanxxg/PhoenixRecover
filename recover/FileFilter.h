#pragma once

#include "FileNode.h"
#include "../common/Types.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstdint>

// ---------------------------------------------------------------------------
// FileFilter
//
// Immutable set of criteria applied to a FileNode.
// Matches() returns true when the node satisfies ALL active criteria.
// A criterion is "inactive" when it holds its default (zero / empty) value.
//
// Usage:
//   FileFilter f;
//   f.includeExtensions = { L"jpg", L"png" };
//   f.minSize = 1024;
//   auto filtered = Filter(files, f);
// ---------------------------------------------------------------------------
struct FileFilter
{
    // ── Extension ─────────────────────────────────────────────────────────
    // Case-insensitive, without the leading dot (e.g. L"jpg", L"docx").
    // includeExtensions – empty list means "accept any extension".
    // excludeExtensions – files with these extensions are excluded.
    std::vector<std::wstring> includeExtensions;
    std::vector<std::wstring> excludeExtensions;

    // ── Name ──────────────────────────────────────────────────────────────
    // Case-insensitive substring match on the file name.
    std::wstring nameContains;

    // ── Path ──────────────────────────────────────────────────────────────
    // Case-insensitive substring match on originalPath (or name as fallback).
    std::wstring pathContains;

    // ── Size  (bytes, 0 = not constrained) ────────────────────────────────
    uint64_t minSize = 0;
    uint64_t maxSize = 0;   // 0 = no upper limit

    // ── Modification time  (Windows FILETIME, 100-ns since 1601-01-01) ───
    // 0 = not constrained.
    uint64_t modifiedAfter  = 0;
    uint64_t modifiedBefore = 0;

    // ── Recoverability ────────────────────────────────────────────────────
    // Unknown means "no constraint".
    // When set to e.g. Good, only files with Good or Excellent pass.
    RecoverabilityLevel minRecoverability = RecoverabilityLevel::Unknown;

    // ── Type ──────────────────────────────────────────────────────────────
    bool includeDirectories = false;  // Exclude directories by default

    // ── Helpers ───────────────────────────────────────────────────────────

    bool IsEmpty() const
    {
        return includeExtensions.empty()
            && excludeExtensions.empty()
            && nameContains.empty()
            && pathContains.empty()
            && minSize == 0
            && maxSize == 0
            && modifiedAfter  == 0
            && modifiedBefore == 0
            && minRecoverability == RecoverabilityLevel::Unknown
            && !includeDirectories;
    }

    // Returns true if 'node' satisfies every active criterion.
    bool Matches(const FileNode& node) const
    {
        // Directories
        if (node.isDirectory && !includeDirectories)
            return false;

        // Extension – include list
        if (!includeExtensions.empty())
        {
            std::wstring ext = Lower(node.extension);
            bool found = false;
            for (const auto& e : includeExtensions)
                if (ext == Lower(e)) { found = true; break; }
            if (!found) return false;
        }

        // Extension – exclude list
        if (!excludeExtensions.empty())
        {
            std::wstring ext = Lower(node.extension);
            for (const auto& e : excludeExtensions)
                if (ext == Lower(e)) return false;
        }

        // Name substring
        if (!nameContains.empty())
        {
            if (Lower(node.name).find(Lower(nameContains)) == std::wstring::npos)
                return false;
        }

        // Path substring
        if (!pathContains.empty())
        {
            const std::wstring& src = node.originalPath.empty()
                                    ? node.name : node.originalPath;
            if (Lower(src).find(Lower(pathContains)) == std::wstring::npos)
                return false;
        }

        // Size
        if (minSize > 0 && node.size < minSize) return false;
        if (maxSize > 0 && node.size > maxSize) return false;

        // Modification time
        if (modifiedAfter  > 0 && node.modifyTime < modifiedAfter)  return false;
        if (modifiedBefore > 0 && node.modifyTime > modifiedBefore) return false;

        // Recoverability
        if (minRecoverability != RecoverabilityLevel::Unknown)
        {
            if (RecLevel(node.recoverability) < RecLevel(minRecoverability))
                return false;
        }

        return true;
    }

    // Filter a list of nodes; returns a new vector.
    std::vector<FileNode> Apply(const std::vector<FileNode>& nodes) const
    {
        if (IsEmpty()) return nodes;

        std::vector<FileNode> out;
        out.reserve(nodes.size());
        for (const auto& n : nodes)
            if (Matches(n)) out.push_back(n);
        return out;
    }

private:
    // Case-insensitive helpers (static, so usable inside const methods)
    static std::wstring Lower(std::wstring s)
    {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }

    // Recoverability level → numeric rank (higher = better)
    static int RecLevel(RecoverabilityLevel r)
    {
        switch (r)
        {
        case RecoverabilityLevel::Excellent: return 4;
        case RecoverabilityLevel::Good:      return 3;
        case RecoverabilityLevel::Poor:      return 2;
        case RecoverabilityLevel::Lost:      return 1;
        default:                             return 0; // Unknown
        }
    }
};
