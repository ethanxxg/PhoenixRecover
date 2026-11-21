#include "GraphCarveFragmentStitcher.h"

#include "FormatPayloadScore.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

static std::wstring LowerExt(std::wstring ext)
{
    for (auto& c : ext) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return ext;
}

static int ScoreToInt(FormatPayloadScore s)
{
    return static_cast<int>(s);
}

} // namespace

GraphCarveFragmentStitcher::GraphCarveFragmentStitcher(SectorReader& reader) : m_reader(reader) {}

bool GraphCarveFragmentStitcher::LoadBlocks(const std::vector<CarveFragmentBlock>& blocks,
                                            uint32_t maxBytesPerBlock,
                                            std::vector<LoadedBlock>& out) const
{
    out.clear();
    out.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.size == 0) {
            return false;
        }
        LoadedBlock lb;
        lb.span = b;
        const uint64_t toRead = std::min(b.size, static_cast<uint64_t>(maxBytesPerBlock));
        lb.bytes.resize(static_cast<size_t>(toRead));
        if (!m_reader.ReadBytes(b.diskOffset, lb.bytes.data(),
                                static_cast<uint32_t>(toRead))) {
            return false;
        }
        out.push_back(std::move(lb));
    }
    return !out.empty();
}

bool GraphCarveFragmentStitcher::HasFormatHeader(const std::wstring& extLower,
                                               const uint8_t* data,
                                               size_t len) const
{
    if (!data || len < 4) {
        return false;
    }
    const std::wstring probed = FormatPayloadScoreUtil::ProbeExtension(data, len);
    if (!probed.empty() && probed == extLower) {
        return true;
    }
    if (extLower == L"jpg" || extLower == L"jpeg") {
        return data[0] == 0xFF && data[1] == 0xD8;
    }
    if (extLower == L"pdf") {
        return len >= 5 && memcmp(data, "%PDF-", 5) == 0;
    }
    if (extLower == L"zip" || extLower == L"docx" || extLower == L"xlsx" || extLower == L"pptx") {
        return data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04;
    }
    if (extLower == L"png") {
        return len >= 8 && data[0] == 0x89 && data[1] == 'P';
    }
    return FormatPayloadScoreUtil::Score(extLower, data, len) != FormatPayloadScore::Invalid;
}

bool GraphCarveFragmentStitcher::CanLink(const std::wstring& extLower,
                                         const LoadedBlock& a,
                                         const LoadedBlock& b) const
{
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) {
        return true;
    }

    constexpr size_t kEdge = 256;
    const size_t tailLen = std::min(a.bytes.size(), kEdge);
    const size_t headLen = std::min(b.bytes.size(), kEdge);
    if (tailLen == 0 || headLen == 0) {
        return false;
    }

    std::vector<uint8_t> probe;
    probe.reserve(tailLen + headLen);
    probe.insert(probe.end(), a.bytes.end() - tailLen, a.bytes.end());
    probe.insert(probe.end(), b.bytes.begin(), b.bytes.begin() + headLen);

    const auto s = FormatPayloadScoreUtil::Score(extLower, probe.data(), probe.size());
    return s != FormatPayloadScore::Invalid;
}

int GraphCarveFragmentStitcher::ScoreOrdered(const std::wstring& extLower,
                                             const std::vector<LoadedBlock>& loaded,
                                             const std::vector<int>& order) const
{
    if (order.empty()) {
        return -1;
    }

    uint64_t total = 0;
    for (int idx : order) {
        total += loaded[static_cast<size_t>(idx)].span.size;
    }

    std::vector<uint8_t> buf;
    const size_t cap = static_cast<size_t>(std::min<uint64_t>(total, 8u * 1024u * 1024u));
    buf.reserve(cap);

    std::vector<uint8_t> chunk;
    for (int idx : order) {
        if (buf.size() >= cap) {
            break;
        }
        const auto& lb = loaded[static_cast<size_t>(idx)];
        const size_t take = static_cast<size_t>(
            std::min(lb.span.size, static_cast<uint64_t>(cap - buf.size())));
        chunk.resize(take);
        if (!m_reader.ReadBytes(lb.span.diskOffset, chunk.data(),
                                static_cast<uint32_t>(take))) {
            return -1;
        }
        buf.insert(buf.end(), chunk.begin(), chunk.end());
    }

    return ScoreToInt(FormatPayloadScoreUtil::Score(extLower, buf.data(), buf.size()));
}

