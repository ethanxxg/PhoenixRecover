#include "UiTypeSignatureSupplement.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::vector<uint8_t> Hex(const char* hex)
{
    std::vector<uint8_t> out;
    std::string h(hex);
    for (size_t i = 0; i + 1 < h.size(); ++i) {
        if (h[i] == ' ') continue;
        unsigned v = 0;
        if (sscanf(h.c_str() + i, "%02x", &v) == 1)
            out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

FileSignature MakeSig(const wchar_t* ext, const char* hdr, const char* ftr = nullptr,
                      int hdrOff = 0, uint64_t maxMB = 50)
{
    FileSignature s;
    s.extension    = ext;
    s.header       = Hex(hdr);
    s.headerOffset = hdrOff;
    if (ftr) s.footer = Hex(ftr);
    s.maxSize = maxMB * 1024 * 1024;
    return s;
}

void AddAlias(SignatureDB& db, const wchar_t* ext, const wchar_t* srcExt)
{
    for (const auto& s : db.GetAll()) {
        if (s.extension == srcExt) {
            FileSignature copy = s;
            copy.extension = ext;
            db.Add(std::move(copy));
            return;
        }
    }
}

} // namespace

void RegisterUiTypeSignatureSupplement(SignatureDB& db)
{
    db.Add(MakeSig(L"dds",  "44445320", nullptr, 0, 200));
    db.Add(MakeSig(L"3mf",  "504B0304", nullptr, 0, 500));
    db.Add(MakeSig(L"stl",  "534C5420", nullptr, 0, 200));
    db.Add(MakeSig(L"stl",  "736F6C6964", nullptr, 0, 200));
    db.Add(MakeSig(L"shp",  "0000270A", nullptr, 0, 500));
    db.Add(MakeSig(L"shp",  "0000270D", nullptr, 0, 500));
    db.Add(MakeSig(L"xbm",  "23646566696E65", nullptr, 0, 10));
    db.Add(MakeSig(L"asf",  "3026B2758E66CF11A6D900AA0062CE6C", nullptr, 0, 4096));
    db.Add(MakeSig(L"wma",  "3026B2758E66CF11A6D900AA0062CE6C", nullptr, 0, 500));
    db.Add(MakeSig(L"wma",  "484D4289", nullptr, 0, 500));
    db.Add(MakeSig(L"dv",   "1D070100", nullptr, 0, 4096));
    db.Add(MakeSig(L"m3u",  "234558544D3355", nullptr, 0, 5));
    db.Add(MakeSig(L"aaf",  "41414620", nullptr, 0, 4096));
    db.Add(MakeSig(L"pem",  "2D2D2D2D2D424547494E", nullptr, 0, 10));
    db.Add(MakeSig(L"java", "CAFEBABE", nullptr, 0, 50));
    db.Add(MakeSig(L"db",   "53514C697465203320", nullptr, 0, 4096));
    db.Add(MakeSig(L"html", "3C21444F43545950", nullptr, 0, 50));
    db.Add(MakeSig(L"htm",  "3C21444F43545950", nullptr, 0, 50));
    db.Add(MakeSig(L"xml",  "3C3F786D6C", nullptr, 0, 50));
    db.Add(MakeSig(L"ttc",  "74746366", nullptr, 0, 50));
    db.Add(MakeSig(L"odg",  "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"ott",  "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"numbers", "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"key",  "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"dotm", "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"dox",  "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"thmx", "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"xlf",  "504B0304", nullptr, 0, 200));
    db.Add(MakeSig(L"xmind", "504B0304", nullptr, 0, 200));

    // Mail / bookmarks / unsaved / system (other categories)
    db.Add(MakeSig(L"webloc", "62706C6973743030", nullptr, 0, 10));   // bplist00
    db.Add(MakeSig(L"url",    "5B496E7465726E657453686F7274637574", nullptr, 0, 5)); // [InternetShortcut]
    db.Add(MakeSig(L"ini",    "5B", nullptr, 0, 50));                  // INI section [
    db.Add(MakeSig(L"inf",    "5B76657273696F6E5D", nullptr, 0, 50));   // [version]
    db.Add(MakeSig(L"inf",    "FFFE", nullptr, 0, 50));                 // Unicode INI
    db.Add(MakeSig(L"cmd",    "4045444F2E", nullptr, 0, 10));          // "@echo." batch
    db.Add(MakeSig(L"ps1",    "2320", nullptr, 0, 10));                  // "# " PowerShell
    db.Add(MakeSig(L"lnk",    "4C000000", nullptr, 0, 5));             // Windows .lnk
    db.Add(MakeSig(L"asd",    "D0CF11E0A1B11AE1", nullptr, 0, 100));     // Word/Office autosave (CFB)
    db.Add(MakeSig(L"vob",    "000001BA", nullptr, 0, 8192));              // DVD MPEG-2 pack

    const std::pair<const wchar_t*, const wchar_t*> kAliases[] = {
        {L"jpeg", L"jpg"}, {L"jfif", L"jpg"}, {L"jpe", L"jpg"}, {L"tiff", L"tif"},
        {L"dib", L"bmp"}, {L"icon", L"ico"}, {L"aiff", L"aif"}, {L"aifc", L"aif"},
        {L"mpeg", L"mpg"}, {L"mpga", L"mp3"}, {L"3gpp", L"3gp"}, {L"3gpp2", L"3g2"},
        {L"m2ts", L"mts"}, {L"m2t", L"mts"}, {L"rmvb", L"rm"}, {L"rms", L"rm"},
        {L"acc", L"aac"}, {L"midi", L"mid"}, {L"j2c", L"jp2"}, {L"j2k", L"jp2"},
        {L"jpf", L"jp2"}, {L"pef", L"tif"}, {L"nef", L"tif"}, {L"arw", L"tif"},
        {L"dng", L"tif"}, {L"srw", L"tif"}, {L"3fr", L"tif"}, {L"sci", L"tif"},
        {L"scp", L"tif"}, {L"scr", L"tif"}, {L"mnw", L"tif"}, {L"dsc", L"tif"},
        {L"gpd", L"tif"}, {L"gpr", L"tif"}, {L"gmp", L"tif"}, {L"tpl", L"tif"},
        {L"pam", L"pbm"}, {L"pm", L"ppm"}, {L"tgz", L"gz"},
        {L"docx", L"zip"}, {L"xlsx", L"zip"}, {L"pptx", L"zip"},
        {L"odt", L"zip"}, {L"ods", L"zip"}, {L"odp", L"zip"},
        {L"apk", L"zip"}, {L"jar", L"zip"}, {L"ipa", L"zip"},
        {L"dotx", L"zip"}, {L"potx", L"zip"},
        {L"dot", L"doc"}, {L"pot", L"ppt"}, {L"wps", L"doc"}, {L"accdb", L"mdb"},
        {L"pub", L"doc"}, {L"wpt", L"doc"}, {L"sam", L"doc"},
        {L"ost", L"pst"}, {L"idx", L"pst"}, {L"vob", L"mpg"},
        {L"drv", L"sys"}, {L"ocx", L"dll"}, {L"ax", L"dll"}, {L"com", L"exe"},
        {L"scr", L"exe"}, {L"vxd", L"dll"}, {L"efi", L"exe"},
    };
    for (const auto& a : kAliases)
        AddAlias(db, a.first, a.second);
}
