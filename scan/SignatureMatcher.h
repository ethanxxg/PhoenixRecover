#pragma once
// ---------------------------------------------------------------------------
// SignatureMatcher.h — streaming Aho-Corasick multi-pattern signature matcher
//
// Replaces the O(patterns x bytes) linear memcmp scan used by SignatureDB /
// RawScan with a single O(bytes) pass over an Aho-Corasick automaton built
// from every file-type magic. Designed for RAW carving where the source is a
// byte stream (disk image, partition) and files are located by their header
// magic, optionally bounded by a footer, secondary sub-type pattern, alignment
// and a per-type size cap.
//
// Streaming model: call Feed() once per window with the absolute offset of the
// window. The automaton state is continuous across windows; up to (maxDepth-1)
// trailing bytes of the previous window are re-fed as context so a magic that
// crosses a window boundary is still found exactly once. Header candidates are
// collected during scanning and secondary/footer validation is *deferred* — each
// later window re-checks still-pending candidates against its newly available
// bytes, so footer/secondary located megabytes after the header are correctly
// resolved without buffering the whole stream. Call Finish() after the last
// window to flush any still-pending candidates.
//
// Zero dependency: no Qt, no Win32, no I/O. Header-only, C++17.
// The AC construction follows the canonical algorithm so an independent Python
// oracle can cross-check the emitted match set (see testdata golden corpus).
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

namespace phoenix {

// One matchable magic pattern (maps 1:1 onto a SignatureDB entry).
struct MatchPattern
{
    std::wstring            extension;        // type id, e.g. L"jpg"
    std::vector<uint8_t>   header;           // primary magic bytes (must be non-empty)
    int                    headerOffset = 0;  // byte offset of magic within the file
    std::vector<uint8_t>   secondary;        // optional sub-type disambiguation
    int                    secondaryOffset = -1; // -1 = none
    std::vector<uint8_t>   footer;           // optional end-of-file marker
    uint64_t               maxSize = 50ULL * 1024ULL * 1024ULL; // max span to search for footer
    int                    priority = 0;     // higher wins on same-offset conflict
};

// A located signature occurrence.
struct SignatureHit
{
    uint32_t     patternId    = 0;     // index into the pattern vector passed to Build()
    std::wstring extension;
    uint64_t     fileStart    = 0;     // byte offset of the file start (magic - headerOffset)
    uint64_t     magicOffset  = 0;     // == fileStart + headerOffset
    uint32_t     headerLength = 0;
    bool         secondaryOk  = false; // secondary matched (always true if no secondary)
    bool         footerOk     = false; // footer found within maxSize (always true if no footer)
    int          priority     = 0;
};

// Scan-time knobs shared across a whole pass.
struct MatchOptions
{
    uint64_t alignment = 1;            // a hit is reported only if fileStart % alignment == 0
    uint64_t maxSize   = UINT64_MAX;  // global cap on any single file span
    // Empty = match every type. Otherwise only these extensions are emitted.
    std::vector<std::wstring> allowedExtensions;
    bool     includeFooterSearch = true; // if false, footerOk stays false (faster)
};

class SignatureMatcher
{
public:
    // Build the automaton. Returns false if any pattern has an empty header
    // (an empty magic cannot be placed in a trie) or if the pattern set is empty.
    bool Build(std::vector<MatchPattern> patterns)
    {
        m_patterns = std::move(patterns);
        if (m_patterns.empty()) return false;
        for (const auto& p : m_patterns)
            if (p.header.empty()) return false;

        m_nodes.clear();
        m_nodes.push_back(Node{});            // root = 0
        m_maxDepth = 1;
        for (int id = 0; id < static_cast<int>(m_patterns.size()); ++id) {
            const auto& h = m_patterns[id].header;
            int cur = 0;
            for (uint8_t c : h) {
                int nxt = m_nodes[cur].child[c];
                if (nxt == 0) {
                    nxt = static_cast<int>(m_nodes.size());
                    m_nodes.push_back(Node{});
                    m_nodes[cur].child[c] = nxt;
                }
                m_nodes[nxt].depth = m_nodes[cur].depth + 1;
                cur = nxt;
            }
            if (static_cast<size_t>(m_nodes[cur].depth + 1) > m_maxDepth)
                m_maxDepth = static_cast<size_t>(m_nodes[cur].depth + 1);
            m_nodes[cur].out.push_back(id);
        }
        BuildTransitions();
        Reset();
        return true;
    }

