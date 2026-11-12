// Type declarations for the Forge runtime (Phase 8.4).
//
// These describe the globals Forge adds to standard JavaScript: Forge,
// fs, console, the timers, queueMicrotask and print. Use them with
// TypeScript or a JavaScript editor:
//
//   /// <reference path="path/to/forge/types/forge.d.ts" />
//
// or list this file under "files" in tsconfig.json, with "lib": ["es2023"]
// and no "dom" library (Forge has no DOM).
//
// They match the C++ in forge.cpp, runtime/Console.cpp and runtime/Fs.cpp.
// When a native function is added or changed, change this file in the same
// commit (rule R2 lists every native in kavach/Ops.h).
//
// Status: Phase 8 preview. `fs` will move to `Forge.fs` in Phase 9 (D14);
// the global `fs` then stays only behind a compatibility option.

export {};

declare global {
  /** The `code` of an error from fs, Forge.env or Kavach (see ForgeError). */
  type ForgeErrorCode =
    | "Unknown"
    | "InvalidArgument"
    | "InvalidOperation"
    | "NotSupported"
    | "NotImplemented"
    | "NotFound"
    | "AlreadyExists"
    | "PermissionDenied"
    | "Busy"
    | "Timeout"
    | "Cancelled"
    | "EndOfFile"
    | "IOError"
    | "OutOfMemory"
    | "BufferTooSmall"
    | "Overflow"
    | "Underflow"
    | "InvalidData"
    | "ParseError"
    | "PlatformError";

  /**
   * An error thrown (or a Promise rejected) by fs, Forge.env or a Kavach
   * refusal. It is an ordinary `Error` (a `TypeError` for a wrong argument
   * type) with extra own properties; which ones are present depends on
   * what failed. Wrong arguments to the timers or to Forge.exit and
   * Forge.exitCode throw a plain TypeError or RangeError without `code`.
   */
  interface ForgeError extends Error {
    code: ForgeErrorCode;
    /** The function that failed, for operating-system errors ("readFileSync"). */
    syscall?: string;
    /** The path involved: as checked by Kavach, or the real path after links. */
    path?: string;
    /** The Windows error number, when `code` is "PlatformError". */
    nativeCode?: number;
    /** The JavaScript name of the refused operation ("fs.readFileSync"), for Kavach refusals. */
    op?: string;
    /** The permission that was missing, for Kavach refusals. */
    permission?: "read" | "write" | "env" | "env (every variable)";
    /** The environment variable involved, for Forge.env refusals. */
    variable?: string;
  }

  /** Signals a script can listen for (Ctrl+C and Ctrl+Break on Windows). */
  type ForgeSignal = "SIGINT" | "SIGBREAK";

  interface ForgeStdStream {
    /**
     * Writes text (as UTF-8) or bytes without a line break. Returns the
     * number of bytes written. Text written to a console shows correctly
     * whatever the console code page.
     */
    write(data: string | Uint8Array | ArrayBuffer): number;
    /** True when this stream is a console window, false for a pipe or file. */
    isTerminal(): boolean;
  }

  interface ForgeEnv {
    /**
     * The value of an environment variable, or undefined when it is not
     * set. Needs `--allow-env=<name>` (or `--allow-env`); otherwise throws
     * a ForgeError with code "PermissionDenied". Names are case-insensitive
     * on Windows.
     */
    get(name: string): string | undefined;
    /** Whether an environment variable is set. Needs the same permission as get(). */
    has(name: string): boolean;
    /** Every environment variable. Needs `--allow-env` with no list. */
    toObject(): Record<string, string>;
  }

  /** The Forge namespace. The object and its sub-objects are frozen. */
  interface ForgeNamespace {
    /** Forge's version, for example "0.1.0". */
    readonly version: string;
    /** The operating system: "windows" (Linux and macOS ports come later). */
    readonly platform: "windows" | "linux" | "darwin";
    /** The arguments after the script name, as typed (frozen). */
    readonly args: readonly string[];
    /**
     * The exit code used when the script ends normally: a whole number
     * from 0 to 255. Setting anything else throws.
     */
    exitCode: number;
    /**
     * Ends the program now with `code` (default: exitCode). Nothing after
     * the call runs, not even `finally` blocks or pending timers; it cannot
     * be caught.
     */
    exit(code?: number): never;
    /**
     * Calls `listener` when the signal arrives. While at least one listener
     * is registered, the signal no longer ends the program (unless the
     * listener calls exit()); a second signal that arrives before the first
     * was handled still ends it at once.
     */
    addSignalListener(signal: ForgeSignal, listener: (signal: ForgeSignal) => void): void;
    removeSignalListener(signal: ForgeSignal, listener: (signal: ForgeSignal) => void): void;
    readonly env: Readonly<ForgeEnv>;
    readonly stdout: Readonly<ForgeStdStream>;
    readonly stderr: Readonly<ForgeStdStream>;
  }

  var Forge: ForgeNamespace;

  // ---------------------------------------------------------------- fs ----

  type ForgeFileType = "file" | "directory" | "symlink" | "other";

  interface ForgeStats {
    size: number;
    type: ForgeFileType;
    isFile: boolean;
    isDirectory: boolean;
    isSymbolicLink: boolean;
    /** The number of hard links. Forge refuses to write a file with more than one. */
    nlink: number;
    mtimeMs: number;
    atimeMs: number;
    ctimeMs: number;
    birthtimeMs: number;
  }

  interface ForgeDirEntry {
    name: string;
    type: ForgeFileType;
  }

  /** Text is written as UTF-8. */
  type ForgeWriteData = string | Uint8Array | ArrayBuffer;
  /** "utf8" (or "utf-8") reads text; anything else, or nothing, reads bytes. */
  type ForgeTextEncoding = "utf8" | "utf-8";

  /**
   * Options are read from the object's own data properties only: inherited
   * properties are ignored, and an accessor property or a Proxy is refused
   * with code "InvalidArgument".
   */
  interface ForgeRecursiveOptions {
    recursive?: boolean;
  }

  interface ForgeReaddirOptions {
    withFileTypes?: boolean;
  }

  /**
   * The filesystem. Every path is checked by Kavach: reading needs
   * `--allow-read`, changing needs `--allow-write`, and a refusal throws
   * (or, for the Promise-returning functions, rejects) a ForgeError with
   * code "PermissionDenied" or "InvalidArgument". Paths must be strings.
   */
  interface ForgeFs {
    readFileSync(path: string, encoding: ForgeTextEncoding): string;
    readFileSync(path: string, encoding?: undefined): Uint8Array;
    readFile(path: string, encoding: ForgeTextEncoding): Promise<string>;
    readFile(path: string, encoding?: undefined): Promise<Uint8Array>;

    /** Creates or replaces the file. Refused through a link or for a file with hard links. */
    writeFileSync(path: string, data: ForgeWriteData): void;
    writeFile(path: string, data: ForgeWriteData): Promise<void>;
    appendFileSync(path: string, data: ForgeWriteData): void;
    appendFile(path: string, data: ForgeWriteData): Promise<void>;

    existsSync(path: string): boolean;
    exists(path: string): Promise<boolean>;

    /** Follows links. */
    statSync(path: string): ForgeStats;
    stat(path: string): Promise<ForgeStats>;
    /** Describes a link itself rather than what it points to. */
    lstatSync(path: string): ForgeStats;
    lstat(path: string): Promise<ForgeStats>;

    readdirSync(path: string, options: ForgeReaddirOptions & { withFileTypes: true }): ForgeDirEntry[];
    readdirSync(path: string, options?: ForgeReaddirOptions): string[];
    readdir(path: string, options: ForgeReaddirOptions & { withFileTypes: true }): Promise<ForgeDirEntry[]>;
    readdir(path: string, options?: ForgeReaddirOptions): Promise<string[]>;

    /** The real path, after links and short names are resolved. */
    realpathSync(path: string): string;
    realpath(path: string): Promise<string>;

    mkdirSync(path: string, options?: ForgeRecursiveOptions): void;
    mkdir(path: string, options?: ForgeRecursiveOptions): Promise<void>;

    /**
     * Deletes a file, an empty folder, or (with recursive) a folder and its
     * contents. Deleting a link deletes the link; a recursive delete never
     * follows one.
     */
    rmSync(path: string, options?: ForgeRecursiveOptions): void;
    rm(path: string, options?: ForgeRecursiveOptions): Promise<void>;

    /** Both paths need write access. */
    renameSync(from: string, to: string): void;
    rename(from: string, to: string): Promise<void>;

    /** `from` needs read access and `to` needs write access. */
    copyFileSync(from: string, to: string): void;
    copyFile(from: string, to: string): Promise<void>;
  }

  var fs: ForgeFs;

  // ----------------------------------------------------------- console ----

  /**
   * console writes from C++ and never runs script code while formatting:
   * getters, Proxy traps and toString methods of logged values are not
   * called. Format directives: %s %d %i %f %o %O %c %%.
   */
  interface ForgeConsole {
    log(...data: unknown[]): void;
    info(...data: unknown[]): void;
    debug(...data: unknown[]): void;
    dirxml(...data: unknown[]): void;
    /** Prints like log() (no table layout yet). */
    table(...data: unknown[]): void;
    warn(...data: unknown[]): void;
    error(...data: unknown[]): void;
    dir(item?: unknown, options?: unknown): void;
    assert(condition?: boolean, ...data: unknown[]): void;
    trace(...data: unknown[]): void;
    count(label?: string): void;
    countReset(label?: string): void;
    time(label?: string): void;
    timeLog(label?: string, ...data: unknown[]): void;
    timeEnd(label?: string): void;
    group(...data: unknown[]): void;
    groupCollapsed(...data: unknown[]): void;
    groupEnd(): void;
  }

  var console: ForgeConsole;

  // ------------------------------------------------------------ timers ----

  /**
   * Runs `callback` once after `delay` milliseconds (0 when missing,
   * negative or NaN). Extra arguments are passed to it. Returns an id for
   * clearTimeout.
   */
  function setTimeout<A extends unknown[]>(
    callback: (...args: A) => void,
    delay?: number,
    ...args: A
  ): number;
  function setInterval<A extends unknown[]>(
    callback: (...args: A) => void,
    delay?: number,
    ...args: A
  ): number;
  /** Cancels a timeout or an interval. Unknown ids are ignored. */
  function clearTimeout(id?: number): void;
  function clearInterval(id?: number): void;
  function queueMicrotask(callback: () => void): void;

  /**
   * The engine shell's print, kept for older scripts: converts each value
   * with String() (which may run its toString) and writes them on one line.
   * New code should use console.log.
   */
  function print(...values: unknown[]): void;
}
