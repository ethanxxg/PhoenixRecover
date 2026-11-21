// headless PhoenixRecover driver for the real damaged disk (P0-2 / P1 / P2 / P0-6).
//
// ZERO GUI CHANGES. ZERO WRITES TO THE SOURCE DISK.
//   * SectorReader::Open(diskIndex) opens \\.\PhysicalDriveN with GENERIC_READ
//     only (see SectorReader_win.cpp). No write IOCTL, no chkdsk, no import.
//   * LDM (0x42) volumes are fed to the engine by constructing Partition by
//     hand, because PartitionProbe has no LDM parser. The engine itself is
//     unchanged: NtfsParser + RecoverEngine are used exactly as in the GUI.
//
// P0-6 — engine-only pipeline (user requirement):
//   * DISCOVERY  = NtfsParser::ScanMetadata() + GetAllFiles()   (`scan` cmd)
//     The independent Python MFT oracle is a VERIFICATION TRUTH only, it is
//     never a source of file candidates.
//   * RECOVERY   = RecoverEngine::RecoverBatch()                 (`recover` cmd)
//   * FORMAT     = SignatureDB::Match() (513 built-in signatures, AC matcher)
//     + FormatPayloadScoreUtil::Score()/ProbeExtension() — engine code, never
//     an extension-name guess in the orchestrator.
//   * Prefix reads during `scan` use RecoverEngine::ReadFileDataPrefix(), so
//     resident / compressed / fragmented nodes are decoded by the engine.
//
// Usage:
//   phoenix_headless scan     --disk 2 --boot 63 --out engine_nodes_vol63.csv \
//                              [--engine-format 1] [--ids ids.txt]
//   phoenix_headless enumerate --disk 2 --boot 63 --out nodes.csv
//   phoenix_headless recover   --disk 2 --boot 63 --ids ids.txt \
//                              --out-dir <staging> --result result.csv
//
#include "../disk/SectorReader.h"
#include "../disk/Partition.h"
#include "../disk/BadSectorMap.h"
#include "../fs/ntfs/NtfsParser.h"
#include "../recover/RecoverEngine.h"
#include "../recover/FileNode.h"
#include "../recover/FormatPayloadScore.h"
#include "../scan/SignatureDB.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

std::string Utf8(const std::wstring& w)
{
#if defined(_WIN32)
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

std::wstring Wide(const std::string& s)
{
#if defined(_WIN32)
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string Csv(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c == '\r' || c == '\n') out += ' ';
        else out += c;
    }
    out += '"';
    return out;
}

struct Args {
    std::string cmd;
    int         disk = 2;
    // P0-14: read from a raw volume image instead of a physical disk. When set,
    // --disk is ignored for I/O (it is still reported in the CSV for provenance)
    // and --boot is the boot-sector LBA *inside the image* (0 for a volume dump).
    std::wstring image;
    uint64_t    boot = 0;
    std::string out;
    std::string ids;
    std::string outDir;
    std::string result;
    uint64_t    limit = 0;
    // A1-12: opt-in sequential read-ahead. 0 = unmodified baseline path.
    uint32_t    readAheadMB = 0;
    // P0-4: output-path isolation (default ON). See SafeLeaf() below.
    bool        keepOriginalPaths = false;
    // P0-14: flat isolated output. One file per shard root instead of one
    // directory per id; avoids ~872k CreateDirectoryRecursive calls.
    bool        flatOutput = false;
    // P0-6: engine-side file-format matching. ON by default for `recover`;
    // `scan` only probes a bounded subset unless --ids is supplied.
    bool        engineFormat = true;
    uint32_t    formatProbeBytes = 4096;
    uint64_t    formatSample = 4096;
};

