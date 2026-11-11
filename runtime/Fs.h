#pragma once

// globalThis.fs (Phase 8.3): every filesystem function, synchronous and
// Promise-returning, built on kavach/SecureFs (handle-based, checked).
//
//   readFileSync(path, encoding?)        readFile(path, encoding?)
//   writeFileSync(path, data)            writeFile(path, data)
//   appendFileSync(path, data)           appendFile(path, data)
//   existsSync(path)                     exists(path)
//   statSync(path)  lstatSync(path)      stat(path)  lstat(path)
//   readdirSync(path, {withFileTypes})   readdir(path, {withFileTypes})
//   realpathSync(path)                   realpath(path)
//   mkdirSync(path, {recursive})         mkdir(path, {recursive})
//   rmSync(path, {recursive})            rm(path, {recursive})
//   renameSync(from, to)                 rename(from, to)
//   copyFileSync(from, to)               copyFile(from, to)
//
// `encoding` is "utf8" (or "utf-8") for a string; otherwise a Uint8Array.
// `data` is a string (written as UTF-8), a Uint8Array or an ArrayBuffer.
// Stats: { size, type, isFile, isDirectory, isSymbolicLink, nlink, mtimeMs,
// atimeMs, ctimeMs, birthtimeMs }; type is "file", "directory", "symlink" or
// "other". Every path goes through Kavach; a refusal from a Promise-returning
// function is a rejected Promise, never a synchronous throw.

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

#include "forge-core/ThreadPool.h"
#include "forge-core/platform/IoLoop.h"

namespace forge::runtime
{

/// Defines globalThis.fs and starts tracing pending async operations.
[[nodiscard]]
bool DefineFs(
    JSContext* cx,
    JS::HandleObject global);

/// Drops every async operation still pending (their Promises stay
/// unsettled) and stops tracing. Call after the worker pool has been shut
/// down and before the context is destroyed.
void ShutdownFs(
    JSContext* cx);

// Implemented by forge.cpp, which owns the event loop and the worker pool.
forge::core::ThreadPool& HostWorkerPool(JSContext* cx);
forge::core::platform::IoLoop& HostIoLoop(JSContext* cx);
void HostAsyncStarted(JSContext* cx);
void HostAsyncFinished(JSContext* cx);

} // namespace forge::runtime
