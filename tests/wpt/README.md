# Web Platform Tests coverage (decision D29)

Forge checks its web APIs against the [Web Platform Tests](https://github.com/web-platform-tests/wpt)
(WPT) without running any WPT code. Each case is transcribed by hand into
Forge's own tests (`tests/web-*.js`) and reviewed like any other Forge
code. This folder records which cases are covered:

- `coverage.json`: for one pinned WPT commit, every subtest of every
  tracked WPT file, mapped to the Forge check that covers it, or to the
  reason it does not apply (most often: it needs a browser feature such as
  an iframe). Each file's SHA-1 is recorded too, so a change upstream shows
  up.
- `LICENSE.md`: WPT's license (3-Clause BSD). Cases transcribed from WPT
  are derived from WPT and stay under that license.

`tools/wpt-coverage.py --check` runs in CI (offline): every Forge check the
record names must exist. When moving to a newer WPT commit, or adding an
API, a maintainer runs `tools/wpt-coverage.py --upstream <clone>` against a
git clone of WPT (a blobless clone is enough): it reads the files as text,
never runs them, and lists every subtest not yet recorded, every recorded
file that changed, and every file of a tracked directory the record does
not list.

**Why not run WPT itself.** The WPT harness and test files are third-party
code: running them in CI and on the build machine would execute code that
Forge's maintainers did not review, from a repository with thousands of
contributors, which a security-first runtime does not do. Transcribing
costs more effort per API, but everything Forge runs is Forge's own code,
and the record still answers "which WPT cases pass" exactly. See
PHASE10_DESIGN.md section 4.

| Phase | WPT files recorded |
|---|---|
| 10.3 | `dom/abort/` (all of it) and `dom/events/AddEventListenerOptions-signal.any.js` |
| 10.4 | `webmessaging/*.any.js`, `webmessaging/message-channels/` (all of it) and `html/webappapis/structured-clone/` (all of it) |
| 10.1, 10.2 | Transcribed before this record existed (`tests/web-apis.js`, `tests/web-events.js`); to be added |
