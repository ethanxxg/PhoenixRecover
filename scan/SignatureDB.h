#pragma once

#include <string>
#include <vector>
#include <cstdint>

// AC-backed header matching (zero-dep, header-only, C++17). The matcher is built
// once from the signature set and replaces the O(patterns × bytes) linear memcmp
// scan previously done inside Match(). RawScan / CarveModuleService transparently
// benefit; see SignatureMatcher.h for the streaming Aho-Corasick design.
#include "SignatureMatcher.h"

// ── One file-type signature entry ─────────────────────────────────────────
struct FileSignature
{
    std::wstring extension;              // e.g. L"jpg"
    std::vector<uint8_t> header;         // Primary magic bytes
    int                  headerOffset = 0; // Byte offset where primary magic appears
    std::vector<uint8_t> footer;         // Optional end-of-file marker
    uint64_t             maxSize = 50 * 1024 * 1024; // Default 50 MB max

    // Optional secondary pattern for sub-type disambiguation (e.g. RIFF→WAV/AVI/WebP)
    std::vector<uint8_t> secondary;
    int                  secondaryOffset = -1; // -1 = no secondary check
};

// ── Signature database ────────────────────────────────────────────────────
class SignatureDB
{
public:
    SignatureDB();

    // Build a database containing EXACTLY 'sigs' — no built-in signatures.
    // A1-10: each parallel RAW-scan worker needs a private DB whose signature
    // set is byte-for-byte identical to the effective (possibly custom or
    // extension-filtered) DB used by the serial path.  SignatureMatcher /
    // ScanBatch keep mutable scratch state, so sharing one DB across threads
    // is not an option.  The AC matcher is still built lazily on first use.
    explicit SignatureDB(const std::vector<FileSignature>& sigs);

    const std::vector<FileSignature>& GetAll() const { return m_sigs; }

    // Load additional signatures from a JSON file and APPEND them to the
    // built-in set.  Qt-free (A1-9): implemented with std::ifstream +
    // SignatureJson.h, so headless / non-GUI builds behave identically to GUI
    // builds.  Schema: SIGNATURE_FORMAT.md.  Invalid entries are skipped with
    // a warning; returns the number appended, or -1 on file / JSON / structure
    // error.  Clears the cached AC matcher so it is rebuilt lazily.
    int LoadFromFile(const std::wstring& path);

    // Add a single signature programmatically
    void Add(FileSignature sig) { m_sigs.push_back(std::move(sig)); m_matcherBuilt = false; }

    // Find matching signature for the first bytes of a sector
    // Returns nullptr if no match
    const FileSignature* Match(const uint8_t* buf, size_t bufLen) const;

    // ── Streaming batch discovery (A1-6 / parallel-track) ──────────────────
    // Single O(N) pass over `buf`: emits one candidate per stride-aligned file
    // start — exactly the positions the legacy per-stride Match() loop probed.
    // When several magics share a start the lowest-priority-index (== earliest
    // DB order) signature wins, matching Match()'s tie-break. `baseOffset` is
    // the disk offset of buf[0]; emitted fileStart values are in disk space.
    struct ScanCandidate
    {
        uint64_t             fileStart   = 0;   // disk offset of the file start
        const FileSignature* sig         = nullptr;
        bool                 secondaryOk = false;
    };
    void ScanBatch(const uint8_t* buf, size_t bufLen, uint64_t baseOffset,
                   uint64_t alignment, std::vector<ScanCandidate>& out) const;

    // ── Test-only switch: force AC or legacy linear matching ──────────────────
    // Used by CTest to prove the AC-backed Match is behavior-identical to the
    // original linear scan over real signature sets.
    enum class MatchImpl { AC, Linear };
    static void SetMatchImplForTesting(MatchImpl m);

private:
    // Build the Aho-Corasick matcher from m_sigs (lazily, once per signature-set
    // change). Empty-header signatures cannot live in the trie and are flagged so
    // Match() can fall back to the linear reference for them only.
    void EnsureMatcher() const;

    // Faithful copy of the pre-AC linear scan (kept for fallback + regression).
    static const FileSignature* LinearMatch(const std::vector<FileSignature>& sigs,
                                            const uint8_t* buf, size_t bufLen);

    static MatchImpl s_matchImpl;

    mutable phoenix::SignatureMatcher        m_matcher;
    mutable std::vector<phoenix::SignatureHit> m_matchScratch;
    mutable bool                            m_matcherBuilt   = false;
    mutable bool                            m_matcherHasOmitted = false;
    // Longest span (header at headerOffset, or secondary at secondaryOffset) we
    // must scan to answer a point lookup. Match() only feeds this many bytes to
    // the automaton, keeping each call O(maxMagicLen) instead of O(bufLen).
    mutable size_t                         m_maxScanLen = 16;

    static std::vector<uint8_t> HexToBytes(const std::string& hex);
    static bool MatchPattern(const uint8_t* buf, size_t bufLen,
                             const std::vector<uint8_t>& pattern, int offset);

    std::vector<FileSignature> m_sigs;
};
