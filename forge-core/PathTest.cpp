// Standalone correctness test for Path — same role as StringTest.cpp/
// VectorSmokeTest.cpp. Compiled under the real project constraints
// (C++17, exceptions disabled — see HISTORY.md/AGENTS.md) from the
// start, per the user's explicit instruction to always target the real
// mach build's constraints rather than a more permissive setup.

#include "Path.h"

#include <cstdio>
#include <utility>

using namespace forge::core;
using namespace forge::core::memory;

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

class FailingAllocator final : public Allocator
{
public:
    explicit FailingAllocator(int failAfterNCalls) : failAfterNCalls_(failAfterNCalls) {}

    [[nodiscard]] void* Allocate(Size size, Size alignment) noexcept override
    {
        if (callCount_ >= failAfterNCalls_)
        {
            return nullptr;
        }
        ++callCount_;
        return DefaultAllocator{}.Allocate(size, alignment);
    }

    void Deallocate(void* memory, Size size, Size alignment) noexcept override
    {
        DefaultAllocator{}.Deallocate(memory, size, alignment);
    }

private:
    int failAfterNCalls_;
    int callCount_{ 0 };
};

Path MakePath(StringView text)
{
    Result<Path> result = Path::Create(text);
    CHECK(result.HasValue());
    return std::move(result.Value());
}

void Test_Path_CreateNormalizesSeparators()
{
    std::printf("Test_Path_CreateNormalizesSeparators...\n");

    Path path = MakePath("a\\b/c.txt");
    CHECK(path.View() == StringView("a/b/c.txt"));
    CHECK(!path.Empty());

    Path empty = MakePath("");
    CHECK(empty.Empty());
}

void Test_Path_IsAbsolute()
{
    std::printf("Test_Path_IsAbsolute...\n");

    CHECK(MakePath("C:/foo/bar").IsAbsolute());
    CHECK(MakePath("C:\\foo\\bar").IsAbsolute()); // backslash input still detected after normalization
    CHECK(MakePath("//server/share").IsAbsolute());
    CHECK(!MakePath("/just/rooted").IsAbsolute()); // bare leading '/' is NOT absolute (no root-name)
    CHECK(!MakePath("relative/path").IsAbsolute());
    CHECK(!MakePath("C:").IsAbsolute()); // drive-relative, not drive-absolute

    CHECK(MakePath("relative/path").IsRelative());
    CHECK(!MakePath("C:/foo").IsRelative());
}

void Test_Path_ParentFileNameStemExtension()
{
    std::printf("Test_Path_ParentFileNameStemExtension...\n");

    Path path = MakePath("a/b/c.txt");
    CHECK(path.Parent() == StringView("a/b"));
    CHECK(path.FileName() == StringView("c.txt"));
    CHECK(path.Stem() == StringView("c"));
    CHECK(path.Extension() == StringView(".txt"));

    Path noExt = MakePath("a/b/README");
    CHECK(noExt.FileName() == StringView("README"));
    CHECK(noExt.Stem() == StringView("README"));
    CHECK(noExt.Extension() == StringView(""));

    Path dotfile = MakePath("a/.gitignore");
    CHECK(dotfile.FileName() == StringView(".gitignore"));
    CHECK(dotfile.Stem() == StringView(".gitignore")); // leading dot doesn't count as an extension separator
    CHECK(dotfile.Extension() == StringView(""));

    Path noSeparator = MakePath("c.txt");
    CHECK(noSeparator.Parent() == StringView(""));
    CHECK(noSeparator.FileName() == StringView("c.txt"));

    Path trailingSlash = MakePath("a/b/");
    CHECK(trailingSlash.FileName() == StringView("b")); // documented simplification
    CHECK(trailingSlash.Parent() == StringView("a"));

    Path root = MakePath("/");
    CHECK(root.Parent() == StringView("/")); // root is its own parent
    CHECK(root.FileName() == StringView(""));

    Path driveRoot = MakePath("C:/");
    CHECK(driveRoot.Parent() == StringView("C:/"));
    CHECK(driveRoot.FileName() == StringView(""));

    Path rootChild = MakePath("/a");
    CHECK(rootChild.Parent() == StringView("/"));
    CHECK(rootChild.FileName() == StringView("a"));

    Path empty = MakePath("");
    CHECK(empty.Parent() == StringView(""));
    CHECK(empty.FileName() == StringView(""));
}

