#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

struct CustomCarveGolden {
    std::string name;
    uint64_t    offset = 0;
    uint64_t    size = 0;
};

struct CustomCarveImageCase {
    std::string image;
    std::vector<CustomCarveGolden> files;
};

inline static bool ParseJsonStringValue(const std::string& content, size_t keyPos, std::string& out)
{
    const size_t colon = content.find(':', keyPos);
    if (colon == std::string::npos)
        return false;
    const size_t q0 = content.find('"', colon + 1);
    if (q0 == std::string::npos)
        return false;
    const size_t q1 = content.find('"', q0 + 1);
    if (q1 == std::string::npos)
        return false;
    out = content.substr(q0 + 1, q1 - q0 - 1);
    return true;
}

inline static bool ParseJsonUintValue(const std::string& content, size_t keyPos, uint64_t& out)
{
    const size_t colon = content.find(':', keyPos);
    if (colon == std::string::npos)
        return false;
    size_t i = colon + 1;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' || content[i] == '\r'))
        ++i;
    if (i >= content.size())
        return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(content.c_str() + i, &end, 10);
    if (end == content.c_str() + i)
        return false;
    out = v;
    return true;
}

inline static void ParseFilesSlice(const std::string& slice, std::vector<CustomCarveGolden>& out)
{
    size_t pos = 0;
    for (;;) {
        const size_t nameKey = slice.find("\"name\"", pos);
        if (nameKey == std::string::npos)
            break;
        const size_t sizeKey = slice.find("\"size\"", nameKey);
        const size_t offKey  = slice.find("\"offset\"", nameKey);
        if (sizeKey == std::string::npos || offKey == std::string::npos)
            break;

        CustomCarveGolden g{};
        if (!ParseJsonStringValue(slice, nameKey, g.name))
            break;
        if (!ParseJsonUintValue(slice, sizeKey, g.size))
            break;
        if (!ParseJsonUintValue(slice, offKey, g.offset))
            break;
        out.push_back(std::move(g));
        pos = offKey + 8;
    }
}

inline static bool LoadCustomCarvingManifest(const std::string& manifestPath,
                                             std::vector<CustomCarveImageCase>& out)
{
    std::ifstream f(manifestPath, std::ios::binary);
    if (!f)
        return false;
    const std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    size_t pos = 0;
    while (pos < content.size()) {
        const size_t imageKey = content.find("\"image\"", pos);
        if (imageKey == std::string::npos)
            break;

        CustomCarveImageCase cs{};
        if (!ParseJsonStringValue(content, imageKey, cs.image))
            break;

        const size_t filesKey = content.find("\"files\"", imageKey);
        if (filesKey == std::string::npos)
            break;

        const size_t nextImageKey = content.find("\"image\"", imageKey + 8);
        const size_t sliceEnd = nextImageKey == std::string::npos ? content.size() : nextImageKey;
        const std::string slice = content.substr(filesKey, sliceEnd - filesKey);
        ParseFilesSlice(slice, cs.files);

        if (!cs.files.empty())
            out.push_back(std::move(cs));

        if (nextImageKey == std::string::npos)
            break;
        pos = nextImageKey;
    }
    return !out.empty();
}