static std::wstring LowerW(std::wstring s)
{
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

// NTFS metadata streams ($MFT, $LogFile, ...) are not user files.
static bool IsSystemNode(const FileNode& n)
{
    return !n.name.empty() && n.name[0] == L'$';
}

// Minimal RFC-4180-style split for the result CSV written by `recover`.
static std::vector<std::string> SplitCsvLine(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (c == ',' && !quoted) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

struct VerifyRow {
    uint64_t    id = 0;
    std::string path;
};

// The id list comes from the ENGINE result CSV (and optionally an explicit
// shard filter).  The independent oracle is never consulted here.
static std::vector<VerifyRow> ReadRecoverResultRows(const std::string& csvPath,
                                                     const std::string& idsPath)
{
    std::unordered_set<std::string> wanted;
    if (!idsPath.empty()) {
        std::ifstream f(idsPath);
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) wanted.insert(line);
    }

    std::vector<VerifyRow> out;
    std::ifstream f(csvPath);
    if (!f) {
        std::cerr << "cannot read result CSV " << csvPath << "\n";
        return out;
    }
    std::string line;
    std::getline(f, line);  // header
    const std::vector<std::string> head = SplitCsvLine(line);
    size_t idCol = 0, statusCol = 1, pathCol = 2;
    for (size_t i = 0; i < head.size(); ++i) {
        if (head[i] == "id") idCol = i;
        else if (head[i] == "status") statusCol = i;
        else if (head[i] == "targetPath") pathCol = i;
    }
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> c = SplitCsvLine(line);
        if (c.size() <= pathCol) continue;
        if (c[statusCol] != "0") continue;           // only successful recoveries
        if (!wanted.empty() && !wanted.count(c[idCol])) continue;
        out.push_back({std::strtoull(c[idCol].c_str(), nullptr, 10), c[pathCol]});
    }
    return out;
}

bool ParseArgs(int argc, char** argv, Args& a)
{
    if (argc < 2) return false;
    a.cmd = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if      (k == "--disk")     a.disk    = std::atoi(v.c_str());
        else if (k == "--image")    a.image   = Wide(v);
        else if (k == "--boot")     a.boot    = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "--out")      a.out     = v;
        else if (k == "--ids")      a.ids     = v;
        else if (k == "--out-dir")  a.outDir  = v;
        else if (k == "--result")   a.result  = v;
        else if (k == "--limit")    a.limit   = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "--read-ahead-mb")
            a.readAheadMB = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "--keep-original-paths")
            a.keepOriginalPaths = (std::atoi(v.c_str()) != 0);
        else if (k == "--flat-output")
            a.flatOutput = (std::atoi(v.c_str()) != 0);
        else if (k == "--engine-format")
            a.engineFormat = (std::atoi(v.c_str()) != 0);
        else if (k == "--format-probe-bytes")
            a.formatProbeBytes = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "--format-sample")
            a.formatSample = std::strtoull(v.c_str(), nullptr, 10);
        else { std::cerr << "unknown option " << k << "\n"; return false; }
    }
    return true;
}

// Bad disk: never hammer a failing sector. One retry at most, short timeout,
// zero-fill unreadable sectors and keep going (BadSectorMap records them).
ReadPolicy MakeReadOnlyPolicy()
{
    ReadPolicy p;
    p.maxRetries     = 1;
    p.timeoutMs      = 5000;
    p.retryDelayMs   = 50;
    p.skipBadSectors = true;
    return p;
}

bool OpenVolume(SectorReader& reader, int diskIndex, const std::wstring& image,
                uint64_t bootLBA, Partition& part)
{
    reader.SetReadPolicy(MakeReadOnlyPolicy());
    if (!image.empty()) {
        if (!reader.OpenImage(image)) {
            std::cerr << "SectorReader::OpenImage(" << Utf8(image)
                      << ") FAILED\n";
            return false;
        }
        std::cerr << "[engine] source = image " << Utf8(image) << "\n";
    } else if (!reader.Open(diskIndex)) {
        std::cerr << "SectorReader::Open(disk=" << diskIndex << ") FAILED\n";
        return false;
    }
    part.index           = 0;
    part.physicalDiskIdx = diskIndex;
    part.startSector     = bootLBA;   // absolute LBA of the NTFS boot sector
    part.fsType          = L"NTFS";

    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot((size_t)ss);
    if (!reader.ReadSectors(bootLBA, boot.data(), 1)) {
        std::cerr << "cannot read boot sector at LBA " << bootLBA << "\n";
        return false;
    }
    if (std::memcmp(boot.data() + 3, "NTFS    ", 8) != 0) {
        std::cerr << "LBA " << bootLBA << " is not an NTFS boot sector\n";
        return false;
    }
    uint64_t totalSectors = 0;
    std::memcpy(&totalSectors, boot.data() + 0x28, sizeof(totalSectors));
    part.totalSectors = totalSectors;
    part.sizeBytes    = totalSectors * ss;
    return true;
}

