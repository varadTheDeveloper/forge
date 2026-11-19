"""Tracks which Web Platform Tests cases Forge's own tests cover (decision D29).

Forge never runs WPT code. Its tests transcribe WPT cases by hand, and
tests/wpt/coverage.json records, for a pinned WPT commit, every subtest of
every tracked WPT file: the Forge check that covers it, or why it does not
apply. This script keeps that record honest.

Usage (from the repository root):

    python tools/wpt-coverage.py --check
        Offline (CI): every Forge check the record names exists in its test
        file, and every subtest that is not covered has a reason.

    python tools/wpt-coverage.py --upstream WPT_CLONE [--commit SHA]
        Maintainer use: reads the tracked WPT files AS TEXT from a git clone
        of WPT (a blobless clone is enough) at the pinned commit, or at SHA,
        and reports subtests missing from the record, files that changed
        since the pin, and files in a tracked directory that the record does
        not list. Nothing from the clone is executed: files are read with
        `git show` and scanned with regular expressions.

Exit code 1 if anything is wrong or missing.
"""

import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECORD = os.path.join(ROOT, "tests", "wpt", "coverage.json")

# A testharness subtest: test(...), async_test(...) or promise_test(...)
# ends with `}, "name");` (the name may use ' " or `, with escapes). A file
# can list other patterns ("name_patterns": group 1 the quote, group 2 the
# name), for example the structured clone battery's check('name', ...).
UPSTREAM_NAME = re.compile(r"\}\s*,\s*([\"'`])((?:\\.|(?!\1)[^\\])*)\1\s*\)")
# A Forge check's name: any double-quoted string literal of the Forge test
# file (names also appear in data tables there, not only in test(...)).
FORGE_STRING = re.compile(r"\"((?:\\.|[^\"\\\n])*)\"")


def unescape(text):
    return re.sub(r"\\(.)", r"\1", text)


def load_record():
    with open(RECORD, encoding="utf-8") as f:
        return json.load(f)


def forge_names(path, cache):
    if path not in cache:
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            cache[path] = {unescape(n) for n in FORGE_STRING.findall(f.read())}
    return cache[path]


def check(record):
    problems = []
    cache = {}

    for wpt_path, entry in record["files"].items():
        if "not_applicable" in entry:
            if not entry["not_applicable"].strip():
                problems.append(f"{wpt_path}: not_applicable needs a reason")
            continue

        forge_file = entry.get("forge")
        if not forge_file or not os.path.exists(os.path.join(ROOT, forge_file)):
            problems.append(f"{wpt_path}: Forge test file {forge_file!r} does not exist")
            continue

        names = forge_names(forge_file, cache)

        for subtest, covered_by in entry["subtests"].items():
            if isinstance(covered_by, dict):
                if not covered_by.get("not_applicable", "").strip():
                    problems.append(f"{wpt_path}: '{subtest}' needs a reason")
            elif covered_by not in names:
                problems.append(f"{wpt_path}: '{subtest}' -> no check named '{covered_by}' "
                                f"in {forge_file}")

    return problems


def git(clone, *args):
    return subprocess.run(["git", "-C", clone, *args], check=True, capture_output=True).stdout


def upstream_subtests(text, entry):
    substitutions = entry.get("substitutions", {})
    expand = entry.get("expand", {})
    patterns = [re.compile(p, re.S) for p in entry.get("name_patterns", [])] or [UPSTREAM_NAME]
    found = []

    for pattern in patterns:
        found.extend(pattern.findall(text))

    names = []

    for quote, raw in found:
        name = unescape(raw)

        if name in expand:
            names.extend(expand[name])
            continue

        if quote == "`":
            for key, value in substitutions.items():
                name = name.replace("${" + key + "}", value)

            if "${" in name:
                name = "UNEXPANDED: " + name

        names.append(name)

    return names


def compare(record, clone, commit):
    problems = []
    commit = commit or record["wpt_commit"]
    listed = set(record["files"])

    for wpt_path, entry in record["files"].items():
        try:
            data = git(clone, "show", f"{commit}:{wpt_path}")
        except subprocess.CalledProcessError:
            problems.append(f"{wpt_path}: not in WPT at {commit}")
            continue

        if hashlib.sha1(data).hexdigest() != entry["sha1"]:
            problems.append(f"{wpt_path}: changed since the pinned commit; review it")

        if "not_applicable" in entry:
            continue

        text = data.decode("utf-8")

        for name in upstream_subtests(text, entry):
            if name not in entry["subtests"]:
                problems.append(f"{wpt_path}: subtest not recorded: '{name}'")

    for directory in record["tracked_directories"]:
        files = git(clone, "ls-tree", "-r", "--name-only", commit, directory).decode().split()

        for path in files:
            if path not in listed and not path.endswith(".yml"):
                problems.append(f"{path}: in a tracked directory but not recorded")

    return problems


def main():
    record = load_record()

    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        problems = check(record)
    elif len(sys.argv) >= 3 and sys.argv[1] == "--upstream":
        commit = sys.argv[4] if len(sys.argv) >= 5 and sys.argv[3] == "--commit" else None
        problems = check(record) + compare(record, sys.argv[2], commit)
    else:
        raise SystemExit(__doc__)

    for problem in problems:
        print("wpt-coverage: " + problem)

    covered = sum(1 for e in record["files"].values() if "subtests" in e
                  for v in e["subtests"].values() if isinstance(v, str))
    skipped = sum(1 for e in record["files"].values() if "subtests" in e
                  for v in e["subtests"].values() if isinstance(v, dict))
    files_skipped = sum(1 for e in record["files"].values() if "not_applicable" in e)
    print(f"wpt-coverage: {covered} subtests covered, {skipped} not applicable, "
          f"{files_skipped} files not applicable, {len(problems)} problems "
          f"(WPT {record['wpt_commit'][:12]})")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
