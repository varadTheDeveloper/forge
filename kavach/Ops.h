#pragma once

// The Kavach op table (Phase 8.2, rule R2: deny by default, every native
// operation listed in one table).
//
// Every function or accessor that Forge makes reachable from JavaScript has
// exactly one row here: its JavaScript name and the permission it needs.
//   * A native that checks a permission names its row (Op::FsReadFileSync),
//     so a missing row is a compile error.
//   * `forge --self-test` walks every object Forge adds to the global and
//     fails if it finds a function with no row, or a row with no function.
// "None" means the operation touches nothing outside the process (timers,
// console output, exit). It is still listed, so adding a new native without
// thinking about its permission is impossible.

#include <stddef.h>
#include <stdint.h>

namespace forge::kavach
{

enum class Permission : uint8_t
{
    None,
    Read,    // read files and folders under a granted path
    Write,   // create, change or delete under a granted path
    Env,     // read one granted environment variable
    EnvAll,  // read every environment variable
    ReadWrite,  // read one path and write another (fs.copyFile: source, destination)
};

/// What a filesystem check asks for. An op with Permission::Read or Write
/// asks for that; Permission::ReadWrite asks for Read on one path and Write
/// on another (Phase 8.3).
enum class Access : uint8_t
{
    Read,
    Write,
};

/// True if `op`'s permission includes `access`.
bool OpAllows(
    Permission permission,
    Access access) noexcept;

// X(Id, "JavaScript name", Permission)
#define FORGE_KAVACH_OPS(X)                                      \
    X(SetTimeout, "setTimeout", None)                            \
    X(SetInterval, "setInterval", None)                          \
    X(ClearTimeout, "clearTimeout", None)                        \
    X(ClearInterval, "clearInterval", None)                      \
    X(QueueMicrotask, "queueMicrotask", None)                    \
    X(Print, "print", None)                                      \
    X(FsReadFileSync, "fs.readFileSync", Read)                   \
    X(FsReadFile, "fs.readFile", Read)                           \
    X(FsWriteFileSync, "fs.writeFileSync", Write)                \
    X(FsWriteFile, "fs.writeFile", Write)                        \
    X(FsAppendFileSync, "fs.appendFileSync", Write)              \
    X(FsAppendFile, "fs.appendFile", Write)                      \
    X(FsExistsSync, "fs.existsSync", Read)                       \
    X(FsExists, "fs.exists", Read)                               \
    X(FsMkdirSync, "fs.mkdirSync", Write)                        \
    X(FsMkdir, "fs.mkdir", Write)                                \
    X(FsRmSync, "fs.rmSync", Write)                              \
    X(FsRm, "fs.rm", Write)                                      \
    X(FsStatSync, "fs.statSync", Read)                           \
    X(FsStat, "fs.stat", Read)                                   \
    X(FsLstatSync, "fs.lstatSync", Read)                         \
    X(FsLstat, "fs.lstat", Read)                                 \
    X(FsReaddirSync, "fs.readdirSync", Read)                     \
    X(FsReaddir, "fs.readdir", Read)                             \
    X(FsRealpathSync, "fs.realpathSync", Read)                   \
    X(FsRealpath, "fs.realpath", Read)                           \
    X(FsRenameSync, "fs.renameSync", Write)                      \
    X(FsRename, "fs.rename", Write)                              \
    X(FsCopyFileSync, "fs.copyFileSync", ReadWrite)              \
    X(FsCopyFile, "fs.copyFile", ReadWrite)                      \
    X(ForgeExit, "Forge.exit", None)                             \
    X(ForgeExitCode, "Forge.exitCode", None)                     \
    X(ForgeAddSignalListener, "Forge.addSignalListener", None)   \
    X(ForgeRemoveSignalListener, "Forge.removeSignalListener", None) \
    X(EnvGet, "Forge.env.get", Env)                              \
    X(EnvHas, "Forge.env.has", Env)                              \
    X(EnvToObject, "Forge.env.toObject", EnvAll)                 \
    X(StdoutWrite, "Forge.stdout.write", None)                   \
    X(StdoutIsTerminal, "Forge.stdout.isTerminal", None)         \
    X(StderrWrite, "Forge.stderr.write", None)                   \
    X(StderrIsTerminal, "Forge.stderr.isTerminal", None)         \
    X(ConsoleLog, "console.log", None)                           \
    X(ConsoleInfo, "console.info", None)                         \
    X(ConsoleDebug, "console.debug", None)                       \
    X(ConsoleDirxml, "console.dirxml", None)                     \
    X(ConsoleTable, "console.table", None)                       \
    X(ConsoleWarn, "console.warn", None)                         \
    X(ConsoleError, "console.error", None)                       \
    X(ConsoleDir, "console.dir", None)                           \
    X(ConsoleAssert, "console.assert", None)                     \
    X(ConsoleTrace, "console.trace", None)                       \
    X(ConsoleCount, "console.count", None)                       \
    X(ConsoleCountReset, "console.countReset", None)             \
    X(ConsoleTime, "console.time", None)                         \
    X(ConsoleTimeLog, "console.timeLog", None)                   \
    X(ConsoleTimeEnd, "console.timeEnd", None)                   \
    X(ConsoleGroup, "console.group", None)                       \
    X(ConsoleGroupCollapsed, "console.groupCollapsed", None)     \
    X(ConsoleGroupEnd, "console.groupEnd", None)

enum class Op : uint16_t
{
#define FORGE_KAVACH_OP_ENUM(id, name, permission) id,
    FORGE_KAVACH_OPS(FORGE_KAVACH_OP_ENUM)
#undef FORGE_KAVACH_OP_ENUM
    Count
};

constexpr size_t kOpCount = static_cast<size_t>(Op::Count);

struct OpInfo
{
    const char* name;
    Permission permission;
};

/// Row `op` of the table.
const OpInfo& GetOpInfo(
    Op op) noexcept;

/// "read", "write", "env", "env (every variable)" or "none".
const char* PermissionName(
    Permission permission) noexcept;

/// The op whose JavaScript name is `name`, or Op::Count if there is none.
Op FindOp(
    const char* name,
    size_t length) noexcept;

} // namespace forge::kavach