bool GraphCarveFragmentStitcher::SearchBestOrder(const std::wstring& extLower,
                                                 const std::vector<LoadedBlock>& loaded,
                                                 std::vector<int>& outOrder,
                                                 int& outScore,
                                                 const FragmentStitchPolicy& policy) const
{
    const int n = static_cast<int>(loaded.size());
    if (n <= 0) {
        return false;
    }
    if (n == 1) {
        outOrder = { 0 };
        outScore = ScoreOrdered(extLower, loaded, outOrder);
        return outScore >= 0;
    }

    const auto budgetStart = std::chrono::steady_clock::now();
    const int64_t budgetMs = policy.graphBudgetMs > 0 ? policy.graphBudgetMs : 50;

    int bestScore = -1;
    std::vector<int> bestOrder;
    std::vector<int> path;
    path.reserve(static_cast<size_t>(n));
    std::vector<bool> used(static_cast<size_t>(n), false);

    std::vector<int> starts;
    starts.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (HasFormatHeader(extLower, loaded[static_cast<size_t>(i)].bytes.data(),
                            loaded[static_cast<size_t>(i)].bytes.size())) {
            starts.push_back(i);
        }
    }
    if (starts.empty()) {
        for (int i = 0; i < n; ++i) {
            starts.push_back(i);
        }
    }

    const auto timedOut = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - budgetStart)
                   .count() >= budgetMs;
    };

    for (int startIdx : starts) {
        if (timedOut()) {
            break;
        }

        path.clear();
        std::fill(used.begin(), used.end(), false);
        path.push_back(startIdx);
        used[static_cast<size_t>(startIdx)] = true;

        while (static_cast<int>(path.size()) < n) {
            if (timedOut()) {
                break;
            }

            int bestNext = -1;
            int bestNextScore = -1;
            const int last = path.back();

            for (int j = 0; j < n; ++j) {
                if (used[static_cast<size_t>(j)]) {
                    continue;
                }
                if (!CanLink(extLower, loaded[static_cast<size_t>(last)],
                             loaded[static_cast<size_t>(j)])) {
                    continue;
                }
                path.push_back(j);
                const int sc = ScoreOrdered(extLower, loaded, path);
                path.pop_back();
                if (sc > bestNextScore) {
                    bestNextScore = sc;
                    bestNext = j;
                }
            }

            if (bestNext < 0) {
                for (int j = 0; j < n; ++j) {
                    if (!used[static_cast<size_t>(j)]) {
                        bestNext = j;
                        break;
                    }
                }
            }
            if (bestNext < 0) {
                break;
            }

            path.push_back(bestNext);
            used[static_cast<size_t>(bestNext)] = true;
        }

        if (static_cast<int>(path.size()) == n) {
            const int sc = ScoreOrdered(extLower, loaded, path);
            if (sc > bestScore) {
                bestScore = sc;
                bestOrder = path;
            }
            if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) {
                break;
            }
        }
    }

    // Exact permutation for tiny n (graph DP via brute force)
    if (bestScore < ScoreToInt(FormatPayloadScore::Complete) && n <= 8 && !timedOut()) {
        std::vector<int> perm(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            perm[static_cast<size_t>(i)] = i;
        }
        do {
            if (timedOut()) {
                break;
            }
            bool headerOk = false;
            for (int i = 0; i < n; ++i) {
                if (HasFormatHeader(extLower,
                                    loaded[static_cast<size_t>(perm[static_cast<size_t>(i)])].bytes.data(),
                                    loaded[static_cast<size_t>(perm[static_cast<size_t>(i)])].bytes.size())) {
                    if (i == 0) {
                        headerOk = true;
                    }
                    break;
                }
            }
            if (!headerOk && n > 1) {
                continue;
            }
            bool linksOk = true;
            for (int i = 0; i + 1 < n; ++i) {
                if (!CanLink(extLower, loaded[static_cast<size_t>(perm[static_cast<size_t>(i)])],
                             loaded[static_cast<size_t>(perm[static_cast<size_t>(i + 1)])])) {
                    linksOk = false;
                    break;
                }
            }
            if (!linksOk) {
                continue;
            }
            const int sc = ScoreOrdered(extLower, loaded, perm);
            if (sc > bestScore) {
                bestScore = sc;
                bestOrder = perm;
            }
        } while (std::next_permutation(perm.begin(), perm.end()));
    }

    if (bestOrder.empty()) {
        return false;
    }
    outOrder = std::move(bestOrder);
    outScore = bestScore;
    return outScore >= ScoreToInt(FormatPayloadScore::Weak);
}

