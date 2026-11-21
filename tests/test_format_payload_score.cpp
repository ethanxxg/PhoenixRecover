#include "recover/FormatPayloadScore.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

int main()
{
    // Minimal JPEG: SOI + EOI
    const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xD9 };
    const auto jpgScore =
        FormatPayloadScoreUtil::Score(L"jpg", jpg, sizeof(jpg));
    CHECK(jpgScore == FormatPayloadScore::Complete);

  const uint8_t pngSig[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
        0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0 };
    const auto pngScore =
        FormatPayloadScoreUtil::Score(L"png", pngSig, sizeof(pngSig));
    CHECK(pngScore >= FormatPayloadScore::Partial);

    CHECK(FormatPayloadScoreUtil::SupportsFragmentStitch(L"jpg"));
    CHECK(!FormatPayloadScoreUtil::SupportsFragmentStitch(L"dat"));

    if (g_failures != 0) {
        fprintf(stderr, "FormatPayloadScore: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: FormatPayloadScore\n");
    return 0;
}