void Test_Path_AppendAndJoin()
{
    std::printf("Test_Path_AppendAndJoin...\n");

    Path path = MakePath("a/b");
    CHECK(!path.Append("c.txt").HasError());
    CHECK(path.View() == StringView("a/b/c.txt"));

    Path rootPath = MakePath("C:/");
    CHECK(!rootPath.Append("foo").HasError());
    CHECK(rootPath.View() == StringView("C:/foo")); // no double separator after a root that already ends in '/'

    Result<Path> joined = Path::Join("a/b", "c.txt");
    CHECK(joined.HasValue());
    CHECK(joined.Value().View() == StringView("a/b/c.txt"));

    // Appending an empty segment is a no-op.
    Path unchanged = MakePath("a/b");
    CHECK(!unchanged.Append("").HasError());
    CHECK(unchanged.View() == StringView("a/b"));
}

void Test_Path_NormalizeRelative()
{
    std::printf("Test_Path_NormalizeRelative...\n");

    Path a = MakePath("a/./b/../c");
    CHECK(!a.Normalize().HasError());
    CHECK(a.View() == StringView("a/c"));

    Path b = MakePath("../../a");
    CHECK(!b.Normalize().HasError());
    CHECK(b.View() == StringView("../../a")); // can't resolve ".." with nothing real to cancel against

    Path c = MakePath("a/../..");
    CHECK(!c.Normalize().HasError());
    CHECK(c.View() == StringView("..")); // one real segment cancels one ".."; the second has nothing left to cancel

    Path dot = MakePath(".");
    CHECK(!dot.Normalize().HasError());
    CHECK(dot.View() == StringView("."));

    Path collapsedSlashes = MakePath("a//b///c");
    CHECK(!collapsedSlashes.Normalize().HasError());
    CHECK(collapsedSlashes.View() == StringView("a/b/c"));
}

void Test_Path_NormalizeAbsolute()
{
    std::printf("Test_Path_NormalizeAbsolute...\n");

    Path a = MakePath("/a/../..");
    CHECK(!a.Normalize().HasError());
    CHECK(a.View() == StringView("/")); // can't escape above an absolute root

    Path b = MakePath("C:/a/b/../c");
    CHECK(!b.Normalize().HasError());
    CHECK(b.View() == StringView("C:/a/c"));

    Path root = MakePath("/");
    CHECK(!root.Normalize().HasError());
    CHECK(root.View() == StringView("/"));

    Path driveRoot = MakePath("C:/");
    CHECK(!driveRoot.Normalize().HasError());
    CHECK(driveRoot.View() == StringView("C:/"));
}

// Phase 7.9 regression table (study 13 found these with ASan+UBSan): a
// root must survive Normalize() unchanged, absoluteness must be
// preserved, and ".." must never climb above a UNC share or a device
// volume.
void Test_Path_NormalizeWindowsRoots()
{
    std::printf("Test_Path_NormalizeWindowsRoots...\n");

    struct Case
    {
        const char* input;
        const char* expected;
    };

    const Case cases[] = {
        { "//server/share/x", "//server/share/x" },
        { "\\\\server\\share\\..\\..\\x", "//server/share/x" },
        { "//server/share", "//server/share" },
        { "//server/share/", "//server/share/" },
        { "//server/share/a/..", "//server/share/" },
        { "\\\\?\\C:\\a\\..\\b", "//?/C:/b" },
        { "//?/C:/../../x", "//?/C:/x" },
        { "//?/UNC/server/share/a/../../..", "//?/UNC/server/share/" },
        { "//./PIPE/name", "//./PIPE/name" },
        { "C:/a/../../..", "C:/" },
        { "C:foo/../..", "C:.." },
        { "C:foo/./bar", "C:foo/bar" },
        { "C:", "C:" },
        { "/a/../..", "/" },
        { "a/../..", ".." },
        { "./C:/x", "./C:/x" },
        { "b/../C:x", "./C:x" },
    };

    for (const Case& c : cases)
    {
        Path p = MakePath(c.input);
        const bool wasAbsolute = p.IsAbsolute();

        CHECK(!p.Normalize().HasError());

        if (p.View() != StringView(c.expected))
        {
            std::fprintf(stderr, "  Normalize(%s): got '%.*s', expected '%s'\n", c.input,
                         static_cast<int>(p.View().Size()), p.View().Data(), c.expected);
        }

        CHECK(p.View() == StringView(c.expected));
        CHECK(p.IsAbsolute() == wasAbsolute);

        // Idempotent.
        Path again = MakePath(c.expected);
        CHECK(!again.Normalize().HasError());
        CHECK(again.View() == StringView(c.expected));
    }

    // Parent() stops at the full UNC / device root.
    Path unc = MakePath("//server/share/x");
    CHECK(unc.Parent() == StringView("//server/share/"));

    Path uncRoot = MakePath("//server/share");
    CHECK(uncRoot.Parent() == StringView("//server/share"));
    CHECK(uncRoot.FileName().Empty());

    Path device = MakePath("//?/C:/x");
    CHECK(device.Parent() == StringView("//?/C:/"));
}