bool GraphCarveFragmentStitcher::StitchBlocks(const std::wstring& extensionLower,
                                              const std::vector<CarveFragmentBlock>& blocks,
                                              std::vector<CarveFragmentBlock>& outOrdered,
                                              int& outBestScore,
                                              const FragmentStitchPolicy& policy) const
{
    outOrdered.clear();
    outBestScore = -1;

    if (blocks.size() < 2 || blocks.size() > policy.graphMaxBlocks) {
        return false;
    }

    const std::wstring extLower = LowerExt(extensionLower);
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) {
        return false;
    }

    std::vector<LoadedBlock> loaded;
    const uint32_t perBlockCap = std::max<uint32_t>(
        4096u, policy.graphMaxAssembleBytes / static_cast<uint32_t>(blocks.size()));
    if (!LoadBlocks(blocks, perBlockCap, loaded)) {
        return false;
    }

    std::vector<int> order;
    if (!SearchBestOrder(extLower, loaded, order, outBestScore, policy)) {
        return false;
    }

    outOrdered.reserve(order.size());
    for (int idx : order) {
        outOrdered.push_back(loaded[static_cast<size_t>(idx)].span);
    }
    return true;
}

bool GraphCarveFragmentStitcher::TryImproveNode(const Partition& /*partition*/,
                                              FileNode& node,
                                              const FragmentStitchPolicy& policy) const
{
    if (!policy.enabled || policy.strategy != FragmentStitchStrategy::GraphCarve) {
        return false;
    }
    if (node.rawFragments.size() < 2) {
        return false;
    }

    const std::wstring extLower = LowerExt(node.extension);
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) {
        return false;
    }

    std::vector<CarveFragmentBlock> ordered;
    int score = -1;
    if (!StitchBlocks(extLower, node.rawFragments, ordered, score, policy)) {
        return false;
    }

  const int baseScore = [&]() {
        std::vector<int> identity;
        identity.reserve(node.rawFragments.size());
        for (size_t i = 0; i < node.rawFragments.size(); ++i) {
            identity.push_back(static_cast<int>(i));
        }
        std::vector<LoadedBlock> loaded;
        const uint32_t perBlockCap = std::max<uint32_t>(
            4096u, policy.graphMaxAssembleBytes /
                       static_cast<uint32_t>(node.rawFragments.size()));
        if (!LoadBlocks(node.rawFragments, perBlockCap, loaded)) {
            return -1;
        }
        return ScoreOrdered(extLower, loaded, identity);
    }();

    if (score <= baseScore) {
        return false;
    }

    uint64_t total = 0;
    for (const auto& s : ordered) {
        total += s.size;
    }

    node.rawFragments = std::move(ordered);
    node.size = total;
    node.rawSize = total;
    node.rawOffset = node.rawFragments.front().diskOffset;
    if (score >= ScoreToInt(FormatPayloadScore::Complete)) {
        node.recoverability = RecoverabilityLevel::Good;
    } else if (score >= ScoreToInt(FormatPayloadScore::Partial)) {
        node.recoverability = RecoverabilityLevel::Poor;
    }
    return true;
}
