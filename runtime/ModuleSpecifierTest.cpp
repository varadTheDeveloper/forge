// Standalone test for runtime/ModuleSpecifier (Phase 9.1). Portable: runs
// in the Linux sanitizer job and the Windows MSVC job.
//
// Build (g++/clang, from the repository root):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined -iquote .
//       runtime/ModuleSpecifierTest.cpp runtime/ModuleSpecifier.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp

#include "runtime/ModuleSpecifier.h"

#include <cstdio>

using namespace forge::core;
using namespace forge::runtime;

namespace
{

int g_failures = 0;
int g_checks = 0;

const char* kBase = "file:///C:/app/src/main.js";

void ExpectUrl(const char* specifier, const char* expected, const char* base = kBase)
{
    ++g_checks;
    String out;
    const SpecifierError error = ResolveSpecifier(StringView(specifier), StringView(base), out);

    if (error != SpecifierError::None)
    {
        std::fprintf(stderr, "  '%s': unexpected error: %s\n", specifier,
                     DescribeSpecifierError(error));
        ++g_failures;
        return;
    }

    if (!(out.View() == StringView(expected)))
    {
        std::fprintf(stderr, "  '%s': got '%s', expected '%s'\n", specifier, out.CStr(), expected);
        ++g_failures;
    }
}

void ExpectError(const char* specifier, SpecifierError expected, const char* base = kBase)
{
    ++g_checks;
    String out;
    const SpecifierError error = ResolveSpecifier(StringView(specifier), StringView(base), out);

    if (error != expected)
    {
        std::fprintf(stderr, "  '%s': got '%s', expected '%s'\n", specifier,
                     DescribeSpecifierError(error), DescribeSpecifierError(expected));
        ++g_failures;
    }
}

void ExpectPath(const char* url, const char* expected)
{
    ++g_checks;
    String out;
    const SpecifierError error = FileUrlToPath(StringView(url), out);

    if (error != SpecifierError::None || !(out.View() == StringView(expected)))
    {
        std::fprintf(stderr, "  FileUrlToPath('%s'): got '%s' (%s), expected '%s'\n", url,
                     out.CStr(), DescribeSpecifierError(error), expected);
        ++g_failures;
    }
}

void ExpectFileUrl(const char* path, const char* expected)
{
    ++g_checks;
    String out;

    if (!PathToFileUrl(StringView(path), out) || !(out.View() == StringView(expected)))
    {
        std::fprintf(stderr, "  PathToFileUrl('%s'): got '%s', expected '%s'\n", path, out.CStr(),
                     expected);
        ++g_failures;
    }
}

void ExpectNoFileUrl(const char* path)
{
    ++g_checks;
    String out;

    if (PathToFileUrl(StringView(path), out))
    {
        std::fprintf(stderr, "  PathToFileUrl('%s'): got '%s', expected failure\n", path,
                     out.CStr());
        ++g_failures;
    }
}

} // namespace

