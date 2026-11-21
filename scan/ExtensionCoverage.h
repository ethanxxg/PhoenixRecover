#pragma once

#include <string>
#include <vector>

class SignatureDB;

// True when extension is listed in UiFileTypeCatalog and covered by signature/alias/container/metadata rules.
bool IsUiExtensionSupported(const std::wstring& extLower);

// Returns extensions with no coverage rule (empty = all UI subtypes accounted for).
std::vector<std::wstring> FindUnsupportedUiExtensions(const SignatureDB& db);
