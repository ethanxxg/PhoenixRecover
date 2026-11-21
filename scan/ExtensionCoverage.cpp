#include "ExtensionCoverage.h"

#include "../common/UiFileTypeCatalog.h"
#include "SignatureDB.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>

namespace {

std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}

const std::unordered_set<std::wstring>& MetadataOnlyExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        // Generic / text / source — FS metadata only (no safe carve magic)
        L"addin", L"ascii", L"bak", L"cfg", L"chk", L"config", L"cpp", L"crwl", L"cs", L"css", L"csv",
        L"enc", L"et", L"fcpevent", L"fdf", L"fft", L"fil", L"fs", L"gthr", L"htm", L"inf", L"ipynb",
        L"java", L"js", L"json", L"lst", L"m", L"manifest", L"me", L"mhtml", L"mm", L"mmap", L"mof",
        L"old", L"ort", L"p7s", L"pfx", L"php", L"py", L"rad", L"readme", L"rpt", L"rst", L"scpt",
        L"sin", L"slk", L"srt", L"swift", L"sxc", L"tab", L"text", L"tlb", L"tm", L"txt", L"url",
        L"utf8", L"vbs", L"vcproj", L"wpl", L"xib", L"xsi", L"yml", L"log", L"md", L"manifest",
        L"lnk", L"readme", L"key", L"numbers", L"dotm", L"dox", L"pot", L"potx", L"sam", L"thmx",
        L"xlf", L"xmind", L"plist", L"vcf", L"manifest",
        // Ambiguous containers / sidecar
        L"bin", L"dir", L"dat", L"h264", L"hp", L"raw", L"tga", L"rt", L"scm", L"apm", L"fbx",
        L"rpm", L"mod", L"mxf",
        // Archives / scripts without stable magic
        L"bat", L"dist", L"n", L"zap", L"zfsendtotarget", L"sh", L"cat", L"cbt", L"pack", L"pak",
        L"pfb", L"pup", L"rev", L"ro", L"sdi", L"sfx", L"img",
        // Audio misc
        L"acm", L"al", L"dls", L"dsp", L"emx", L"flp", L"ins", L"jam", L"lib", L"mui", L"pga",
        L"rbs", L"sb", L"sct", L"smp", L"sng", L"tsp", L"val", L"voc", L"qcp",
        // Mail index / shortcuts / unsaved / system sidecars
        L"idx", L"~",
        // System & scripts (carve unreliable)
        L"drv", L"ocx", L"ax", L"com", L"cpl", L"scr", L"so", L"dylib", L"ko", L"msu",
        L"vxd", L"rom", L"bios", L"pf", L"oem",
    };
    return k;
}

const std::unordered_set<std::wstring>& BookmarkExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        L"webloc", L"url",
    };
    return k;
}

const std::unordered_set<std::wstring>& UnsavedExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        L"asd", L"tmp", L"~",
    };
    return k;
}

const std::unordered_map<std::wstring, std::wstring>& AliasMap()
{
    static const std::unordered_map<std::wstring, std::wstring> k = {
        {L"jpeg", L"jpg"}, {L"jfif", L"jpg"}, {L"jpe", L"jpg"}, {L"tiff", L"tif"}, {L"dib", L"bmp"},
        {L"icon", L"ico"}, {L"aiff", L"aif"}, {L"aifc", L"aif"}, {L"mpeg", L"mpg"}, {L"mpga", L"mp3"},
        {L"3gpp", L"3gp"}, {L"3gpp2", L"3g2"}, {L"m2ts", L"mts"}, {L"m2t", L"mts"}, {L"rmvb", L"rm"},
        {L"rms", L"rm"}, {L"acc", L"aac"}, {L"midi", L"mid"}, {L"j2c", L"jp2"}, {L"j2k", L"jp2"},
        {L"jpf", L"jp2"}, {L"pef", L"tif"}, {L"nef", L"tif"}, {L"arw", L"tif"}, {L"dng", L"tif"},
        {L"srw", L"tif"}, {L"3fr", L"tif"}, {L"sci", L"tif"}, {L"scp", L"tif"},
        {L"mnw", L"tif"}, {L"dsc", L"tif"}, {L"gpd", L"tif"}, {L"gpr", L"tif"}, {L"gmp", L"tif"},
        {L"tpl", L"tif"}, {L"pam", L"pbm"}, {L"pm", L"ppm"}, {L"tgz", L"gz"}, {L"deb", L"ar"},
        {L"cbt", L"tar"}, {L"html", L"htm"}, {L"ost", L"pst"},
    };
    return k;
}

