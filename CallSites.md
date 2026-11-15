# Where C++ calls into JavaScript

Rule R24 of the master plan asks for a list of every place where Forge's
C++ runs script code: a callback, a getter, a `toString`, a Proxy trap. Each
one is a place where a script can act in the middle of a native function,
so each one needs an answer to two questions:

1. **Is the native's state safe at that moment?** Script code must never
   run between a Kavach decision and the operation it allows, or after a
   native has changed state it still relies on.
2. **Whose authority does the script code run with?** In Phase 8 there is
   one policy for the whole program, so the answer is always "the
   program's". From Phase 9, when packages get their own permissions, a
   callback must run with the authority of the package that **registered**
   it, never that of the code that happens to fire it.

`tests/fuzz-natives.js` checks this list on every build: it passes values
whose getters, conversions and Proxy traps count their own calls, and fails
if a native outside the "may run script code" rows below ran any of them.

## Calls into script code

| Where | What runs | When | State at that moment | Authority (Phase 8 / from Phase 9) |
|---|---|---|---|---|
| Timers (`forge.cpp`, timer dispatch) | The `setTimeout` / `setInterval` callback, with its extra arguments | From the event loop, never inside another native | No native is running | The program / the package that called `setTimeout`. Phase 9.3: the callback runs in the realm that created the timer, and only that realm can cancel it. |
| Microtasks (`forge.cpp`, microtask queue) | `queueMicrotask` callbacks and the engine's Promise jobs (`then` handlers) | At a microtask checkpoint, after the current script or callback returns | No native is running | The program / the package that queued it |
| Signal listeners (`forge.cpp`, signal dispatch) | `Forge.addSignalListener` listeners, with the signal name | From the event loop on the main thread; the Windows signal thread never runs script | No native is running | The program / the package that added the listener |
| Promise results of async `fs` functions (`runtime/Fs.cpp`) | If the result is an object (stats, an array, a `Uint8Array`), the engine looks up `then` on it and calls it if a script added one to a prototype | As a Promise job, after the native returned | The operation is finished; nothing depends on the result any more | The program. Phase 9.3 closes the open item: a package cannot add `then` to the program's prototypes (an intrinsic that crosses is replaced by the package's own, frozen one), and the prototypes the pure packages share are frozen. |
| `setTimeout` / `setInterval` delay (`JS::ToNumber`) | `valueOf` / `Symbol.toPrimitive` of the delay argument | Inside the native, **before** the timer is created | Nothing has been changed yet; a throw leaves no timer | The caller |
| `clearTimeout` / `clearInterval` id (`JS::ToInt32`) | `valueOf` / `Symbol.toPrimitive` of the id | Inside the native, before any timer is looked up | Nothing has been changed yet | The caller |
| Module evaluation (`runtime/Modules.cpp`, `RunEntryModule`) | The program's modules, including top-level `await` continuations | The entry module runs once, after its whole static graph is loaded and linked; a dynamic `import()` evaluates its module in a promise job | No native is running; a rejected top-level `await` is reported as an uncaught exception by a handler Forge adds | The program / the package that owns the module |
| Package evaluation (`runtime/Modules.cpp`, `import.meta.load` of a facade, Phase 9.3) | Another package's modules, in that package's compartment | When the facade module evaluates, before the importer; only the facade's generated code can call it | Nothing depends on state after the call: it defines `import.meta.namespace` and returns a promise | The package that owns the modules (its own compartment) |
| Assignment to an inherited built-in property in a frozen realm (`runtime/Compartments.cpp`, the "override mistake" setters, Phase 9.3) | If the object already has the property as its own accessor, that accessor's setter | Inside the assignment the script itself wrote (`obj.toString = f`) | No other native is running | The caller |
| `print` (`JS::ToString`) | `toString` / `Symbol.toPrimitive` of each value | Inside the native, while the line is built, before anything is written | A throw leaves nothing written | The caller |

`print` and the timer functions are the only natives that run script code
while they are running. They are kept compatible with the engine shell and
with the web; none of them makes a Kavach decision.

## Places that deliberately do not call into script code