int main()
{
    // Relative specifiers.
    ExpectUrl("./lib.js", "file:///C:/app/src/lib.js");
    ExpectUrl("./sub/deep.js", "file:///C:/app/src/sub/deep.js");
    ExpectUrl("../shared/x.js", "file:///C:/app/shared/x.js");
    ExpectUrl("./a/../b.js", "file:///C:/app/src/b.js");
    ExpectUrl("./a/./b.js", "file:///C:/app/src/a/b.js");
    ExpectUrl("./a//b.js", "file:///C:/app/src/a/b.js");
    ExpectUrl(".\\win\\style.js", "file:///C:/app/src/win/style.js");
    ExpectUrl("..\\up.js", "file:///C:/app/up.js");
    ExpectUrl("../../../../../../x.js", "file:///C:/x.js");  // cannot climb above the drive
    ExpectUrl("/rooted.js", "file:///C:/rooted.js");
    ExpectUrl("./%2e%2e/dots.js", "file:///C:/app/dots.js");  // escaped dots are dots (URL rule)
    ExpectUrl("./a%20b.js", "file:///C:/app/src/a%20b.js");
    ExpectUrl("./a b.js", "file:///C:/app/src/a%20b.js");
    ExpectUrl("./caf\xC3\xA9.js", "file:///C:/app/src/caf%C3%A9.js");
    ExpectUrl("./100%25.js", "file:///C:/app/src/100%25.js");
    ExpectUrl("./x.json", "file:///C:/app/src/x.json");

    // file: URLs.
    ExpectUrl("file:///C:/app/src/x.js", "file:///C:/app/src/x.js");
    ExpectUrl("file:///c:/app/x.js", "file:///C:/app/x.js");
    ExpectUrl("FILE:///D:/other/y.js", "file:///D:/other/y.js");
    ExpectUrl("file://localhost/C:/a.js", "file:///C:/a.js");
    ExpectUrl("file:///C|/legacy.js", "file:///C:/legacy.js");
    ExpectUrl("file://C:/hostish.js", "file:///C:/hostish.js");
    ExpectUrl("file:///C:/a/../../b.js", "file:///C:/b.js");
    ExpectError("file://server/share/x.js", SpecifierError::NetworkFileUrl);
    ExpectError("file:///no-drive/x.js", SpecifierError::BadFileUrl);
    ExpectError("file:///C:x.js", SpecifierError::BadFileUrl);
    ExpectError("file:///C:/a.js?v=1", SpecifierError::QueryOrFragment);

    // Refused kinds.
    ExpectError("", SpecifierError::Empty);
    ExpectError("lodash", SpecifierError::Bare);
    ExpectError("@scope/pkg/sub.js", SpecifierError::Bare);
    ExpectError(".", SpecifierError::Bare);
    ExpectError("..", SpecifierError::Bare);
    ExpectError(".hidden.js", SpecifierError::Bare);
    ExpectError("node:fs", SpecifierError::NodeBuiltin);
    ExpectError("NODE:child_process", SpecifierError::NodeBuiltin);
    ExpectError("https://example.com/x.js", SpecifierError::UnsupportedScheme);
    ExpectError("data:text/javascript,1", SpecifierError::UnsupportedScheme);
    ExpectError("npm:left-pad", SpecifierError::UnsupportedScheme);
    ExpectError("jsr:@std/path", SpecifierError::UnsupportedScheme);
    ExpectError("blob:abc", SpecifierError::UnsupportedScheme);
    ExpectError("C:/x.js", SpecifierError::WindowsPath);
    ExpectError("C:\\x.js", SpecifierError::WindowsPath);
    ExpectError("c:x.js", SpecifierError::WindowsPath);
    ExpectError("./a.js?v=1", SpecifierError::QueryOrFragment);
    ExpectError("./a.js#frag", SpecifierError::QueryOrFragment);
    ExpectError("./bad%zz.js", SpecifierError::BadEscape);
    ExpectError("./bad%2.js", SpecifierError::BadEscape);
    ExpectError("./trail%", SpecifierError::BadEscape);
    ExpectError("./a%2Fb.js", SpecifierError::EncodedSeparator);
    ExpectError("./a%5cb.js", SpecifierError::EncodedSeparator);
    ExpectError("./nul%00.js", SpecifierError::InvalidCharacter);
    ExpectError("./tab\t.js", SpecifierError::InvalidCharacter);
    ExpectError("./x.js", SpecifierError::NoBase, "");
    ExpectError("./x.js", SpecifierError::NoBase, "https://example.com/a.js");

    // URL to path.
    ExpectPath("file:///C:/app/src/lib.js", "C:\\app\\src\\lib.js");
    ExpectPath("file:///C:/a%20b/caf%C3%A9.js", "C:\\a b\\caf\xC3\xA9.js");
    ExpectPath("file:///C:/", "C:\\");

    // Path to URL, and back.
    ExpectFileUrl("C:\\app\\src\\main.js", "file:///C:/app/src/main.js");
    ExpectFileUrl("c:\\a b\\100%.js", "file:///C:/a%20b/100%25.js");
    ExpectFileUrl("C:\\x#y\\z?.js", "file:///C:/x%23y/z%3F.js");
    ExpectFileUrl("C:\\", "file:///C:/");
    ExpectNoFileUrl("\\\\server\\share\\x.js");
    ExpectNoFileUrl("relative\\x.js");
    ExpectNoFileUrl("");

    {
        ++g_checks;
        String url;
        String back;

        if (!PathToFileUrl(StringView("C:\\dir with space\\%41\\caf\xC3\xA9.js"), url) ||
            FileUrlToPath(url.View(), back) != SpecifierError::None ||
            !(back.View() == StringView("C:\\dir with space\\%41\\caf\xC3\xA9.js")))
        {
            std::fprintf(stderr, "  round trip failed: '%s' -> '%s'\n", url.CStr(), back.CStr());
            ++g_failures;
        }
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "ModuleSpecifierTest: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }

    std::printf("ModuleSpecifierTest: all %d checks passed\n", g_checks);
    return 0;
}