const std::unordered_set<std::wstring>& ZipContainerExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        L"docx", L"xlsx", L"pptx", L"odt", L"ods", L"odp", L"epub", L"apk", L"jar", L"ipa",
        L"odg", L"ott", L"dotx", L"potx", L"thmx", L"xlf", L"xmind", L"xps", L"numbers", L"key",
        L"dotm", L"dox", L"3mf", L"kml", L"apk",
    };
    return k;
}

const std::unordered_set<std::wstring>& CfbContainerExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        L"doc", L"xls", L"ppt", L"mdb", L"pub", L"wps", L"msi", L"vsd", L"accdb",
        L"dot", L"pot", L"sam", L"wpt",
    };
    return k;
}

const std::unordered_set<std::wstring>& MailExtensions()
{
    static const std::unordered_set<std::wstring> k = {
        L"edb", L"eml", L"pst", L"ost", L"idx", L"msg",
    };
    return k;
}

std::unordered_set<std::wstring> BuildSignatureExtensionSet(const SignatureDB& db)
{
    std::unordered_set<std::wstring> s;
    for (const auto& sig : db.GetAll())
        s.insert(Lower(sig.extension));
    return s;
}

bool HasSignature(const std::unordered_set<std::wstring>& sigs, const std::wstring& ext)
{
    return sigs.count(ext) != 0;
}

} // namespace

bool IsUiExtensionSupported(const std::wstring& extLower)
{
    SignatureDB db;
    const std::wstring e = Lower(extLower);
    const auto bad = FindUnsupportedUiExtensions(db);
    return std::find(bad.begin(), bad.end(), e) == bad.end();
}

std::vector<std::wstring> FindUnsupportedUiExtensions(const SignatureDB& db)
{
    const auto sigs = BuildSignatureExtensionSet(db);
    std::vector<std::wstring> bad;

    auto isSupported = [&](const std::wstring& e) -> bool {
        if (MetadataOnlyExtensions().count(e))
            return true;
        if (HasSignature(sigs, e))
            return true;
        auto aliasIt = AliasMap().find(e);
        if (aliasIt != AliasMap().end() && HasSignature(sigs, aliasIt->second))
            return true;
        if (ZipContainerExtensions().count(e) && HasSignature(sigs, L"zip"))
            return true;
        if (CfbContainerExtensions().count(e) && HasSignature(sigs, L"doc"))
            return true;
        if (e == L"wav" || e == L"webp" || e == L"avi")
            return true;
        if (e == L"m4a" || e == L"3gp" || e == L"3g2" || e == L"heic" || e == L"heif")
            return true;
        if (e == L"mov" && HasSignature(sigs, L"mp4"))
            return true;
        if (MailExtensions().count(e)) {
            if (HasSignature(sigs, L"pst") || HasSignature(sigs, L"eml") ||
                HasSignature(sigs, L"edb") ||
                (e == L"msg" && HasSignature(sigs, L"doc")))
                return true;
        }
        if (BookmarkExtensions().count(e)) {
            if (HasSignature(sigs, L"webloc") || HasSignature(sigs, L"url"))
                return true;
        }
        if (UnsavedExtensions().count(e)) {
            if (HasSignature(sigs, L"doc") || e == L"tmp" || e == L"~")
                return true;
        }
        return false;
    };

    for (const auto& ext : UiFileTypeCatalog::AllExtensions()) {
        const std::wstring e = Lower(ext);
        if (!isSupported(e))
            bad.push_back(e);
    }
    return bad;
}
