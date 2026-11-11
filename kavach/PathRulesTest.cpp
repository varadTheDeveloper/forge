// Standalone test for kavach/PathRules (Phase 8.2): the canonical path
// function and the grant matcher. Portable: runs in the Linux sanitizer job
// and the Windows MSVC job.
//
// Build (g++/clang, from the repository root):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined -iquote .
//       kavach/PathRulesTest.cpp kavach/PathRules.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp

#include "kavach/PathRules.h"

#include <cstdio>
#include <cstring>

using namespace forge::core;
using namespace forge::kavach;

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                        \
        {                                                                    \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__,    \
                          __LINE__, #cond);                                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

const char* kCwd = "C:\\work\\project";

void ExpectCanonical(const char* input, const char* expected, const char* cwd = kCwd)
{
    CanonicalPath out;
    const PathError error = LexicalCanonicalize(StringView(input), StringView(cwd), out);

    if (error != PathError::None)
    {
        std::fprintf(stderr, "  '%s': unexpected error: %s\n", input, DescribePathError(error));
        ++g_failures;
        return;
    }

    if (!(out.text.View() == StringView(expected)))
    {
        std::fprintf(stderr, "  '%s': got '%s', expected '%s'\n", input, out.text.CStr(), expected);
        ++g_failures;
    }
}

void ExpectError(const char* input, PathError expected, const char* cwd = kCwd)
{
    CanonicalPath out;
    const PathError error = LexicalCanonicalize(StringView(input), StringView(cwd), out);

    if (error != expected)
    {
        std::fprintf(stderr, "  '%s': got '%s', expected '%s'\n", input, DescribePathError(error),
                     DescribePathError(expected));
        ++g_failures;
    }

    CHECK(error == PathError::None || out.text.Empty());
}

void Test_Canonical()
{
    std::printf("Test_Canonical...\n");

    ExpectCanonical("C:\\a\\b", "C:\\a\\b");
    ExpectCanonical("c:/a/b", "C:\\a\\b");
    ExpectCanonical("C:\\", "C:\\");
    ExpectCanonical("C:/", "C:\\");
    ExpectCanonical("C:\\a\\\\b\\", "C:\\a\\b");
    ExpectCanonical("C:\\a\\.\\b\\..\\c", "C:\\a\\c");
    ExpectCanonical("file.txt", "C:\\work\\project\\file.txt");
    ExpectCanonical("./sub/../file.txt", "C:\\work\\project\\file.txt");
    ExpectCanonical("..\\other", "C:\\work\\other");
    ExpectCanonical("\\top", "C:\\top");
    ExpectCanonical(".", "C:\\work\\project");
    ExpectCanonical("\\\\server\\share", "\\\\server\\share");
    ExpectCanonical("//server/share/dir/../f", "\\\\server\\share\\f");
    ExpectCanonical("f", "\\\\srv\\sh\\dir\\f", "\\\\srv\\sh\\dir");
    ExpectCanonical("\\x", "\\\\srv\\sh\\x", "\\\\srv\\sh\\dir");
    ExpectCanonical("caf\xc3\xa9\\na\xc3\xafve.txt", "C:\\work\\project\\caf\xc3\xa9\\na\xc3\xafve.txt");
    ExpectCanonical("C:\\a\\.hidden", "C:\\a\\.hidden");
    ExpectCanonical("C:\\a\\..b", "C:\\a\\..b");
    ExpectCanonical("C:\\console.log", "C:\\console.log");  // not CON
    ExpectCanonical("C:\\COM10", "C:\\COM10");              // only COM0-COM9
    ExpectCanonical("C:\\nulled", "C:\\nulled");
}

void Test_Rejected()
{
    std::printf("Test_Rejected...\n");

    ExpectError("", PathError::Empty);
    {
        CanonicalPath out;
        CHECK(LexicalCanonicalize(StringView("a\0b", 3), StringView(kCwd), out) ==
              PathError::ContainsNul);
    }
    ExpectError("\\\\?\\C:\\a", PathError::DevicePath);
    ExpectError("\\\\.\\PhysicalDrive0", PathError::DevicePath);
    ExpectError("//?/C:/a", PathError::DevicePath);
    ExpectError("\\??\\C:\\a", PathError::DevicePath);
    ExpectError("C:a", PathError::DriveRelative);
    ExpectError("C:", PathError::DriveRelative);
    ExpectError("\\\\server", PathError::InvalidUnc);
    ExpectError("\\\\server\\", PathError::InvalidUnc);
    ExpectError("\\\\\\share", PathError::InvalidUnc);
    ExpectError("\\\\.\\x\\", PathError::DevicePath);
    ExpectError("\\\\..\\x", PathError::InvalidUnc);
    ExpectError("C:\\..", PathError::EscapesRoot);
    ExpectError("C:\\a\\..\\..\\b", PathError::EscapesRoot);
    ExpectError("..\\..\\..", PathError::EscapesRoot);
    ExpectError("\\\\s\\sh\\..", PathError::EscapesRoot);
    ExpectError("C:\\a.", PathError::TrailingDotOrSpace);
    ExpectError("C:\\a \\b", PathError::TrailingDotOrSpace);
    ExpectError("C:\\a...", PathError::TrailingDotOrSpace);
    ExpectError("C:\\a:stream", PathError::AlternateStream);
    ExpectError("file.txt::$DATA", PathError::AlternateStream);
    ExpectError("1:\\x", PathError::AlternateStream);
    ExpectError("C:\\a*", PathError::InvalidCharacter);
    ExpectError("C:\\a?b", PathError::InvalidCharacter);
    ExpectError("C:\\a<b", PathError::InvalidCharacter);
    ExpectError("C:\\a|b", PathError::InvalidCharacter);
    ExpectError("C:\\a\"b", PathError::InvalidCharacter);
    ExpectError("C:\\a\tb", PathError::InvalidCharacter);
    ExpectError("NUL", PathError::ReservedName);
    ExpectError("C:\\dir\\con", PathError::ReservedName);
    ExpectError("C:\\dir\\Con.txt", PathError::ReservedName);
    ExpectError("C:\\dir\\COM1", PathError::ReservedName);
    ExpectError("C:\\dir\\lpt9.log", PathError::ReservedName);
    ExpectError("C:\\dir\\COM\xc2\xb9", PathError::ReservedName);
    ExpectError("C:\\dir\\CONIN$", PathError::ReservedName);
    ExpectError("C:\\dir\\aux .txt", PathError::ReservedName);
    ExpectError("relative", PathError::DriveRelative, "");  // no base folder
}

void Test_ShortNames()
{
    std::printf("Test_ShortNames...\n");

    CanonicalPath out;
    CHECK(LexicalCanonicalize("C:\\PROGRA~1\\x", "", out) == PathError::None);
    CHECK(out.lastShortNameComponent == 1 && out.componentCount == 2);

    CHECK(LexicalCanonicalize("C:\\a\\b\\DOCUME~12.TXT", "", out) == PathError::None);
    CHECK(out.lastShortNameComponent == 3);

    CHECK(LexicalCanonicalize("C:\\file~name.txt\\no~x", "", out) == PathError::None);
    CHECK(out.lastShortNameComponent == 0);

    CHECK(LexicalCanonicalize("C:\\toolongname~1", "", out) == PathError::None);
    CHECK(out.lastShortNameComponent == 0);
}

void Test_Covers()
{
    std::printf("Test_Covers...\n");

    CHECK(PathCovers("C:\\work", "C:\\work"));
    CHECK(PathCovers("C:\\work", "C:\\work\\a"));
    CHECK(PathCovers("C:\\work", "c:\\WORK\\a\\b"));
    CHECK(!PathCovers("C:\\work", "C:\\workshop"));
    CHECK(!PathCovers("C:\\work", "C:\\wor"));
    CHECK(!PathCovers("C:\\work\\a", "C:\\work"));
    CHECK(PathCovers("C:\\", "C:\\anything"));
    CHECK(!PathCovers("C:\\", "D:\\x"));
    CHECK(PathCovers("\\\\s\\sh", "\\\\s\\sh\\x"));
    CHECK(!PathCovers("\\\\s\\sh", "\\\\s\\shared"));
    CHECK(!PathCovers("", "C:\\x"));
    // Non-ASCII letters must match exactly (fail closed).
    CHECK(!PathCovers("C:\\\xc3\xa9", "C:\\\xc3\x89"));
}

void Test_Helpers()
{
    std::printf("Test_Helpers...\n");

    CHECK(IsRootPath("C:\\"));
    CHECK(IsRootPath("\\\\s\\sh"));
    CHECK(!IsRootPath("C:\\a"));
    CHECK(!IsRootPath("\\\\s\\sh\\a"));

    CHECK(HasComponent("C:\\p\\.git\\config", ".git"));
    CHECK(HasComponent("C:\\p\\.GIT", ".git"));
    CHECK(!HasComponent("C:\\p\\.gitignore", ".git"));
    CHECK(!HasComponent("C:\\p\\x.git", ".git"));

    CHECK(LastComponent("C:\\a\\b.txt") == StringView("b.txt"));
    CHECK(LastComponent("C:\\").Empty());
    CHECK(LastComponent("\\\\s\\sh\\f") == StringView("f"));

    CHECK(ParentPath("C:\\a\\b") == StringView("C:\\a"));
    CHECK(ParentPath("C:\\a") == StringView("C:\\"));
    CHECK(ParentPath("C:\\") == StringView("C:\\"));
    CHECK(ParentPath("\\\\s\\sh\\a") == StringView("\\\\s\\sh"));
    CHECK(ParentPath("\\\\s\\sh\\a\\b") == StringView("\\\\s\\sh\\a"));
}

void Test_Idempotent()
{
    std::printf("Test_Idempotent...\n");

    const char* inputs[] = { "C:\\a\\b", "x/y/../z", "\\\\s\\sh\\q", "\\root", "." };

    for (const char* input : inputs)
    {
        CanonicalPath first;
        CanonicalPath second;
        CHECK(LexicalCanonicalize(input, kCwd, first) == PathError::None);
        CHECK(LexicalCanonicalize(first.text.View(), "", second) == PathError::None);
        CHECK(first.text.View() == second.text.View());
    }
}

} // namespace

int main()
{
    Test_Canonical();
    Test_Rejected();
    Test_ShortNames();
    Test_Covers();
    Test_Helpers();
    Test_Idempotent();

    if (g_failures != 0)
    {
        std::printf("PathRulesTest: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("PathRulesTest: all passed\n");
    return 0;
}
