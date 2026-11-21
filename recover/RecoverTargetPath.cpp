#include "RecoverEngine.h"

#include "../common/Utils.h"

namespace {

static bool IsIllegalPathChar(wchar_t c)
{
    return c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
           c == L'"' || c == L'<' || c == L'>' || c == L'|';
}

static std::wstring SanitizePathComponent(const std::wstring& component)
{
    if (component.empty())
        return L"_";

    std::wstring out;
    out.reserve(component.size());
    for (wchar_t c : component) {
        if (IsIllegalPathChar(c) || c < 32)
            out += L'_';
        else
            out += c;
    }

    // Windows: no trailing dots/spaces in path components.
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    if (out.empty())
        out = L"_";

    // Reserved device names (case-insensitive).
    static const wchar_t* kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };
    for (const wchar_t* r : kReserved) {
        if (_wcsicmp(out.c_str(), r) == 0) {
            out += L"_";
            break;
        }
    }

    // Long IE-cache / URL filenames: truncate with stable suffix.
    constexpr size_t kMaxComponent = 200;
    if (out.size() > kMaxComponent) {
        uint32_t hash = 2166136261u;
        for (wchar_t c : component) {
            hash ^= static_cast<uint32_t>(c);
            hash *= 16777619u;
        }
        wchar_t suffix[16]{};
        swprintf_s(suffix, L"_%08x", hash);
        out.resize(kMaxComponent);
        out += suffix;
    }

    return out;
}

static std::wstring SanitizePathSegments(std::wstring path)
{
    if (path.empty())
        return path;

    for (auto& c : path) {
        if (c == L'/')
            c = L'\\';
    }

    std::wstring result;
    size_t i = 0;
    while (i < path.size()) {
        const size_t next = path.find(L'\\', i);
        const std::wstring seg = (next == std::wstring::npos)
                                     ? path.substr(i)
                                     : path.substr(i, next - i);
        if (!seg.empty()) {
            if (!result.empty())
                result += L'\\';
            result += SanitizePathComponent(seg);
        }
        if (next == std::wstring::npos)
            break;
        i = next + 1;
    }
    return result;
}

} // namespace

std::wstring RecoverEngine::BuildTargetPathForNode(const FileNode&      node,
                                                   const std::wstring& targetDir,
                                                   bool                keepDirStructure)
{
    std::wstring path = targetDir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';

    if (keepDirStructure && !node.originalPath.empty()) {
        std::wstring rel = node.originalPath;
        if (rel.size() >= 2 && rel[1] == L':') rel = rel.substr(2);
        if (!rel.empty() && rel[0] == L'\\') rel = rel.substr(1);
        path += SanitizePathSegments(rel);
    } else {
        path += SanitizePathComponent(node.name);
    }

    // Avoid MAX_PATH failures on deep IE-cache paths: flatten to basename only.
    if (path.size() > 240) {
        std::wstring flat = targetDir;
        if (!flat.empty() && flat.back() != L'\\') flat += L'\\';
        flat += SanitizePathComponent(node.name.empty() ? L"recovered" : node.name);
        if (flat.size() <= 240)
            path = flat;
    }

    return path;
}
