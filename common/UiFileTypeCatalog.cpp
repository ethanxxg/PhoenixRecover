#include "UiFileTypeCatalog.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <vector>

namespace {

std::wstring ToLower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

#define EXT(...) std::vector<const wchar_t*>{ __VA_ARGS__ }

// Each extension appears in exactly one category (validated by ValidateUniqueOwnership).
const std::vector<UiFileTypeCatalog::Category>& BuildCatalog()
{
    static const std::vector<UiFileTypeCatalog::Category> kCatalog = {
        { L"图片", EXT(
            L"jpg", L"jpeg", L"png", L"gif", L"tif", L"eps", L"psd", L"bmp", L"ico", L"3mf", L"ai", L"ani",
            L"apm", L"crw", L"cur", L"dds", L"djvu", L"dsc", L"emf", L"exr", L"gmp", L"gpd", L"gpr", L"hdr",
            L"heic", L"heif", L"hp", L"icns", L"jp2", L"jpf", L"jxr", L"mnw", L"pbm", L"pgm", L"pm",
            L"ppm", L"ps", L"raw", L"sci", L"scp", L"sgi", L"shp", L"stl", L"svg", L"tga",
            L"webp", L"wmf", L"xar", L"xbm", L"xcf", L"xpm",
            L"avif", L"arw", L"cr2", L"dng", L"nef", L"orf", L"pam", L"pef", L"raf",
            L"rw2", L"srw", L"3fr", L"mrw", L"j2c", L"j2k") },
        { L"视频", EXT(
            L"mp4", L"mov", L"avi", L"swf", L"m4v", L"wmv", L"3g2", L"3gp", L"3gpp", L"3gpp2", L"aaf", L"asf",
            L"bin", L"dat", L"dir", L"dv", L"fbx", L"flv", L"h264", L"m2t", L"m2ts", L"m3u", L"mkv", L"mod",
            L"mpeg", L"mts", L"mxf", L"ogv", L"rms", L"rmvb", L"rpm", L"rt", L"scm", L"ts", L"webm",
            L"srt", L"vob") },
        { L"文档", EXT(
            L"doc", L"docx", L"pdf", L"xls", L"xlsx", L"ppt", L"pptx", L"3ds", L"accdb", L"addin", L"ascii",
            L"bak", L"cfg", L"chk", L"chm", L"config", L"cpp", L"crwl", L"cs", L"css", L"csv", L"db", L"dot",
            L"dotm", L"dotx", L"dox", L"dwg", L"enc", L"eot", L"epub", L"et", L"fcpevent", L"fdf", L"fft", L"fil",
            L"fs", L"gpx", L"gthr", L"htm", L"html", L"ics", L"ipynb", L"java", L"js", L"json", L"key",
            L"lit", L"log", L"lst", L"lzh", L"m", L"manifest", L"md", L"mdb", L"me", L"mhtml", L"mm",
            L"mmap", L"mobi", L"mof", L"numbers", L"odg", L"odp", L"ods", L"odt", L"old", L"ort", L"ott",
            L"p7s", L"pem", L"pfx", L"php", L"plist", L"pot", L"potx", L"pub", L"py", L"rad", L"readme", L"rpt",
            L"rst", L"rtf", L"sam", L"scpt", L"sitx", L"slk", L"sin", L"swift", L"sxc", L"tab", L"text",
            L"thmx", L"tlb", L"tm", L"ttf", L"txt", L"utf8", L"vbs", L"vcf", L"vcproj", L"woff", L"woff2",
            L"wpl", L"wps", L"wpt", L"xib", L"xlf", L"xmind", L"xml", L"xsi", L"yml") },
        { L"音频", EXT(
            L"mp3", L"wav", L"wma", L"m4a", L"ape", L"flac", L"aac", L"acc", L"acm", L"aifc", L"aiff", L"al",
            L"amr", L"au", L"dls", L"dsp", L"emx", L"flp", L"ins", L"jam", L"lib", L"mid", L"midi", L"mpc",
            L"mpga", L"mui", L"pga", L"ogg", L"qcp", L"rbs", L"sb", L"sct", L"smp", L"sng", L"tsp", L"val",
            L"voc") },
        { L"压缩文件", EXT(
            L"zip", L"rar", L"7z", L"tar", L"apk", L"bat", L"bz2", L"cab", L"cat", L"cbt", L"deb", L"dist",
            L"gz", L"img", L"iso", L"jar", L"lzma", L"msi", L"otf", L"pack", L"pak", L"pf", L"pfb", L"pup",
            L"ro", L"rev", L"sdi", L"sfx", L"sh", L"tgz", L"ttc", L"wim", L"xz", L"n", L"zap", L"zfsendtotarget",
            L"z") },
        { L"邮件", EXT(
            L"edb", L"eml", L"idx", L"pst", L"ost", L"msg") },
        { L"书签", EXT(
            L"webloc", L"url") },
        { L"未保存的文件", EXT(
            L"asd", L"tmp", L"~") },
        { L"其他", EXT(
            L"sys", L"exe", L"dll", L"ini", L"drv", L"ocx", L"ax", L"com", L"cpl", L"scr", L"so", L"dylib",
            L"ko", L"msu", L"ps1", L"vxd", L"efi", L"rom", L"bios", L"cmd", L"lnk", L"inf") },
    };
    return kCatalog;
}

#undef EXT

} // namespace

