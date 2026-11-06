// Real-filesystem smoke test for File — same role as IocpSmokeTest.cpp/
// VectorSmokeTest.cpp: this touches actual Win32 APIs and a real
// directory on disk, so it can ONLY be built and run on an actual
// Windows machine (e.g. via a standalone Visual Studio project, same as
// IocpSmokeTest.cpp), never in the Linux sandbox this was written in.
// Not part of the production moz.build build.
//
// Build (run from this file's own directory, forge-core\, so the
// unprefixed paths below resolve correctly):
//   cl /std:c++17 /EHs- /W4 FileSmokeTest.cpp File.cpp
//   memory\DefaultAllocator.cpp memory\detail\AllocationBackend.cpp

#include "File.h"
#include "Path.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace forge::core;

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

Path MakePath(const char* text)
{
    Result<Path> result = Path::Create(text);
    if (result.HasError())
    {
        std::fprintf(stderr, "Path::Create(\"%s\") failed unexpectedly\n", text);
        std::exit(1);
    }
    return std::move(result.Value());
}

void RunSmokeTest()
{
    Path testDir = MakePath("forge_file_smoke_test_tmp");
    Path nestedDir = MakePath("forge_file_smoke_test_tmp/nested/deeper");
    Path testFile = MakePath("forge_file_smoke_test_tmp/smoke.txt");

    // Clean up anything left over from a previous run, best-effort.
    File::Remove(testFile).Ignore();

    // CreateDirectories: recursive create, and "already exists" is not
    // an error on a second call.
    Result<void> createdNested = File::CreateDirectories(nestedDir);
    CHECK(!createdNested.HasError());
    Result<void> createdAgain = File::CreateDirectories(nestedDir);
    CHECK(!createdAgain.HasError());

    Result<bool> dirExists = File::Exists(testDir);
    CHECK(dirExists.HasValue() && dirExists.Value() == true);

    Result<bool> missingExists = File::Exists(MakePath("forge_file_smoke_test_tmp/does_not_exist"));
    CHECK(missingExists.HasValue() && missingExists.Value() == false);

    // Write, then read back.
    {
        Result<File> opened = File::Open(testFile, FileMode::Write);
        CHECK(opened.HasValue());

        File file = std::move(opened.Value());
        CHECK(file.IsOpen());

        const char* text = "hello, forge filesystem";
        Span<const u8> buffer(reinterpret_cast<const u8*>(text), std::strlen(text));

        Result<Size> written = file.Write(buffer);
        CHECK(written.HasValue() && written.Value() == buffer.Size());

        Result<u64> size = file.SizeInBytes();
        CHECK(size.HasValue() && size.Value() == buffer.Size());
    } // File destructor closes the handle here.

    {
        Result<String> text = File::ReadAllText(testFile);
        CHECK(text.HasValue());
        CHECK(text.Value().View() == StringView("hello, forge filesystem"));
    }

    // Append: opens at end-of-file, so a write lands after existing content.
    {
        Result<File> opened = File::Open(testFile, FileMode::Append);
        CHECK(opened.HasValue());

        File file = std::move(opened.Value());
        const char* more = " -- appended";
        Span<const u8> buffer(reinterpret_cast<const u8*>(more), std::strlen(more));

        Result<Size> written = file.Write(buffer);
        CHECK(written.HasValue() && written.Value() == buffer.Size());
    }

    {
        Result<String> text = File::ReadAllText(testFile);
        CHECK(text.HasValue());
        CHECK(text.Value().View() == StringView("hello, forge filesystem -- appended"));
    }

    // Seek/Tell.
    {
        Result<File> opened = File::Open(testFile, FileMode::Read);
        CHECK(opened.HasValue());
        File file = std::move(opened.Value());

        Result<u64> afterOpen = file.Tell();
        CHECK(afterOpen.HasValue() && afterOpen.Value() == 0);

        Result<u64> seeked = file.Seek(7, SeekOrigin::Begin);
        CHECK(seeked.HasValue() && seeked.Value() == 7);

        u8 buffer[5] = {};
        Result<Size> read = file.Read(Span<u8>(buffer, sizeof(buffer)));
        CHECK(read.HasValue() && read.Value() == sizeof(buffer));
        CHECK(std::memcmp(buffer, "forge", sizeof(buffer)) == 0);

        Result<u64> tellAfterRead = file.Tell();
        CHECK(tellAfterRead.HasValue() && tellAfterRead.Value() == 12);
    }

    // CreateNew must fail with AlreadyExists against the file we already made.
    {
        Result<File> openedAsNew = File::Open(testFile, FileMode::CreateNew);
        CHECK(openedAsNew.HasError());
        CHECK(openedAsNew.Error() == ErrorCode::AlreadyExists);
    }

    // Reading a file that doesn't exist reports NotFound.
    {
        Result<File> openedMissing = File::Open(
            MakePath("forge_file_smoke_test_tmp/does_not_exist.txt"), FileMode::Read);
        CHECK(openedMissing.HasError());
        CHECK(openedMissing.Error() == ErrorCode::NotFound);
    }

    // Clean up.
    CHECK(!File::Remove(testFile).HasError());
    CHECK(!File::Remove(MakePath("forge_file_smoke_test_tmp/nested/deeper")).HasError());
    CHECK(!File::Remove(MakePath("forge_file_smoke_test_tmp/nested")).HasError());
    CHECK(!File::Remove(testDir).HasError());

    Result<bool> goneNow = File::Exists(testDir);
    CHECK(goneNow.HasValue() && goneNow.Value() == false);
}

} // namespace

int main()
{
    RunSmokeTest();

    if (g_failures == 0)
    {
        std::printf("FileSmokeTest: all checks passed\n");
        return 0;
    }

    std::printf("FileSmokeTest: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