    // Reset streaming state (overlap buffer, automaton state, pending candidates).
    void Reset()
    {
        m_state = 0;
        m_overlap.clear();
        m_overlapBase = 0;
        m_pending.clear();
    }

    // Feed a window of bytes. Hits are appended to `out` as candidates are
    // resolved. `baseOffset` is the absolute byte offset of data[0] within the
    // scanned stream.
    void Feed(const uint8_t* data, size_t length, uint64_t baseOffset,
              const MatchOptions& opt, std::vector<SignatureHit>& out)
    {
        if (m_nodes.empty()) return;

        const bool hasOverlap = !m_overlap.empty();
        const size_t total = m_overlap.size() + length;

        // Zero-copy fast path: when there is no pending overlap (the common
        // single-window case — e.g. SignatureDB::ScanBatch, which Reset()s before
        // each call and prepends any cross-batch context itself), stream directly
        // over `data` instead of copying it into a combined buffer.
        const uint8_t* combinedData;
        std::vector<uint8_t> combined;
        if (hasOverlap) {
            combined.resize(total);
            for (size_t k = 0; k < total; ++k)
                combined[k] = (k < m_overlap.size()) ? m_overlap[k]
                                                    : data[k - m_overlap.size()];
            combinedData = combined.data();
        } else {
            combinedData = data;
        }

        const uint64_t availBase = m_overlapBase;
        const uint64_t availEnd  = baseOffset + static_cast<uint64_t>(length);

        // Pass 1 — state machine over the full combined stream. Re-feeding the
        // overlap reproduces the exact boundary state. Emit a candidate only when
        // its match ENDS inside the new data region (absPos >= baseOffset).
        struct RawCand { int id; uint64_t magicEnd; };
        std::vector<RawCand> raw;
        for (size_t k = 0; k < total; ++k) {
            m_state = m_nodes[m_state].go[combinedData[k]];
            const uint64_t absPos = availBase + static_cast<uint64_t>(k);
            if (absPos < baseOffset) continue;        // context only (overlap tail)
            for (int id : m_nodes[m_state].out) {
                const uint32_t L = static_cast<uint32_t>(m_patterns[id].header.size());
                if (L == 0 || absPos + 1 < L) continue;
                const uint64_t magicStart = absPos + 1 - L;
                const int64_t fs = static_cast<int64_t>(magicStart) -
                                   static_cast<int64_t>(m_patterns[id].headerOffset);
                if (fs < 0) continue;
                raw.push_back({id, absPos});
            }
        }

        // Register raw candidates as pending (after alignment / type filtering).
        for (const auto& c : raw) {
            const MatchPattern& p = m_patterns[c.id];
            const uint32_t L = static_cast<uint32_t>(p.header.size());
            const uint64_t magicStart = c.magicEnd + 1 - L;
            const uint64_t fileStart = magicStart - static_cast<uint64_t>(p.headerOffset);
            if (opt.alignment > 1 && (fileStart % opt.alignment) != 0) continue;
            if (!opt.allowedExtensions.empty() &&
                !Contains(opt.allowedExtensions, p.extension))
                continue;
            const uint64_t spanCap = (opt.maxSize < p.maxSize) ? opt.maxSize : p.maxSize;
            Pending pend;
            pend.id        = c.id;
            pend.fileStart = fileStart;
            pend.deadline  = fileStart + spanCap;
            pend.secDone   = p.secondary.empty() || p.secondaryOffset < 0;
            pend.footDone  = p.footer.empty() || !opt.includeFooterSearch;
            pend.secOk     = true;
            pend.footOk    = true;
            m_pending.push_back(pend);
        }

        // Pass 2 — resolve pending candidates against the bytes available now.
        ResolvePending(combinedData, total, availBase, availEnd, opt, out);

        // Update overlap to the last (maxDepth-1) bytes of the combined stream.
        const size_t keep = (m_maxDepth > 1) ? (m_maxDepth - 1) : 0;
        if (keep == 0) {
            m_overlap.clear();
            m_overlapBase = availEnd;
        } else if (total <= keep) {
            m_overlap.assign(combinedData, combinedData + total);
            m_overlapBase = availBase;
        } else {
            m_overlap.assign(combinedData + (total - keep), combinedData + total);
            m_overlapBase = availBase + static_cast<uint64_t>(total - keep);
        }
    }