std::vector<FileNode> EnumerateNodes(SectorReader& reader, const Partition& part)
{
    NtfsParser parser;
    if (!parser.Open(reader, part)) {
        std::cerr << "NtfsParser::Open FAILED\n";
        return {};
    }
    if (!parser.ScanMetadata()) {
        std::cerr << "NtfsParser::ScanMetadata FAILED\n";
        return {};
    }
    return parser.GetAllFiles();
}

// ── P0-6: engine-owned file-format matching ────────────────────────────────
// The type decision is made ONLY by production engine code:
//   SignatureDB::Match()          – 513 built-in signatures, AC automaton
//   FormatPayloadScoreUtil        – engine structural scorer / magic probe
// No extension-name guessing, no Python-side type table.
struct EngineFormatHit
{
    std::wstring engineExt;    // SignatureDB::Match result (lower-case)
    std::wstring probeExt;     // FormatPayloadScoreUtil::ProbeExtension
    int          score    = -2; // -2 = not evaluated
    bool         readOk   = false;
};

class EngineFormatMatcher
{
public:
    EngineFormatMatcher() = default;
    size_t SignatureCount() const { return m_db.GetAll().size(); }

    EngineFormatHit Identify(const uint8_t* buf, size_t len,
                             const std::wstring& declaredExt) const
    {
        EngineFormatHit h;
        if (!buf || len == 0) return h;
        h.readOk = true;
        const FileSignature* sig = m_db.Match(buf, len);
        if (sig) h.engineExt = LowerW(sig->extension);
        h.probeExt = LowerW(FormatPayloadScoreUtil::ProbeExtension(buf, len));
        h.score = static_cast<int>(
            FormatPayloadScoreUtil::Score(LowerW(declaredExt), buf, len));
        return h;
    }
private:
    SignatureDB m_db;   // built-in signature set, exactly as in the GUI
};

static std::string Agreement(const EngineFormatHit& h, const std::wstring& declared)
{
    if (!h.readOk)        return "noPayload";
    if (h.engineExt.empty()) return "noSignature";
    if (declared.empty()) return "noDeclaredExt";
    return (h.engineExt == declared) ? "match" : "mismatch";
}

// Read the prefix of a file the ENGINE just wrote: no extra source-disk I/O.
static std::vector<uint8_t> ReadOutputPrefix(const std::wstring& path,
                                             uint32_t maxBytes)
{
    std::vector<uint8_t> buf;
    if (maxBytes == 0) return buf;
#if defined(_WIN32)
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return buf;
    buf.resize(maxBytes);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), maxBytes, &got, nullptr)) got = 0;
    CloseHandle(h);
    buf.resize(got);
#else
    std::ifstream f(std::string(path.begin(), path.end()), std::ios::binary);
    if (!f) return buf;
    buf.resize(maxBytes);
    f.read(reinterpret_cast<char*>(buf.data()), maxBytes);
    buf.resize(static_cast<size_t>(f.gcount()));
#endif
    return buf;
}

static std::unordered_set<uint64_t> LoadIdSet(const std::string& path)
{
    std::unordered_set<uint64_t> ids;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        char* end = nullptr;
        uint64_t v = std::strtoull(line.c_str(), &end, 10);
        if (end != line.c_str()) ids.insert(v);
    }
    return ids;
}

