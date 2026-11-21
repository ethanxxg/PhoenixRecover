#include <cassert>
#include <string>

#include "../recover/FileNode.h"
#include "../recover/RecoverEngine.h"

static void ExpectEq(const std::wstring& a, const std::wstring& b)
{
    assert(a == b);
}

int main()
{
    FileNode n{};
    n.name = L"frag.jpg";

    // Flat mode: always targetDir + name
    {
        n.originalPath.clear();
        auto p = RecoverEngine::BuildTargetPathForNode(n, L"D:\\out", false);
        ExpectEq(p, L"D:\\out\\frag.jpg");
    }

    // Keep structure with drive prefix.
    {
        n.originalPath = L"C:\\dir1\\dir2\\frag.jpg";
        auto p = RecoverEngine::BuildTargetPathForNode(n, L"D:\\out", true);
        ExpectEq(p, L"D:\\out\\dir1\\dir2\\frag.jpg");
    }

    // Keep structure with leading backslash.
    {
        n.originalPath = L"\\dirA\\dirB\\x.pdf";
        auto p = RecoverEngine::BuildTargetPathForNode(n, L"D:\\out\\", true);
        ExpectEq(p, L"D:\\out\\dirA\\dirB\\x.pdf");
    }

    // Illegal ':' replaced.
    {
        n.originalPath = L"C:\\a:b\\c.txt";
        auto p = RecoverEngine::BuildTargetPathForNode(n, L"D:\\out", true);
        ExpectEq(p, L"D:\\out\\a_b\\c.txt");
    }

    return 0;
}