    // Flush any candidates still pending after the final Feed().
    void Finish(const MatchOptions& opt, std::vector<SignatureHit>& out)
    {
        if (m_pending.empty()) return;
        // Build a trivial [0,0) buffer: nothing new is available, so any still
        // pending footer/secondary past the stream end resolves as not-found.
        // Trivial [0,0) buffer: nothing new is visible, so any still-pending
        // footer/secondary past the stream end resolves as not-found.
        ResolvePending(nullptr, 0, 0, 0, opt, out, /*finalize=*/true);
        m_pending.clear();
    }

    // Convenience: scan a single in-memory buffer as one continuous pass.
    // Streaming state (m_state / m_overlap / m_pending) is fully reset at the
    // start, and Feed()/Finish() never mutate m_patterns / m_nodes / m_maxDepth,
    // so scanning *in place* is safe and avoids copying the (potentially large)
    // automaton on every call — critical for RawScan's per-sector header
    // discovery path, which calls this millions of times.
    void Scan(const uint8_t* data, size_t length, uint64_t baseOffset,
              const MatchOptions& opt, std::vector<SignatureHit>& out) const
    {
        const_cast<SignatureMatcher*>(this)->Reset();
        const_cast<SignatureMatcher*>(this)->Feed(data, length, baseOffset, opt, out);
        const_cast<SignatureMatcher*>(this)->Finish(opt, out);
    }

    size_t PatternCount() const { return m_patterns.size(); }
    size_t StateCount()   const { return m_nodes.size(); }
    size_t MaxDepth()     const { return m_maxDepth; }

private:
    struct Node
    {
        int child[256] = {0};  // trie children (0 = none)
        int go[256]    = {0};  // full delta transition table
        int fail       = 0;
        std::vector<int> out;  // pattern ids ending here
        int depth      = 0;
    };

    struct Pending
    {
        int      id        = 0;
        uint64_t fileStart = 0;
        uint64_t deadline  = 0;   // fileStart + effective maxSize
        bool     secDone   = false;
        bool     footDone  = false;
        bool     secOk     = true;
        bool     footOk    = true;
    };

    void BuildTransitions()
    {
        std::queue<int> q;
        for (int c = 0; c < 256; ++c) {
            const int nxt = m_nodes[0].child[c];
            m_nodes[0].go[c] = (nxt != 0) ? nxt : 0;
            if (nxt != 0) { m_nodes[nxt].fail = 0; q.push(nxt); }
        }
        while (!q.empty()) {
            const int r = q.front(); q.pop();
            for (int c = 0; c < 256; ++c) {
                const int nxt = m_nodes[r].child[c];
                if (nxt != 0) {
                    m_nodes[r].go[c] = nxt;   // direct child: delta = child
                    m_nodes[nxt].fail = m_nodes[m_nodes[r].fail].go[c];
                    q.push(nxt);
                } else {
                    m_nodes[r].go[c] = m_nodes[m_nodes[r].fail].go[c];
                }
            }
            // Output propagation: a node also emits what its fail link emits.
            // BFS order guarantees fail[r] was processed before r.
            const auto& fout = m_nodes[m_nodes[r].fail].out;
            if (!fout.empty())
                for (int id : fout) m_nodes[r].out.push_back(id);
        }
    }

    static bool CheckAt(const uint8_t* buf, size_t bufLen, uint64_t bufBase,
                        uint64_t off, const std::vector<uint8_t>& pat)
    {
        if (pat.empty()) return true;
        if (off < bufBase) return false;
        if (off + pat.size() > bufBase + bufLen) return false;
        const size_t k = static_cast<size_t>(off - bufBase);
        for (size_t i = 0; i < pat.size(); ++i)
            if (buf[k + i] != pat[i]) return false;
        return true;
    }