// Engine-driven discovery. This is the ONLY source of recovery candidates;
// the independent MFT oracle is used later strictly as a comparison truth.
int CmdScan(const Args& a)
{
    SectorReader reader;
    Partition part;
    if (!OpenVolume(reader, a.disk, a.image, a.boot, part)) return 3;

    auto files = EnumerateNodes(reader, part);   // NtfsParser::ScanMetadata
    std::cerr << "[engine] DISCOVERY NtfsParser::ScanMetadata/GetAllFiles nodes="
              << files.size() << " badSectors=" << reader.GetBadSectorCount()
              << "\n";

    EngineFormatMatcher fm;
    std::cerr << "[engine] FORMAT SignatureDB built-in signatures="
              << fm.SignatureCount() << "\n";

    const bool idsGiven = !a.ids.empty();
    const std::unordered_set<uint64_t> probeIds =
        idsGiven ? LoadIdSet(a.ids) : std::unordered_set<uint64_t>();
    uint64_t probes = 0, probedOk = 0;

    if (a.out.empty()) return 0;
    std::ofstream f(a.out, std::ios::binary | std::ios::trunc);
    if (!f) { std::cerr << "cannot write " << a.out << "\n"; return 4; }
    f << "id,name,path,size,isDir,isSystem,status,recoverability,"
         "clusterCount,firstCluster,residentSize,isCompressed,compressionUnit,"
         "declaredExt,engineExt,probeExt,formatScore,formatReadOk,"
         "formatAgreement,formatMatcher\n";

    uint64_t n = 0;
    for (const auto& x : files) {
        if (a.limit && n >= a.limit) break;

        bool doProbe = a.engineFormat && !x.isDirectory && x.size > 0 &&
                       !IsSystemNode(x);
        if (doProbe) {
            if (idsGiven) doProbe = probeIds.count(x.id) != 0;
            else if (a.formatSample && probes >= a.formatSample) doProbe = false;
        }
        EngineFormatHit hit;
        if (doProbe) {
            ++probes;
            std::vector<uint8_t> prefix;
            // Engine-side prefix decode (resident / compressed / runlist aware).
            if (RecoverEngine::ReadFileDataPrefix(reader, part, x,
                                                  a.formatProbeBytes, prefix)) {
                hit = fm.Identify(prefix.data(), prefix.size(),
                                  LowerW(x.extension));
                if (hit.readOk) ++probedOk;
            }
        }

        const std::wstring declared = LowerW(x.extension);
        f << x.id << ',' << Csv(Utf8(x.name)) << ',' << Csv(Utf8(x.originalPath))
          << ',' << x.size << ',' << (x.isDirectory ? 1 : 0)
          << ',' << (IsSystemNode(x) ? 1 : 0)
          << ',' << (int)x.status << ',' << (int)x.recoverability
          << ',' << x.clusters.size()
          << ',' << (x.clusters.empty() ? -1 : (int64_t)x.clusters.front())
          << ',' << x.residentData.size()
          << ',' << (x.isNtfsCompressed ? 1 : 0)
          << ',' << x.compressionUnit
          << ',' << Csv(Utf8(declared))
          << ',' << Csv(Utf8(hit.engineExt))
          << ',' << Csv(Utf8(hit.probeExt))
          << ',' << hit.score
          << ',' << (hit.readOk ? 1 : 0)
          << ',' << Csv(Agreement(hit, declared))
          << ',' << Csv("SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score")
          << '\n';
        ++n;
    }
    f.close();
    std::cerr << "[engine] scan wrote " << n << " nodes -> " << a.out
              << " | engineFormatProbes=" << probes << " prefixReadOk="
              << probedOk << "\n";
    return 0;
}

