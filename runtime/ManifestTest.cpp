// Standalone test for runtime/Manifest (Phase 9.2): forge.json validation,
// package names, bare specifiers and "exports" resolution. Portable.
//
// Build (g++/clang, from the repository root):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined -iquote .
//       runtime/ManifestTest.cpp runtime/Manifest.cpp runtime/Json.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp

#include "runtime/Manifest.h"

#include <cstdio>
#include <cstring>

using namespace forge::core;
using namespace forge::runtime;

namespace
{

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what)
{
    ++g_checks;

    if (!condition)
    {
        std::fprintf(stderr, "  check failed: %s\n", what);
        ++g_failures;
    }
}

bool ParseOk(const char* text, Manifest& m, bool isApp = false)
{
    String message;

    if (!ParseManifest(StringView(text), isApp, m, message))
    {
        std::fprintf(stderr, "  should parse: %s\n    error: %s\n", text, message.CStr());
        return false;
    }

    return true;
}

void ExpectRefused(const char* text, const char* messagePart, bool isApp = false)
{
    ++g_checks;
    Manifest m;
    String message;

    if (ParseManifest(StringView(text), isApp, m, message))
    {
        std::fprintf(stderr, "  should be refused: %s\n", text);
        ++g_failures;
        return;
    }

    if (std::strstr(message.CStr(), messagePart) == nullptr)
    {
        std::fprintf(stderr, "  %s\n    message '%s' lacks '%s'\n", text, message.CStr(), messagePart);
        ++g_failures;
    }
}

void ExpectExport(const Manifest& m, const char* subpath, ExportResult expected,
                  const char* target = nullptr)
{
    ++g_checks;
    String out;
    const ExportResult result = ResolveExport(m, StringView(subpath), out);

    if (result != expected || (target != nullptr && !(out.View() == StringView(target))))
    {
        std::fprintf(stderr, "  export '%s': got %d '%s', expected %d '%s'\n", subpath,
                     static_cast<int>(result), out.CStr(), static_cast<int>(expected),
                     target != nullptr ? target : "");
        ++g_failures;
    }
}

void ExpectSplit(const char* specifier, const char* name, const char* subpath)
{
    ++g_checks;
    String n;
    String s;

    if (!SplitBareSpecifier(StringView(specifier), n, s) || !(n.View() == StringView(name)) ||
        !(s.View() == StringView(subpath)))
    {
        std::fprintf(stderr, "  split '%s': got '%s' '%s'\n", specifier, n.CStr(), s.CStr());
        ++g_failures;
    }
}

void ExpectNoSplit(const char* specifier)
{
    ++g_checks;
    String n;
    String s;

    if (SplitBareSpecifier(StringView(specifier), n, s))
    {
        std::fprintf(stderr, "  split '%s' should fail\n", specifier);
        ++g_failures;
    }
}

} // namespace