    // Search for `pat` starting at work.fileStart within the bytes currently
    // visible [bufBase, bufEnd), bounded by spanCap. Returns true if the footer
    // is found in the visible intersection of the search span and the window.
    static bool ScanForFooter(const uint8_t* buf, size_t bufLen, uint64_t bufBase,
                             uint64_t bufEnd, uint64_t fileStart,
                             const std::vector<uint8_t>& pat, uint64_t spanCap)
    {
        if (pat.empty()) return true;
        const uint64_t searchEnd = fileStart + spanCap;
        const uint64_t lo = (fileStart > bufBase) ? fileStart : bufBase;
        const uint64_t hi = (searchEnd < bufEnd) ? searchEnd : bufEnd;
        if (hi <= lo) return false;
        if (lo + pat.size() > hi) return false;
        for (uint64_t pos = lo; pos + pat.size() <= hi; ++pos) {
            const size_t p = static_cast<size_t>(pos - bufBase);
            bool found = true;
            for (size_t i = 0; i < pat.size(); ++i)
                if (buf[p + i] != pat[i]) { found = false; break; }
            if (found) return true;
        }
        return false;
    }

    void ResolvePending(const uint8_t* buf, size_t bufLen, uint64_t bufBase,
                        uint64_t bufEnd, const MatchOptions& opt,
                        std::vector<SignatureHit>& out, bool finalize = false)
    {
        std::vector<Pending> stillPending;
        for (const Pending& pend : m_pending) {
            const MatchPattern& p = m_patterns[pend.id];
            const uint64_t spanCap = (opt.maxSize < p.maxSize) ? opt.maxSize : p.maxSize;
            Pending work = pend;

            // Secondary sub-type pattern (deferred until its bytes are visible).
            if (!work.secDone) {
                const uint64_t off = work.fileStart +
                                     static_cast<uint64_t>(p.secondaryOffset);
                if (off + p.secondary.size() <= bufEnd) {
                    work.secOk   = CheckAt(buf, bufLen, bufBase, off, p.secondary);
                    work.secDone = true;
                } else if (off >= pend.deadline || finalize) {
                    work.secOk   = false;
                    work.secDone = true;
                }
            }

            // Footer (deferred; found-so-far is OR-accumulated across windows).
            if (!work.footDone) {
                if (p.footer.empty() || !opt.includeFooterSearch) {
                    work.footOk   = true;
                    work.footDone = true;
                } else {
                    if (ScanForFooter(buf, bufLen, bufBase, bufEnd, work.fileStart,
                                      p.footer, spanCap))
                        work.footOk = true;
                    // The whole search span is now visible once the stream has
                    // advanced past fileStart+spanCap; nothing later can find it.
                    const bool covered = (work.fileStart + spanCap) <= bufEnd;
                    if (covered || finalize) work.footDone = true;
                }
            }

            if (work.secDone && work.footDone) {
                SignatureHit h;
                h.patternId    = static_cast<uint32_t>(work.id);
                h.extension    = p.extension;
                h.fileStart    = work.fileStart;
                h.magicOffset  = work.fileStart + static_cast<uint64_t>(p.headerOffset);
                h.headerLength = static_cast<uint32_t>(p.header.size());
                h.secondaryOk  = work.secOk;
                h.footerOk     = work.footOk;
                h.priority     = p.priority;
                out.push_back(std::move(h));
            } else {
                stillPending.push_back(work);
            }
        }
        m_pending = std::move(stillPending);
    }

    static bool Contains(const std::vector<std::wstring>& v, const std::wstring& s)
    {
        for (const auto& x : v) if (x == s) return true;
        return false;
    }

    std::vector<MatchPattern> m_patterns;
    std::vector<Node>         m_nodes;
    size_t                    m_maxDepth = 1;
    int                       m_state = 0;
    std::vector<uint8_t>      m_overlap;
    uint64_t                  m_overlapBase = 0;
    std::vector<Pending>      m_pending;
};

// Adapter: build a matcher directly from a SignatureDB (extension only; the
// .cpp-side DB owns the entries, this header just reads the public vector).
// Enable by defining SIGNATUREMATCHER_WITH_SIGDB before including SignatureDB.h
// and this header.
#ifdef SIGNATUREMATCHER_WITH_SIGDB
inline bool BuildMatcherFromSignatureDB(const SignatureDB& db, SignatureMatcher& out)
{
    std::vector<MatchPattern> pats;
    for (const auto& s : db.GetAll()) {
        if (s.header.empty()) continue;
        MatchPattern p;
        p.extension       = s.extension;
        p.header          = s.header;
        p.headerOffset    = s.headerOffset;
        p.secondary       = s.secondary;
        p.secondaryOffset = s.secondaryOffset;
        p.footer          = s.footer;
        p.maxSize         = s.maxSize;
        pats.push_back(std::move(p));
    }
    return out.Build(std::move(pats));
}
#endif

} // namespace phoenix