int CmdEnumerate(const Args& a)
{
    SectorReader reader;
    Partition part;
    if (!OpenVolume(reader, a.disk, a.image, a.boot, part)) return 3;
    auto files = EnumerateNodes(reader, part);
    std::cerr << "[engine] NTFS nodes=" << files.size()
              << " badSectors=" << reader.GetBadSectorCount() << "\n";

    if (a.out.empty()) return 0;
    std::ofstream f(a.out, std::ios::binary | std::ios::trunc);
    if (!f) { std::cerr << "cannot write " << a.out << "\n"; return 4; }
    f << "id,name,path,size,isDir,status,recoverability,clusterCount,"
         "firstCluster,residentSize,isCompressed,compressionUnit\n";
    uint64_t n = 0;
    for (const auto& x : files) {
        if (a.limit && n >= a.limit) break;
        f << x.id << ',' << Csv(Utf8(x.name)) << ',' << Csv(Utf8(x.originalPath))
          << ',' << x.size << ',' << (x.isDirectory ? 1 : 0)
          << ',' << (int)x.status << ',' << (int)x.recoverability
          << ',' << x.clusters.size()
          << ',' << (x.clusters.empty() ? -1 : (int64_t)x.clusters.front())
          << ',' << x.residentData.size()
          << ',' << (x.isNtfsCompressed ? 1 : 0)
          << ',' << x.compressionUnit << '\n';
        ++n;
    }
    f.close();
    std::cerr << "[engine] wrote " << n << " nodes -> " << a.out << "\n";
    return 0;
}

// P0-4: on the real disk some MFT paths contain BOTH a file and a directory
// with the same path segment (e.g. "...\QmSafeMNg.zip" as a 2.7 MB file and
// "...\QmSafeMNg.zip\hooks\commit-msg.sample" as its children). With
// keepDirStructure=true, RecoverEngine::WriteRecoveredFile ->
// Utils::CreateDirectoryRecursive then fails for every child ("Cannot create
// target directory"). Isolation rewrites ONLY the output path metadata
// (FileNode::originalPath); clusters / residentData / rawOffset (the actual
// data location) are untouched, so recovered bytes are identical.
// Default ON; --keep-original-paths 1 restores the collision-prone behaviour
// so the defect stays reproducible.
static std::wstring SafeLeaf(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|')
            out += L'_';
        else
            out += c;
    }
    if (out.empty()) out = L"unnamed";
    return out;
}