int main()
{
    // Names.
    Check(IsValidPackageName(StringView("greet")), "simple name");
    Check(IsValidPackageName(StringView("@acme/util.js-2~x")), "scoped name");
    Check(!IsValidPackageName(StringView("Greet")), "upper case refused");
    Check(!IsValidPackageName(StringView(".hidden")), "leading dot refused");
    Check(!IsValidPackageName(StringView("_x")), "leading underscore refused");
    Check(!IsValidPackageName(StringView("@acme")), "scope without name refused");
    Check(!IsValidPackageName(StringView("@/x")), "empty scope refused");
    Check(!IsValidPackageName(StringView("a/b")), "slash without scope refused");
    Check(!IsValidPackageName(StringView("a b")), "space refused");
    Check(!IsValidPackageName(StringView("")), "empty refused");

    // Bare specifiers.
    ExpectSplit("greet", "greet", ".");
    ExpectSplit("greet/extra", "greet", "./extra");
    ExpectSplit("@acme/util", "@acme/util", ".");
    ExpectSplit("@acme/util/a/b.js", "@acme/util", "./a/b.js");
    ExpectNoSplit("greet/../x");
    ExpectNoSplit("greet/");
    ExpectNoSplit("greet//x");
    ExpectNoSplit("Greet");
    ExpectNoSplit("@acme");
    ExpectNoSplit("greet/a\\b");

    // Valid manifests.
    {
        Manifest m;
        Check(ParseOk("{\"name\":\"greet\",\"version\":\"1.2.3\",\"exports\":\"./index.js\","
                      "\"dependencies\":{\"@acme/util\":\"file:../util\"}}",
                      m),
              "full manifest parses");
        Check(m.name.View() == StringView("greet") && m.version.View() == StringView("1.2.3"),
              "name and version");
        Check(m.dependencies.Size() == 1 && m.dependencies[0].name.View() == StringView("@acme/util") &&
                  m.dependencies[0].path.View() == StringView("../util"),
              "dependency path kept as written");
        ExpectExport(m, ".", ExportResult::Found, "./index.js");
        ExpectExport(m, "./index.js", ExportResult::NotExported);
    }

    {
        Manifest m;
        Check(ParseOk("{}", m, true), "empty app manifest");
        Check(!m.hasExports, "no exports");
        ExpectExport(m, ".", ExportResult::NoExports);
    }

    {
        Manifest m;
        Check(ParseOk("{\"name\":\"p\",\"exports\":{"
                      "\".\":\"./main.js\","
                      "\"./extra\":{\"node\":\"./node.js\",\"forge\":\"./forge.js\",\"default\":\"./d.js\"},"
                      "\"./ordered\":{\"default\":\"./first.js\",\"forge\":\"./second.js\"},"
                      "\"./nested\":{\"browser\":\"./b.js\",\"import\":{\"forge\":\"./nf.js\"}},"
                      "\"./features/*\":\"./src/features/*.js\","
                      "\"./features/private/*\":null,"
                      "\"./features/special\":\"./special.js\","
                      "\"./deep/*/index\":\"./lib/*/index.js\","
                      "\"./blocked\":null,"
                      "\"./only-node\":{\"node\":\"./n.js\"}"
                      "},\"permissions\":{},\"grants\":{}}",
                      m),
              "exports map parses");
        ExpectExport(m, ".", ExportResult::Found, "./main.js");
        ExpectExport(m, "./extra", ExportResult::Found, "./forge.js");
        ExpectExport(m, "./ordered", ExportResult::Found, "./first.js");  // object order, as Node.js
        ExpectExport(m, "./nested", ExportResult::Found, "./nf.js");
        ExpectExport(m, "./features/a", ExportResult::Found, "./src/features/a.js");
        ExpectExport(m, "./features/a/b", ExportResult::Found, "./src/features/a/b.js");
        ExpectExport(m, "./features/private/x", ExportResult::Blocked);  // longer prefix wins
        ExpectExport(m, "./features/special", ExportResult::Found, "./special.js");  // exact first
        ExpectExport(m, "./deep/x/index", ExportResult::Found, "./lib/x/index.js");
        ExpectExport(m, "./blocked", ExportResult::Blocked);
        ExpectExport(m, "./only-node", ExportResult::NotExported);
        ExpectExport(m, "./missing", ExportResult::NotExported);
        ExpectExport(m, "./features/", ExportResult::NotExported);
        ExpectExport(m, "./features/../../escape", ExportResult::Invalid);
        ExpectExport(m, "./features/node_modules/x", ExportResult::Invalid);
    }

    {
        Manifest m;
        Check(ParseOk("{\"name\":\"c\",\"exports\":{\"forge\":\"./f.js\",\"default\":\"./d.js\"}}", m),
              "top-level conditions parse");
        ExpectExport(m, ".", ExportResult::Found, "./f.js");
        ExpectExport(m, "./x", ExportResult::NotExported);
    }

    // Phase 9.4: permissions and grants.
    {
        Manifest m;
        Check(ParseOk("{\"name\":\"p\",\"permissions\":{\"read\":[\"./data\",\"./more\"],"
                      "\"write\":[\"./out\"],\"env\":[\"HOME\"]}}",
                      m),
              "permissions parse");
        Check(m.permissions.read.Size() == 2 && m.permissions.read[1].View() == StringView("./more"),
              "permissions.read kept as written");
        Check(m.permissions.write.Size() == 1 && m.permissions.env.Size() == 1 &&
                  m.permissions.env[0].View() == StringView("HOME"),
              "permissions.write and env kept");
        Check(m.grants.Empty() && !m.permissions.Empty(), "no grants in a package");
    }

    {
        Manifest m;
        Check(ParseOk("{\"grants\":{\"greet\":{\"read\":[\"./data\",\"C:\\\\shared\"]},"
                      "\"@acme/util\":{\"env\":[\"X\"],\"write\":[]}}}",
                      m, true),
              "grants parse");
        Check(m.grants.Size() == 2 && m.grants[0].name.View() == StringView("greet") &&
                  m.grants[0].permissions.read.Size() == 2 &&
                  m.grants[0].permissions.read[1].View() == StringView("C:\\shared"),
              "a grant may be absolute");
        Check(m.grants[1].name.View() == StringView("@acme/util") &&
                  m.grants[1].permissions.env.Size() == 1 && m.grants[1].permissions.write.Empty(),
              "grants keep their order and lists");
        Check(m.permissions.Empty(), "no permissions in the app");
    }

    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"net\":[]}}", "unknown permission \"net\"");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":\"./x\"}}", "must be an array");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":[1]}}", "non-empty strings");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":[\"\"]}}", "non-empty strings");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":[\"C:/x\"]}}", "relative to the program");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"write\":[\"\\\\server\\\\x\"]}}",
                  "relative to the program");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"env\":[\"A=B\"]}}", "invalid variable name");
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":[\"./a\\u0001\"]}}", "control character");
    ExpectRefused("{\"grants\":[]}", "\"grants\" must be an object", true);
    ExpectRefused("{\"grants\":{\"Bad\":{}}}", "not a valid package name", true);
    ExpectRefused("{\"grants\":{\"good\":[]}}", "\"grants\".\"good\" must be an object", true);
    ExpectRefused("{\"grants\":{\"good\":{\"env\":[7]}}}", "non-empty strings", true);
    ExpectRefused("{\"name\":\"a\",\"permissions\":{\"read\":[\"./1\",\"./2\",\"./3\",\"./4\",\"./5\","
                  "\"./6\",\"./7\",\"./8\",\"./9\",\"./10\",\"./11\",\"./12\",\"./13\",\"./14\",\"./15\","
                  "\"./16\",\"./17\",\"./18\",\"./19\",\"./20\",\"./21\",\"./22\",\"./23\",\"./24\","
                  "\"./25\",\"./26\",\"./27\",\"./28\",\"./29\",\"./30\",\"./31\",\"./32\",\"./33\","
                  "\"./34\",\"./35\",\"./36\",\"./37\",\"./38\",\"./39\",\"./40\",\"./41\",\"./42\","
                  "\"./43\",\"./44\",\"./45\",\"./46\",\"./47\",\"./48\",\"./49\",\"./50\",\"./51\","
                  "\"./52\",\"./53\",\"./54\",\"./55\",\"./56\",\"./57\",\"./58\",\"./59\",\"./60\","
                  "\"./61\",\"./62\",\"./63\",\"./64\",\"./65\"]}}",
                  "more than 64 entries");

    // Refused manifests.
    ExpectRefused("[]", "top level");
    ExpectRefused("{\"name\":\"a\",\"name\":\"b\"}", "twice");
    ExpectRefused("{\"name\":\"a\",}", "trailing comma");
    ExpectRefused("{\"dependancies\":{}}", "unknown field \"dependancies\"", true);
    ExpectRefused("{\"name\":\"a\",\"main\":\"./x.js\"}", "\"main\" is not used");
    ExpectRefused("{\"name\":\"a\",\"scripts\":{}}", "never runs package scripts");
    ExpectRefused("{\"version\":\"1\"}", "\"name\" is required");
    ExpectRefused("{\"name\":\"Bad\"}", "valid package name");
    ExpectRefused("{\"name\":7}", "valid package name");
    ExpectRefused("{\"name\":\"a\",\"version\":1}", "\"version\" must be a string");
    ExpectRefused("{\"name\":\"a\",\"dependencies\":[]}", "must be an object");
    ExpectRefused("{\"name\":\"a\",\"dependencies\":{\"b\":\"^1.0.0\"}}", "file:<folder>");
    ExpectRefused("{\"name\":\"a\",\"dependencies\":{\"b\":\"file:\"}}", "file:<folder>");
    ExpectRefused("{\"name\":\"a\",\"dependencies\":{\"B\":\"file:./b\"}}", "not a valid package name");
    ExpectRefused("{\"name\":\"a\",\"permissions\":[]}", "must be an object");
    ExpectRefused("{\"name\":\"a\",\"exports\":7}", "string or an object");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"index.js\"}", "must start with ./");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./../x.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./a/./b.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./node_modules/x.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./a%2e%2e/x.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./a\\\\b.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./C:/x.js\"}", "stay inside");
    ExpectRefused("{\"name\":\"a\",\"exports\":\"./*.js\"}", "single target");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\".\":\"./a.js\",\"forge\":\"./b.js\"}}", "mixes");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./x\":\"./*.js\"}}", "its key has none");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./*/*\":\"./*.js\"}}", "more than one *");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./x\":[\"./a.js\"]}}", "arrays");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./x\":{\"./y\":\"./a.js\"}}}", "must not contain subpaths");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./x\":7}}", "string, null or a condition object");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"x\":\"./a.js\",\"./y\":\"./b.js\"}}", "mixes");
    ExpectRefused("{\"name\":\"a\",\"exports\":{\"./../x\":\"./a.js\"}}", "not a valid subpath");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "ManifestTest: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }

    std::printf("ManifestTest: all %d checks passed\n", g_checks);
    return 0;
}
