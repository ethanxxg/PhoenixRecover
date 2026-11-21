// Unit tests for media/GridPreviewExtensions (document + preview byte caps).
#include "media/GridPreviewExtensions.h"

#include <cstdio>

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

int main()
{
    CHECK(GridPreview::isDocumentExt(QStringLiteral("pdf")));
    CHECK(GridPreview::isDocumentExt(QStringLiteral("docx")));
    CHECK(GridPreview::isDocumentExt(QStringLiteral("pptx")));
    CHECK(GridPreview::isDocumentExt(QStringLiteral("xlsx")));
    CHECK(!GridPreview::isDocumentExt(QStringLiteral("mp4")));
    CHECK(GridPreview::isThumbnailExt(QStringLiteral("pdf")));
    CHECK(GridPreview::isThumbnailExt(QStringLiteral("jpg")));

    const quint32 capSmall = GridPreview::effectivePreviewReadCap(QStringLiteral("pdf"), 500);
    CHECK(capSmall == 500u);

    const quint32 capLarge = GridPreview::effectivePreviewReadCap(QStringLiteral("pdf"), 200LL * 1024 * 1024);
    CHECK(capLarge == 64u * 1024u * 1024u);

    return 0;
}