int CmdRecover(const Args& a)
{
    if (a.ids.empty() || a.outDir.empty()) {
        std::cerr << "recover needs --ids and --out-dir\n";
        return 2;
    }
    std::unordered_set<uint64_t> wanted;
    {
        std::ifstream f(a.ids);
        if (!f) { std::cerr << "cannot read " << a.ids << "\n"; return 4; }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            char* end = nullptr;
            uint64_t v = std::strtoull(line.c_str(), &end, 10);
            if (end != line.c_str()) wanted.insert(v);
        }
    }
    std::cerr << "[engine] requested ids=" << wanted.size() << "\n";

    SectorReader reader;
    Partition part;
    if (!OpenVolume(reader, a.disk, a.image, a.boot, part)) return 3;
    // Must be after Open(): Open() -> Close() invalidates any cached window.
    if (a.readAheadMB) {
        reader.SetReadAheadWindow(a.readAheadMB * 1024u * 1024u);
        std::cerr << "[engine] read-ahead window=" << a.readAheadMB << " MiB"
                  << " (opt-in; 0 in baseline batches)\n";
    }
    auto all = EnumerateNodes(reader, part);
    std::cerr << "[engine] parsed NTFS nodes=" << all.size() << "\n";

    std::vector<FileNode> selected;
    selected.reserve(wanted.size());
    for (const auto& x : all)
        if (wanted.count(x.id)) selected.push_back(x);
    std::cerr << "[engine] matched " << selected.size() << " of "
              << wanted.size() << " requested nodes\n";
    if (selected.empty()) return 0;

    if (!a.keepOriginalPaths) {
        size_t seq = 0;
        for (auto& n : selected) {
            const std::wstring prefix = L"f" + std::to_wstring(seq++) +
                                        L"_" + std::to_wstring(n.id) + L"_";
            n.originalPath = a.flatOutput ? prefix + SafeLeaf(n.name)
                                          : prefix.substr(0, prefix.size() - 1) +
                                            L"\\" + SafeLeaf(n.name);
        }
        std::cerr << "[engine] output path mode: "
                  << (a.flatOutput ? "flat isolated (no per-id directories)"
                                   : "isolated by id (collision-safe)")
                  << "\n";
    } else {
        std::cerr << "[engine] output path mode: original (collision-prone, "
                     "defect reproduction)\n";
    }

    RecoverOptions opts;
    opts.formatAwareFatFragments = false;
    RecoverEngine engine;
    engine.SetOptions(opts);

    // P0-6: the format decision belongs to the engine's signature database.
    EngineFormatMatcher fm;
    std::cerr << "[engine] FORMAT SignatureDB built-in signatures="
              << fm.SignatureCount() << " engineFormat="
              << (a.engineFormat ? "on" : "off") << "\n";

    auto res = engine.RecoverBatch(selected, Wide(a.outDir),
                                   true /* keepDirStructure */,
                                   reader, part);
    std::cerr << "[engine] succeeded=" << res.succeeded
              << " failed=" << res.failed
              << " skipped=" << res.skipped
              << " cancelled=" << res.cancelled
              << " badSectors=" << reader.GetBadSectorCount()
              << " readAheadHits=" << reader.GetReadAheadHits()
              << " readAheadFills=" << reader.GetReadAheadFills()
              << " sectorsRead=" << reader.GetSectorsRead() << "\n";

    if (!a.result.empty()) {
        std::ofstream f(a.result, std::ios::binary | std::ios::trunc);
        if (!f) { std::cerr << "cannot write " << a.result << "\n"; return 4; }
        f << "id,status,targetPath,reason,declaredExt,engineExt,probeExt,"
             "formatScore,formatReadOk,formatAgreement,formatMatcher\n";
        uint64_t fmtTotal = 0, fmtMatch = 0, fmtMismatch = 0, fmtNoSig = 0;
        for (const auto& r : res.details) {
            const std::wstring declared =
                r.node ? LowerW(r.node->extension) : std::wstring();
            EngineFormatHit hit;
            if (a.engineFormat && r.node &&
                (r.status == RecoverStatus::Success ||
                 r.status == RecoverStatus::Partial) &&
                !r.targetPath.empty()) {
                // The bytes just written by RecoverEngine are fed back into
                // SignatureDB::Match — engine recovers, engine classifies.
                std::vector<uint8_t> prefix =
                    ReadOutputPrefix(r.targetPath, a.formatProbeBytes);
                hit = fm.Identify(prefix.data(), prefix.size(), declared);
                ++fmtTotal;
                const std::string ag = Agreement(hit, declared);
                if      (ag == "match")      ++fmtMatch;
                else if (ag == "mismatch")   ++fmtMismatch;
                else if (ag == "noSignature")++fmtNoSig;
            }
            f << (r.node ? r.node->id : 0) << ','
              << (int)r.status << ','
              << Csv(Utf8(r.targetPath)) << ','
              << Csv(Utf8(r.reason)) << ','
              << Csv(Utf8(declared)) << ','
              << Csv(Utf8(hit.engineExt)) << ','
              << Csv(Utf8(hit.probeExt)) << ','
              << hit.score << ','
              << (hit.readOk ? 1 : 0) << ','
              << Csv(Agreement(hit, declared)) << ','
              << Csv("SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score")
              << '\n';
        }
        std::cerr << "[engine] FORMAT classified=" << fmtTotal
                  << " match=" << fmtMatch << " mismatch=" << fmtMismatch
                  << " noSignature=" << fmtNoSig << "\n";
    }
    return (res.failed > 0) ? 1 : 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// CmdVerify — ENGINE-SIDE verification of recovered files.
