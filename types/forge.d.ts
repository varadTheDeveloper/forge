// Type declarations for the Forge runtime (Phase 8.4).
//
// These describe the globals Forge adds to standard JavaScript: Forge,
// fs, console, the timers, queueMicrotask and print, and (Phase 10) the
// web-standard APIs: DOMException, TextEncoder, TextDecoder, atob, btoa;
// events (Event, EventTarget, CustomEvent, ErrorEvent,
// PromiseRejectionEvent), reportError, self, navigator and the global
// event handlers. Use them with
// TypeScript or a JavaScript editor:
//
//   /// <reference path="path/to/forge/types/forge.d.ts" />
//
// or list this file under "files" in tsconfig.json, with "lib": ["es2023"]
// and no "dom" library (Forge has no DOM).
//
// They match the C++ in forge.cpp, runtime/Console.cpp and runtime/Fs.cpp,
// and runtime/WebApis.js for the web APIs.
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
    /**
     * The package whose permissions refused, when a package's own code was
     * refused (Phase 9.4). Absent when the program itself was refused.
     */
    package?: string;
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

  // ----------------------------------------------------------- modules ----

  /**
   * Phase 9.1: every program file is an ES module. Modules load only from
   * the program's module root (the nearest folder with forge.json, or the
   * entry's folder); no permission is needed for that, and no read grant
   * extends it.
   */
  interface ImportMeta {
    /** The module's file: URL, for example "file:///C:/app/src/main.js". */
    url: string;
    /** True in the module Forge was started with, false in every other. */
    main: boolean;
    /**
     * The URL `specifier` would load from this module, without loading it.
     * Throws a ForgeError for a specifier Forge never loads (`node:`, a
     * package name before Phase 9.2, a network URL). Only strings are
     * accepted.
     */
    resolve(specifier: string): string;
  }

  /**
   * The engine shell's print, kept for older scripts: converts each value
   * with String() (which may run its toString) and writes them on one line.
   * New code should use console.log.
   */
  function print(...values: unknown[]): void;

  // ---------------------------------------------------------------------
  // Phase 10.1: web-standard APIs (WinterTC.md). Every one is available in
  // packages too, needs no permission, and is frozen inside a package.
  // ---------------------------------------------------------------------

  /**
   * WebIDL's exception type. A real Error (it has a stack, and
   * `instanceof Error` is true); its name, message and code are read-only
   * and inherited, never own properties.
   */
  class DOMException extends Error {
    constructor(message?: string, name?: string);
    /** "Error" when no name was given. */
    readonly name: string;
    readonly message: string;
    /** The legacy code of the name (InvalidCharacterError is 5), or 0. */
    readonly code: number;
    static readonly INDEX_SIZE_ERR: 1;
    static readonly DOMSTRING_SIZE_ERR: 2;
    static readonly HIERARCHY_REQUEST_ERR: 3;
    static readonly WRONG_DOCUMENT_ERR: 4;
    static readonly INVALID_CHARACTER_ERR: 5;
    static readonly NO_DATA_ALLOWED_ERR: 6;
    static readonly NO_MODIFICATION_ALLOWED_ERR: 7;
    static readonly NOT_FOUND_ERR: 8;
    static readonly NOT_SUPPORTED_ERR: 9;
    static readonly INUSE_ATTRIBUTE_ERR: 10;
    static readonly INVALID_STATE_ERR: 11;
    static readonly SYNTAX_ERR: 12;
    static readonly INVALID_MODIFICATION_ERR: 13;
    static readonly NAMESPACE_ERR: 14;
    static readonly INVALID_ACCESS_ERR: 15;
    static readonly VALIDATION_ERR: 16;
    static readonly TYPE_MISMATCH_ERR: 17;
    static readonly SECURITY_ERR: 18;
    static readonly NETWORK_ERR: 19;
    static readonly ABORT_ERR: 20;
    static readonly URL_MISMATCH_ERR: 21;
    static readonly QUOTA_EXCEEDED_ERR: 22;
    static readonly TIMEOUT_ERR: 23;
    static readonly INVALID_NODE_TYPE_ERR: 24;
    static readonly DATA_CLONE_ERR: 25;
    readonly INDEX_SIZE_ERR: 1;
    readonly DOMSTRING_SIZE_ERR: 2;
    readonly HIERARCHY_REQUEST_ERR: 3;
    readonly WRONG_DOCUMENT_ERR: 4;
    readonly INVALID_CHARACTER_ERR: 5;
    readonly NO_DATA_ALLOWED_ERR: 6;
    readonly NO_MODIFICATION_ALLOWED_ERR: 7;
    readonly NOT_FOUND_ERR: 8;
    readonly NOT_SUPPORTED_ERR: 9;
    readonly INUSE_ATTRIBUTE_ERR: 10;
    readonly INVALID_STATE_ERR: 11;
    readonly SYNTAX_ERR: 12;
    readonly INVALID_MODIFICATION_ERR: 13;
    readonly NAMESPACE_ERR: 14;
    readonly INVALID_ACCESS_ERR: 15;
    readonly VALIDATION_ERR: 16;
    readonly TYPE_MISMATCH_ERR: 17;
    readonly SECURITY_ERR: 18;
    readonly NETWORK_ERR: 19;
    readonly ABORT_ERR: 20;
    readonly URL_MISMATCH_ERR: 21;
    readonly QUOTA_EXCEEDED_ERR: 22;
    readonly TIMEOUT_ERR: 23;
    readonly INVALID_NODE_TYPE_ERR: 24;
    readonly DATA_CLONE_ERR: 25;
  }

  interface TextEncoderEncodeIntoResult {
    /** UTF-16 code units of the source that were encoded. */
    read: number;
    /** Bytes written to the destination. */
    written: number;
  }

  /** Encodes strings as UTF-8. Lone surrogates become U+FFFD. */
  class TextEncoder {
    constructor();
    readonly encoding: "utf-8";
    encode(input?: string): Uint8Array;
    /** Encodes as many whole characters as fit in `destination`. */
    encodeInto(source: string, destination: Uint8Array): TextEncoderEncodeIntoResult;
  }

  interface TextDecoderOptions {
    /** Throw a TypeError on invalid input instead of inserting U+FFFD. */
    fatal?: boolean;
    /** Keep a leading byte-order mark instead of removing it. */
    ignoreBOM?: boolean;
  }

  interface TextDecodeOptions {
    /** More input follows: keep an incomplete character for the next call. */
    stream?: boolean;
  }

  /**
   * Decodes bytes as text. Supports UTF-8, UTF-16LE, UTF-16BE, every
   * single-byte encoding of the Encoding Standard (windows-1252, the
   * ISO-8859 family, KOI8, ...) and x-user-defined, under all their labels.
   * The CJK multi-byte encodings throw a RangeError for now.
   */
  class TextDecoder {
    constructor(label?: string, options?: TextDecoderOptions);
    /** The encoding's name in lower case, for example "utf-8". */
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    decode(input?: ArrayBuffer | ArrayBufferView, options?: TextDecodeOptions): string;
  }

  /**
   * Decodes base64 (ASCII whitespace ignored, padding optional) to a string
   * of one character per byte. Throws a DOMException named
   * "InvalidCharacterError" for anything else.
   */
  function atob(data: string): string;

  /**
   * Encodes a string of characters U+0000 to U+00FF as base64. Throws a
   * DOMException named "InvalidCharacterError" for any other character.
   */
  function btoa(data: string): string;
  // ---------------------------------------------------------------------
  // Phase 10.2: events (WinterTC.md). The global object is an EventTarget.
  // ---------------------------------------------------------------------

  interface EventInit {
    bubbles?: boolean;
    cancelable?: boolean;
    composed?: boolean;
  }

  class Event {
    constructor(type: string, eventInitDict?: EventInit);
    readonly type: string;
    readonly target: EventTarget | null;
    /** Legacy alias of target. */
    readonly srcElement: EventTarget | null;
    readonly currentTarget: EventTarget | null;
    /** [target] while the event is dispatched, [] otherwise. */
    composedPath(): EventTarget[];
    readonly eventPhase: number;
    static readonly NONE: 0;
    static readonly CAPTURING_PHASE: 1;
    static readonly AT_TARGET: 2;
    static readonly BUBBLING_PHASE: 3;
    readonly NONE: 0;
    readonly CAPTURING_PHASE: 1;
    readonly AT_TARGET: 2;
    readonly BUBBLING_PHASE: 3;
    stopPropagation(): void;
    /** Legacy: true after stopPropagation(); setting it to true stops propagation. */
    cancelBubble: boolean;
    stopImmediatePropagation(): void;
    readonly bubbles: boolean;
    readonly cancelable: boolean;
    /** Legacy: false once the event is cancelled; setting false cancels it. */
    returnValue: boolean;
    preventDefault(): void;
    readonly defaultPrevented: boolean;
    readonly composed: boolean;
    /** True only for events Forge itself dispatches. */
    readonly isTrusted: boolean;
    /** Milliseconds since Forge started, in steps of 0.1 ms. */
    readonly timeStamp: number;
    /** Legacy. */
    initEvent(type: string, bubbles?: boolean, cancelable?: boolean): void;
  }

  interface CustomEventInit<T = any> extends EventInit {
    detail?: T;
  }

  class CustomEvent<T = any> extends Event {
    constructor(type: string, eventInitDict?: CustomEventInit<T>);
    readonly detail: T;
    /** Legacy. */
    initCustomEvent(type: string, bubbles?: boolean, cancelable?: boolean, detail?: T): void;
  }

  interface ErrorEventInit extends EventInit {
    message?: string;
    filename?: string;
    lineno?: number;
    colno?: number;
    error?: any;
  }

  class ErrorEvent extends Event {
    constructor(type: string, eventInitDict?: ErrorEventInit);
    readonly message: string;
    readonly filename: string;
    readonly lineno: number;
    readonly colno: number;
    readonly error: any;
  }

  interface PromiseRejectionEventInit extends EventInit {
    promise: object;
    reason?: any;
  }

  class PromiseRejectionEvent extends Event {
    constructor(type: string, eventInitDict: PromiseRejectionEventInit);
    readonly promise: object;
    readonly reason: any;
  }

  interface EventListener {
    (evt: Event): void;
  }

  interface EventListenerObject {
    handleEvent(object: Event): void;
  }

  type EventListenerOrEventListenerObject = EventListener | EventListenerObject;

  interface EventListenerOptions {
    capture?: boolean;
  }

  interface AddEventListenerOptions extends EventListenerOptions {
    once?: boolean;
    /** A passive listener's preventDefault() does nothing. */
    passive?: boolean;
    /** Aborting it removes the listener; an aborted one adds nothing. */
    signal?: AbortSignal;
  }

  class EventTarget {
    constructor();
    addEventListener(type: string, callback: EventListenerOrEventListenerObject | null,
                     options?: AddEventListenerOptions | boolean): void;
    removeEventListener(type: string, callback: EventListenerOrEventListenerObject | null,
                        options?: EventListenerOptions | boolean): void;
    /** False if a listener cancelled the event. */
    dispatchEvent(event: Event): boolean;
  }

  interface AbortSignalEventMap {
    abort: Event;
  }

  /**
   * Made by AbortController and the static methods; `new AbortSignal()` is
   * a TypeError. A signal of another package works here too.
   */
  class AbortSignal extends EventTarget {
    private constructor();
    /** An already aborted signal (reason: an "AbortError" DOMException by default). */
    static abort(reason?: any): AbortSignal;
    /**
     * Aborts with a "TimeoutError" DOMException after `milliseconds` (an
     * integer from 0 to 2^53 - 1). Its timer does not keep the program
     * running.
     */
    static timeout(milliseconds: number): AbortSignal;
    /** Aborts as soon as any of `signals` does, with that signal's reason. */
    static any(signals: Iterable<AbortSignal>): AbortSignal;
    readonly aborted: boolean;
    readonly reason: any;
    /** Throws `reason` if aborted. */
    throwIfAborted(): void;
    onabort: ((this: AbortSignal, event: Event) => any) | null;
    addEventListener<K extends keyof AbortSignalEventMap>(
      type: K, listener: (this: AbortSignal, event: AbortSignalEventMap[K]) => any,
      options?: AddEventListenerOptions | boolean): void;
    addEventListener(type: string, callback: EventListenerOrEventListenerObject | null,
                     options?: AddEventListenerOptions | boolean): void;
  }

  class AbortController {
    constructor();
    readonly signal: AbortSignal;
    /** Aborts `signal` (reason: an "AbortError" DOMException by default). */
    abort(reason?: any): void;
  }

  /**
   * The global object's events. "error" (an uncaught exception) and
   * "unhandledrejection" are cancellable: a listener that cancels one keeps
   * the program running. Inside a package these never fire: only the
   * program handles uncaught errors.
   */
  interface ForgeGlobalEventMap {
    error: ErrorEvent;
    unhandledrejection: PromiseRejectionEvent;
    rejectionhandled: PromiseRejectionEvent;
  }

  function addEventListener<K extends keyof ForgeGlobalEventMap>(
    type: K, listener: (event: ForgeGlobalEventMap[K]) => any,
    options?: AddEventListenerOptions | boolean): void;
  function addEventListener(type: string, listener: EventListenerOrEventListenerObject | null,
                            options?: AddEventListenerOptions | boolean): void;
  function removeEventListener<K extends keyof ForgeGlobalEventMap>(
    type: K, listener: (event: ForgeGlobalEventMap[K]) => any,
    options?: EventListenerOptions | boolean): void;
  function removeEventListener(type: string, listener: EventListenerOrEventListenerObject | null,
                               options?: EventListenerOptions | boolean): void;
  function dispatchEvent(event: Event): boolean;

  /** Reports `e` as an uncaught exception would be (an "error" event first), without throwing. */
  function reportError(e: any): void;

  /** The global object. */
  var self: typeof globalThis;

  interface ForgeNavigator {
    /** "Forge" (no version, decision D13). */
    readonly userAgent: string;
  }

  /** Frozen. */
  var navigator: ForgeNavigator;

  /** Runs for every "error" event; returning true cancels it. */
  var onerror: ((message: string | Event, source?: string, lineno?: number, colno?: number,
                 error?: any) => any) | null;
  /** Runs for every "unhandledrejection" event; returning false cancels it. */
  var onunhandledrejection: ((event: PromiseRejectionEvent) => any) | null;
  var onrejectionhandled: ((event: PromiseRejectionEvent) => any) | null;
}