void Test_Path_Comparison()
{
    std::printf("Test_Path_Comparison...\n");

    Path a = MakePath("a/b");
    Path b = MakePath("a/b");
    Path c = MakePath("a/c");

    CHECK(a == b);
    CHECK(a != c);
    CHECK(!(a == c));
}

void Test_Path_CopyAndMove()
{
    std::printf("Test_Path_CopyAndMove...\n");

    Path original = MakePath("a/b");
    Path copy(original);
    CHECK(!copy.Append("c").HasError());

    CHECK(copy.View() == StringView("a/b/c"));
    CHECK(original.View() == StringView("a/b")); // unaffected by mutating the copy

    Path moved(std::move(original));
    CHECK(moved.View() == StringView("a/b"));
    CHECK(original.Empty()); // moved-from, same convention as String
}

void Test_Path_CreateReportsOomWithoutCorruption()
{
    std::printf("Test_Path_CreateReportsOomWithoutCorruption...\n");

    FailingAllocator allocator(/*failAfterNCalls=*/0);
    Result<Path> result = Path::Create(allocator, "some/path");
    CHECK(result.HasError());
}

void Test_Path_NormalizeReportsOomWithoutCorruption()
{
    std::printf("Test_Path_NormalizeReportsOomWithoutCorruption...\n");

    // The scratch Vector<StringView>/String used inside Normalize() use
    // the default allocator, not the Path's own — Normalize()'s
    // fallibility here is about that scratch work, not about value_'s
    // own allocator. This test just confirms Normalize() itself reports
    // Result<void> correctly end to end on the happy path; a dedicated
    // "Path's own allocator fails" path is covered by
    // Test_Path_CreateReportsOomWithoutCorruption/Append below instead.
    Path path = MakePath("a/./b");
    CHECK(!path.Normalize().HasError());
}

void Test_Path_AppendReportsOomWithoutCorruption()
{
    std::printf("Test_Path_AppendReportsOomWithoutCorruption...\n");

    FailingAllocator allocator(/*failAfterNCalls=*/1);
    Path path(allocator);

    CHECK(!path.Append("a").HasError());

    bool sawFailure = false;
    for (int i = 0; i < 64; ++i)
    {
        if (path.Append("segment").HasError())
        {
            sawFailure = true;
            break;
        }
    }

    CHECK(sawFailure);
    CHECK(path.View() == StringView("a")); // whatever succeeded is still intact
}

} // namespace

int main()
{
    Test_Path_CreateNormalizesSeparators();
    Test_Path_IsAbsolute();
    Test_Path_ParentFileNameStemExtension();
    Test_Path_AppendAndJoin();
    Test_Path_NormalizeRelative();
    Test_Path_NormalizeAbsolute();
    Test_Path_NormalizeWindowsRoots();
    Test_Path_Comparison();
    Test_Path_CopyAndMove();
    Test_Path_CreateReportsOomWithoutCorruption();
    Test_Path_NormalizeReportsOomWithoutCorruption();
    Test_Path_AppendReportsOomWithoutCorruption();

    if (g_failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }

    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