//
// The oracle hash is useful as an independent cross-check, but the verdict
// must also be reproducible with product code. This command re-opens the raw
// image with the production SectorReader, re-discovers nodes with the same
// NtfsParser path used by `scan`, rebuilds each file's bytes from the engine's
// own runlist, and compares them byte-for-byte against what `recover` wrote.
// No Python, no oracle CSV, no candidate source other than the engine result.
// ─────────────────────────────────────────────────────────────────────────────
int CmdVerify(const Args& a)
{
    if (a.result.empty() || a.out.empty()) {
        std::cerr << "verify needs --result <recover csv> and --out <verify csv>\n";
        return 2;
    }
    const std::vector<VerifyRow> rows = ReadRecoverResultRows(a.result, a.ids);
    std::cerr << "[engine] verify rows=" << rows.size() << "\n";

    SectorReader reader;
    Partition part;
    if (!OpenVolume(reader, a.disk, a.image, a.boot, part)) return 3;
    if (a.readAheadMB) {
        reader.SetReadAheadWindow(a.readAheadMB * 1024u * 1024u);
        std::cerr << "[engine] verify read-ahead=" << a.readAheadMB << " MiB\n";
    }

    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot(ss);
    if (!reader.ReadSectors(a.boot, boot.data(), 1)) return 3;
    uint32_t bps = (uint32_t)boot[11] | ((uint32_t)boot[12] << 8);
    uint32_t spc = boot[13];
    if (!bps) bps = ss;
    if (!spc) spc = 1;
    const uint32_t bytesPerCluster = bps * spc;

    const std::vector<FileNode> all = EnumerateNodes(reader, part);
    std::unordered_map<uint64_t, std::vector<const FileNode*>> byId;
    for (const FileNode& n : all) byId[n.id].push_back(&n);

    std::ofstream out(a.out, std::ios::binary | std::ios::trunc);
    if (!out) { std::cerr << "cannot write " << a.out << "\n"; return 4; }
    out << "id,status,targetPath,reason,bytes,firstDiffOffset\n";

    uint64_t matched = 0, mismatched = 0, skipped = 0, failed = 0, bytes = 0;
    constexpr size_t CHUNK = 1u << 20;
    std::vector<uint8_t> imgBuf(CHUNK), fileBuf(CHUNK);

    for (const VerifyRow& row : rows) {
        const auto it = byId.find(row.id);
        const FileNode* node = nullptr;
        if (it != byId.end()) {
            // An id may have several NTFS stream nodes. Prefer the node whose
            // recovered leaf name matches, exactly like `recover` did.
            std::string leaf = row.path.substr(
                row.path.find_last_of("/\\") == std::string::npos
                    ? 0 : row.path.find_last_of("/\\") + 1);
            // Flat output leaves are "f<seq>_<id>_<original leaf>".
            if (!leaf.empty() && leaf[0] == 'f') {
                const size_t p1 = leaf.find('_');
                const size_t p2 = p1 == std::string::npos
                                      ? std::string::npos
                                      : leaf.find('_', p1 + 1);
                if (p1 != std::string::npos && p2 != std::string::npos) {
                    leaf = leaf.substr(p2 + 1);
                }
            }
            for (const FileNode* n : it->second) {
                if (Utf8(SafeLeaf(n->name)) == leaf) { node = n; break; }
            }
            if (!node) node = it->second.front();
        }
        if (!node) {
            out << row.id << ",skip," << Csv(row.path)
                << ",\"no engine node\",0,0\n";
            ++skipped;
            continue;
        }
        if (node->isNtfsCompressed) {
            out << row.id << ",skip," << Csv(row.path)
                << ",\"ntfs compressed\",0,0\n";
            ++skipped;
            continue;
        }

        // MinGW's libstdc++ has no wide-path std::ifstream constructor; use
        // the CRT wide-path API so Chinese file names open correctly.
        FILE* f = _wfopen(Wide(row.path).c_str(), L"rb");
        if (!f) {
            out << row.id << ",error," << Csv(row.path)
                << ",\"recovered file missing\",0,0\n";
            ++failed;
            continue;
        }

        uint64_t      done      = 0;
        uint64_t      firstDiff = 0;
        bool          ok        = true;
        std::string   reason;

        auto compareChunk = [&](const uint8_t* a, const uint8_t* b, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                if (a[i] != b[i]) {
                    ok = false;
                    firstDiff = done + i;
                    return;
                }
            }
        };

        if (!node->residentData.empty()) {
            const size_t take = (size_t)std::min<uint64_t>(node->size,
                                        node->residentData.size());
            if (take) {
                if (std::fread(fileBuf.data(), 1, take, f) != take) {
                    ok = false;
                    reason = "short file";
                }
                else compareChunk(node->residentData.data(), fileBuf.data(), take);
                done = take;
            }
            if (ok && done != node->size) { ok = false; reason = "resident short"; }
        } else if (node->size > 0 && node->clusters.empty()) {
            ok = false;
            reason = "no runlist";
        } else {
            for (uint64_t clus : node->clusters) {
                if (done >= node->size) break;
                uint64_t inCluster = 0;
                const uint64_t span = std::min<uint64_t>(bytesPerCluster,
                                                          node->size - done);
                const uint64_t byteOff = part.startSector * (uint64_t)ss +
                                         clus * (uint64_t)bytesPerCluster;
                while (inCluster < span && ok) {
                    const size_t take = (size_t)std::min<uint64_t>(CHUNK,
                                                        span - inCluster);
                    if (clus == 0) {
                        std::memset(imgBuf.data(), 0, take);
                    } else if (!reader.ReadBytes(byteOff + inCluster,
                                                 imgBuf.data(), (uint32_t)take)) {
                        ok = false;
                        reason = "image read failed";
                        break;
                    }
                    if (std::fread(fileBuf.data(), 1, take, f) != take) {
                        ok = false;
                        reason = "short file";
                        break;
                    }
                    compareChunk(imgBuf.data(), fileBuf.data(), take);
                    done += take;
                    inCluster += take;
                }
                if (!ok) break;
            }
        }
        if (ok && done != node->size) { ok = false; reason = "size mismatch"; }
        std::fclose(f);

        if (ok) {
            ++matched;
            bytes += done;
        } else if (reason.empty()) {
            ++mismatched;
            reason = "byte mismatch";
        } else {
            ++failed;
        }
        out << row.id << ',' << (ok ? "match" : (reason == "byte mismatch" ? "mismatch" : "error")) << ','
            << Csv(row.path) << ',' << Csv(reason) << ',' << done << ','
            << firstDiff << '\n';

        if ((matched + mismatched + skipped + failed) % 10000 == 0) {
            std::cerr << "[engine] verify progress match=" << matched
                      << " mismatch=" << mismatched << " skip=" << skipped
                      << " error=" << failed << " bytes=" << bytes << "\n";
        }
    }
    std::cerr << "[engine] VERIFY DONE match=" << matched
              << " mismatch=" << mismatched << " skip=" << skipped
              << " error=" << failed << " bytes=" << bytes
              << " -> " << a.out << "\n";
    return (mismatched || failed) ? 1 : 0;
}

int main(int argc, char** argv)
{
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::cerr << "usage: phoenix_headless scan|enumerate|recover|verify "
                     "--disk N | --image <raw> --boot LBA [--out csv] [--ids txt] "
                     "[--out-dir dir] [--result csv] [--limit N] "
                     "[--read-ahead-mb N] [--engine-format 0|1] "
                     "[--format-probe-bytes N] [--format-sample N]\n";
        return 2;
    }
    if (a.cmd == "scan")      return CmdScan(a);
    if (a.cmd == "enumerate") return CmdEnumerate(a);
    if (a.cmd == "recover")   return CmdRecover(a);
    if (a.cmd == "verify")    return CmdVerify(a);
    std::cerr << "unknown command: " << a.cmd << "\n";
    return 2;
}