const std::vector<const wchar_t*>& UiFileTypeCatalog::CategoryLookupOrder()
{
    // Kept for stable UI sort / future overlap; catalog entries are unique per extension.
    static const std::vector<const wchar_t*> kOrder = {
        L"未保存的文件",
        L"邮件",
        L"书签",
        L"图片",
        L"视频",
        L"音频",
        L"文档",
        L"压缩文件",
        L"其他",
    };
    return kOrder;
}

const std::vector<UiFileTypeCatalog::Category>& UiFileTypeCatalog::Categories()
{
    return BuildCatalog();
}

std::vector<std::wstring> UiFileTypeCatalog::AllExtensions()
{
    std::vector<std::wstring> out;
    for (const auto& cat : Categories()) {
        for (const wchar_t* e : cat.extensions)
            out.emplace_back(e);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::wstring> UiFileTypeCatalog::ExtensionsForCategory(const wchar_t* categoryName)
{
    std::vector<std::wstring> out;
    for (const auto& cat : Categories()) {
        if (std::wcscmp(cat.name, categoryName) != 0)
            continue;
        for (const wchar_t* e : cat.extensions)
            out.emplace_back(e);
        break;
    }
    return out;
}

std::wstring UiFileTypeCatalog::CategoryForExtension(const std::wstring& extLower)
{
    const std::wstring e = ToLower(extLower);
    if (e.empty())
        return L"其他";

    for (const auto& cat : Categories()) {
        for (const wchar_t* x : cat.extensions) {
            if (e == x)
                return cat.name;
        }
    }
    return L"其他";
}

std::vector<UiFileTypeCatalog::DuplicateReport> UiFileTypeCatalog::FindDuplicateExtensions()
{
    std::unordered_map<std::wstring, std::vector<const wchar_t*>> owners;
    for (const auto& cat : Categories()) {
        for (const wchar_t* ext : cat.extensions) {
            owners[ToLower(ext)].push_back(cat.name);
        }
    }

    std::vector<DuplicateReport> out;
    for (auto& [ext, cats] : owners) {
        if (cats.size() <= 1)
            continue;
        DuplicateReport r;
        r.extension = ext;
        for (const wchar_t* c : cats)
            r.categories.emplace_back(c);
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(),
              [](const DuplicateReport& a, const DuplicateReport& b) {
                  return a.extension < b.extension;
              });
    return out;
}
