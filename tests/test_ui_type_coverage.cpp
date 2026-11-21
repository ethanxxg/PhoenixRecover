// Ensures every extension in the UI type tree (5 categories) has a support rule:
// direct signature, alias, ZIP/CFB container, or metadata-only (FS scan).

#include "scan/ExtensionCoverage.h"
#include "scan/SignatureDB.h"
#include "common/UiFileTypeCatalog.h"

#include <cstdio>
#include <string>

int main()
{
    const auto dupes = UiFileTypeCatalog::FindDuplicateExtensions();
    if (!dupes.empty()) {
        fprintf(stderr, "FAIL: duplicate extensions across categories:\n");
        for (const auto& d : dupes) {
            fprintf(stderr, "  %ls:", d.extension.c_str());
            for (const auto& c : d.categories)
                fprintf(stderr, " %ls", c.c_str());
            fprintf(stderr, "\n");
        }
        return 1;
    }

    SignatureDB db;
    const auto unsupported = FindUnsupportedUiExtensions(db);

    const auto all = UiFileTypeCatalog::AllExtensions();
    fprintf(stdout, "UiFileTypeCatalog categories: %zu  unique extensions: %zu\n",
            UiFileTypeCatalog::Categories().size(), all.size());
    fprintf(stdout, "SignatureDB entries: %zu\n", db.GetAll().size());

    if (!unsupported.empty()) {
        fprintf(stderr, "FAIL: %zu UI extensions lack coverage:\n",
                unsupported.size());
        for (const auto& e : unsupported)
            fprintf(stderr, "  %ls\n", e.c_str());
        return 1;
    }

    fprintf(stdout, "PASS: all UI type-tree extensions are supported.\n");
    return 0;
}