| Where | How it avoids running script code |
|---|---|
| Every path argument (`runtime/JsUtil.cpp`, `CheckPathArg`) | Only a primitive string is accepted. No `ToString`, so an object cannot choose its path, and a path cannot change between the check and the use (rule R3). |
| Write data (`AsByteSpan`, `CopyWriteData`) | A string, `Uint8Array` or `ArrayBuffer` is read through the engine's internal slots, never through `length` or `buffer` properties; the bytes are copied before the operation starts. |
| `fs` options (`runtime/Fs.cpp`, `ParseFlag`) | Phase 8.4: only an own data property counts. Inherited properties are ignored (so `Object.prototype.recursive = true` cannot make `rmSync` recursive), accessor properties and Proxies are refused with `InvalidArgument`, and `JS::ToBoolean` runs no script. |
| `fs` encoding argument | Only the strings `"utf8"` and `"utf-8"` are recognised; nothing is converted. |
| Errors thrown by Forge (`ThrowJsError`, `DefineStringOnPendingError`) | Phase 8.4: `code`, `path`, `op`, `permission`, `syscall`, `nativeCode` and `variable` are **defined** as own properties with the exception taken off the context meanwhile. Before 8.4, `ThrowJsError` used `JS_SetProperty`, which would have called a setter that a script placed on `Error.prototype`, inside the native, with an exception pending. |
| `console` and the formatter (`runtime/Inspect.cpp`) | Properties are read with `JS_GetOwnPropertyDescriptor`, getters are shown as `[Getter]` and never called, Proxies are shown without calling a trap, `Map` and `Set` are read with the engine's own iteration (an overridden `forEach` or `entries` is not called), and `toString` is never called. Numbers and BigInts are converted with `JS::ToString`, which runs no script for primitives. Labels (`console.count`, `console.time`) go through the same formatter. |
| `Forge.exit`, `Forge.exitCode` | Only a number is accepted; nothing is converted. |
| `Forge.env` names, signal names | Only a string is accepted; nothing is converted. |
| `Forge.stdout.write`, `Forge.stderr.write` | Same as write data. |
| Uncaught exceptions | Reported with `JS::ErrorReportBuilder::NoSideEffects`, so a thrown object's `toString` or `message` getter is not called. |
| Module loading (`runtime/Modules.cpp`, the load hook) | Phase 9.1: resolving a specifier, the Kavach check, reading and compiling a file run no script. The specifier string comes from the engine (`JS::GetModuleRequestSpecifier`); for `import(x)` the engine converts `x` to a string itself, as part of `import()`, in the caller's own code, before Forge is asked. A dynamic import finishes in a promise job that reads only the array Forge made for it, which script never sees. |
| `import.meta` (`runtime/Modules.cpp`, the metadata hook) | Defines `url`, `main` and `resolve` on the object the engine creates; runs no script. |
| `import.meta.resolve` | Only a string is accepted; nothing is converted. |
| The wrapper policy (`runtime/Compartments.cpp`, the pre-wrap callback, Phase 9.3) | Runs for every object that crosses between compartments. Reads only Forge's own records (a WeakMap and the list of intrinsics) and the object's class; never a property of the object. |
| The lockdown walk (`runtime/Compartments.cpp`, Phase 9.3) | Reads own property descriptors (getters are recorded, never called) and the prototype; the one script it runs is Forge's own fixed list of hidden intrinsics, in a realm where no other code has run yet. |
| Facade generation (`runtime/Modules.cpp`, Phase 9.3) | Reads the keys of the package's module namespace, which touches no binding and runs no code; each name is written as an escaped string literal. |
| Formatting another compartment's object (`runtime/Inspect.cpp`, Phase 9.3) | Looks through the cross-compartment wrapper with `js::UncheckedUnwrap` and formats the object inside its own realm, with the same no-script rules as above. |
| Self-test natives (`__asyncSelfTestRecord`) | Exist only while `forge --self-test` runs, which runs no user script. |

## Rules for new code

- A native that must run script code does it **before** it makes a Kavach
  decision or changes any state, and never between a decision and the
  operation it allows.
- A value obtained from script code after a Kavach decision is never used
  to choose what that decision covered.
- Reading an object passed by a script uses own data properties
  (`JS_GetOwnPropertyDescriptor`), not `JS_GetProperty`; writing to an
  object a script can see uses `JS_DefineProperty`, not `JS_SetProperty`.
- A new row in this file comes with the code that adds the call, and the
  native fuzzer's `MAY_RUN_SCRIPT` list changes in the same commit.
