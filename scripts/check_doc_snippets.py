#!/usr/bin/env python3
"""Check that the documentation and the Claude Code skills still describe the binary.

Ported from anofox-forecast's scripts/check_doc_snippets.py and extended. Three checks:

1. Every ```sql fence in README.md, docs/**/*.md and plugins/anofox-optimize/skills/**/*.md
   runs against the built extension in an in-memory database. scripts/doc_snippets/fixtures.sql
   runs first, and a CREATE TABLE / INSERT block earlier in the same file is replayed when a
   later block mentions what it creates, so a guide can build its tables step by step.
2. Every function name a document mentions (`anofox_optimize_*`, `opt_*` or
   `anofox_contract_version`, inside backticks or before `(`, and anywhere in a SKILL.md
   frontmatter) is registered in duckdb_functions() (or is a setting, duckdb_settings()).
3. When the plugin exists: every canonical function appears in at least one skill (by its
   canonical or its opt_ name), and plugin.json and every SKILL.md carry the
   same version as the newest released `## vYYYY.MM.DD` heading in CHANGELOG.md. A skill that
   lags the extension it describes is the drift this repo's AGENTS.md warns about.

Statuses per ```sql block: PASS, FAIL, EXPECTED-ERROR (keyed in doc_snippets/expected_errors.txt;
the block must still fail), SYNOPSIS (keyed in doc_snippets/synopsis.txt; never run),
SKIP-NETWORK (INSTALL or a URL; never run). On a full scan a support-file entry that matches
nothing is STALE. Keys are `file<TAB>first non-comment SQL line`.

Usage:
    python3 scripts/check_doc_snippets.py [FILE ...]
    python3 scripts/check_doc_snippets.py --duckdb CLI --extension PATH   # e.g. CI: official CLI
Exit codes: 0 clean; 1 on any FAIL or STALE; 2 if DuckDB or the extension cannot be loaded.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
SUPPORT_DIR = os.path.join(SCRIPT_DIR, "doc_snippets")
FIXTURES_FILE = os.path.join(SUPPORT_DIR, "fixtures.sql")
EXPECTED_ERRORS_FILE = os.path.join(SUPPORT_DIR, "expected_errors.txt")
SYNOPSIS_FILE = os.path.join(SUPPORT_DIR, "synopsis.txt")
REMOVED_NAMES_FILE = os.path.join(SUPPORT_DIR, "removed_names.txt")

PLUGIN_DIR = os.path.join("plugins", "anofox-optimize")
SKILLS_DIR = os.path.join(PLUGIN_DIR, "skills")
DEFAULT_SCAN_DIRS = ["docs", SKILLS_DIR]

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)[ \t]*$")
SETUP_RE = re.compile(r"\b(CREATE\s+(OR\s+REPLACE\s+)?(TEMP\s+|TEMPORARY\s+)?(TABLE|VIEW|MACRO)|INSERT)\b", re.I)
CREATE_NAME_RE = re.compile(
    r"\bCREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP\s+|TEMPORARY\s+)?(?:TABLE|VIEW|MACRO)\s+"
    r"(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)", re.I)
INSERT_NAME_RE = re.compile(r"\bINSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)", re.I)
NETWORK_RE = re.compile(r"\bINSTALL\b|https?://|s3://", re.I)

NAME = r"(?:anofox_optimize_[a-z0-9_]+|opt_[a-z0-9_]+|anofox_contract_version)"
IDENT_RE = re.compile(r"\b(" + NAME + r")\b")
BACKTICK_IDENT_RE = re.compile(r"`(" + NAME + r")`")
CALL_IDENT_RE = re.compile(r"\b(" + NAME + r")\s*\(")
RELEASE_RE = re.compile(r"^## v(\d{4}\.\d{1,2}\.\d{1,2})\b", re.M)
VERSION_FM_RE = re.compile(r"^version:\s*(\S+)\s*$", re.M)
HEADER_VERSION_RE = re.compile(r"^\*\*Extension:\*\* `anofox_optimize` v([0-9.]+)", re.M)


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def default_scan_files():
    files = ["README.md"] if os.path.exists(os.path.join(REPO_ROOT, "README.md")) else []
    for scan_dir in DEFAULT_SCAN_DIRS:
        for root, dirs, names in os.walk(os.path.join(REPO_ROOT, scan_dir)):
            dirs.sort()
            files += [os.path.relpath(os.path.join(root, n), REPO_ROOT) for n in sorted(names) if n.endswith(".md")]
    return files


def extract_sql_blocks(text):
    lines, blocks, i = text.splitlines(), [], 0
    while i < len(lines):
        m = FENCE_RE.match(lines[i].strip())
        if m and m.group(1).lower() == "sql":
            start, body = i, []
            i += 1
            while i < len(lines) and lines[i].strip() != "```":
                body.append(lines[i])
                i += 1
            blocks.append(("\n".join(body), start))
        i += 1
    return blocks


def first_non_comment_line(block):
    for line in block.splitlines():
        if line.strip() and not line.strip().startswith("--"):
            return line.strip()
    return next((line.strip() for line in block.splitlines() if line.strip()), "")


def load_key_set(path):
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        return {line.rstrip("\n") for line in f if line.strip() and not line.startswith("#")}


def load_removed_names(path):
    names = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    name, _, reason = line.rstrip("\n").partition("\t")
                    names[name.strip()] = reason.strip()
    return names


def strip_sql_comments(text):
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def setup_created_name(block):
    m = CREATE_NAME_RE.search(block) or INSERT_NAME_RE.search(block)
    return m.group(1) if m else None


def select_required_setup(target, setup_accum, fixture_names):
    """Earlier setup blocks (transitively) referenced by target, in document order."""
    shadowed = set(CREATE_NAME_RE.findall(strip_sql_comments(target))) | fixture_names
    needed, included, names = [strip_sql_comments(target)], [], set()
    changed = True
    while changed:
        changed = False
        combined = "\n".join(needed)
        for name, block in setup_accum:
            if name not in names and name not in shadowed and re.search(r"\b" + re.escape(name) + r"\b", combined):
                included.append(block)
                names.add(name)
                needed.append(strip_sql_comments(block))
                changed = True
    order = {id(b): i for i, (_, b) in enumerate(setup_accum)}
    return sorted(included, key=lambda b: order.get(id(b), 0))


class DuckDB:
    def __init__(self, binary, extension):
        self.binary = binary
        self.load = f"LOAD '{extension}';" if extension else "LOAD anofox_optimize;"

    def run(self, sql, timeout=120):
        tmp = tempfile.mkdtemp(prefix="doc_snippets_")
        try:
            env = dict(os.environ, HOME=tmp)
            full = "SET autoinstall_known_extensions = false;\n" + self.load + "\n" + sql
            try:
                r = subprocess.run([self.binary, "-unsigned", "-bail", "-list", "-noheader", ":memory:"], input=full,
                                   cwd=tmp, capture_output=True, text=True, timeout=timeout, env=env)
                return r.returncode, r.stdout, r.stderr
            except subprocess.TimeoutExpired:
                return 124, "", f"TIMEOUT after {timeout}s"
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def query_lines(self, sql):
        rc, out, err = self.run(sql)
        if rc != 0:
            print(f"ERROR: {(err.strip() or out.strip())[:500]}", file=sys.stderr)
            sys.exit(2)
        return [line.strip() for line in out.splitlines() if line.strip()]


def first_error_line(stderr, stdout):
    for stream in (stderr, stdout):
        for line in stream.splitlines():
            if line.strip():
                return line.strip()[:200]
    return ""


def collect_name_mentions(rel_path, text):
    """SKILL.md frontmatter: every name-shaped token. Bodies: backtick spans and call syntax."""
    mentions, lines, fm_end = [], text.splitlines(), 0
    is_skill = rel_path.endswith("SKILL.md")
    if is_skill and lines and lines[0].strip() == "---":
        fm_end = next((i for i in range(1, len(lines)) if lines[i].strip() == "---"), 0)
        mentions += [(m.group(1), i + 1) for i in range(1, fm_end) for m in IDENT_RE.finditer(lines[i])]
    for i, line in enumerate(lines):
        if is_skill and i <= fm_end:
            continue
        mentions += [(m.group(1), i + 1) for m in BACKTICK_IDENT_RE.finditer(line)]
        mentions += [(m.group(1), i + 1) for m in CALL_IDENT_RE.finditer(line)]
    return mentions


def plugin_checks(canonical):
    """Coverage and version agreement. Returns a list of (file, detail) failures."""
    failures = []
    skills_root = os.path.join(REPO_ROOT, SKILLS_DIR)
    if not os.path.isdir(skills_root):
        return failures
    skill_files = sorted(os.path.join(r, n) for r, _, ns in os.walk(skills_root) for n in ns if n == "SKILL.md")
    mentioned = set()
    for path in skill_files:
        mentioned |= set(IDENT_RE.findall(read_text(path)))
    for name in sorted(canonical):
        short = "opt_" + name[len("anofox_optimize_"):]
        if name not in mentioned and short not in mentioned:
            failures.append((SKILLS_DIR, f"no skill covers {name} (or {short})"))

    releases = RELEASE_RE.findall(read_text(os.path.join(REPO_ROOT, "CHANGELOG.md")))
    want = releases[0] if releases else None
    manifest = os.path.join(PLUGIN_DIR, ".claude-plugin", "plugin.json")
    versions = {manifest: json.loads(read_text(os.path.join(REPO_ROOT, manifest))).get("version")}
    for path in skill_files:
        m = VERSION_FM_RE.search(read_text(path).split("\n---", 1)[0])
        versions[os.path.relpath(path, REPO_ROOT)] = m.group(1) if m else None
        h = HEADER_VERSION_RE.search(read_text(path))
        versions[os.path.relpath(path, REPO_ROOT) + " (header)"] = h.group(1) if h else None
    for rel, got in versions.items():
        if got != want:
            failures.append((rel, f"version {got} != newest CHANGELOG release {want}"))
    return failures


def main():
    ap = argparse.ArgumentParser(description="Check docs and skills against the built extension")
    ap.add_argument("files", nargs="*", help="restrict the scan to these markdown files")
    ap.add_argument("--duckdb", default=os.path.join(REPO_ROOT, "build", "release", "duckdb"))
    ap.add_argument("--extension", help="LOAD this .duckdb_extension (default: the statically linked one)")
    ap.add_argument("--out", default=os.path.join(REPO_ROOT, "build", "doc-snippets.tsv"))
    args = ap.parse_args()

    if not os.path.exists(args.duckdb):
        print(f"ERROR: duckdb binary not found at {args.duckdb} -- build it first (make release)", file=sys.stderr)
        return 2
    db = DuckDB(os.path.abspath(args.duckdb), os.path.abspath(args.extension) if args.extension else None)
    full_scan = not args.files
    files = args.files or default_scan_files()

    fixtures = read_text(FIXTURES_FILE) if os.path.exists(FIXTURES_FILE) else ""
    fixture_names = set(CREATE_NAME_RE.findall(strip_sql_comments(fixtures)))
    expected_errors = load_key_set(EXPECTED_ERRORS_FILE)
    synopsis_keys = load_key_set(SYNOPSIS_FILE)
    removed_names = load_removed_names(REMOVED_NAMES_FILE)
    # Settings share the prefix (anofox_optimize_knapsack_max_memory) and are as checkable.
    registered = set(db.query_lines(
        "SELECT DISTINCT function_name FROM duckdb_functions() UNION SELECT name FROM duckdb_settings();"))
    canonical = set(db.query_lines(
        "SELECT DISTINCT function_name FROM duckdb_functions() WHERE function_name LIKE 'anofox\\_optimize\\_%' ESCAPE '\\';"))

    rows, counts = [], dict.fromkeys(["PASS", "FAIL", "EXPECTED-ERROR", "SYNOPSIS", "SKIP-NETWORK", "STALE"], 0)
    all_keys, all_mentioned, seen_unknown = set(), set(), set()

    def add(status, file, line="", first="", detail=""):
        rows.append((status, file, str(line), first, detail))
        counts[status] += 1

    for rel in files:
        path = os.path.join(REPO_ROOT, rel)
        if not os.path.exists(path):
            continue
        text = read_text(path)
        for name, line_no in collect_name_mentions(rel, text):
            all_mentioned.add(name)
            if name not in registered and name not in removed_names and (rel, name) not in seen_unknown:
                seen_unknown.add((rel, name))
                add("FAIL", rel, line_no, "", f"unknown function name {name}")

        setup_accum = []
        for block, start in extract_sql_blocks(text):
            first = first_non_comment_line(block)
            key = f"{rel}\t{first}"
            all_keys.add(key)
            if NETWORK_RE.search(block):
                add("SKIP-NETWORK", rel, start + 1, first[:100], "INSTALL/URL, not executed")
            elif key in synopsis_keys:
                add("SYNOPSIS", rel, start + 1, first[:100], "signature, not executed")
            else:
                # A guide that builds its own `cartons` replaces the fixture of that name.
                own = set(CREATE_NAME_RE.findall(strip_sql_comments(block))) & fixture_names
                drops = [f"DROP TABLE IF EXISTS {n};" for n in sorted(own)]
                sql = "\n".join([fixtures] + drops + select_required_setup(block, setup_accum, fixture_names) + [block])
                rc, out, err = db.run(sql)
                if rc == 0:
                    add("FAIL" if key in expected_errors else "PASS", rel, start + 1, first[:100],
                        "expected an error but passed" if key in expected_errors else "")
                elif key in expected_errors:
                    add("EXPECTED-ERROR", rel, start + 1, first[:100])
                else:
                    add("FAIL", rel, start + 1, first[:100], first_error_line(err, out))
            if SETUP_RE.search(block):
                name = setup_created_name(block)
                if name:
                    setup_accum.append((name, block))

    if full_scan:
        for key in sorted(expected_errors - all_keys):
            add("STALE", key.split("\t", 1)[0], "", "", f"stale expected_errors.txt entry: {key}")
        for key in sorted(synopsis_keys - all_keys):
            add("STALE", key.split("\t", 1)[0], "", "", f"stale synopsis.txt entry: {key}")
        for name in sorted(removed_names):
            if name in registered:
                add("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is registered again")
            elif name not in all_mentioned:
                add("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is no longer mentioned")
        for rel, detail in plugin_checks(canonical):
            add("FAIL", rel, "", "", detail)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.writelines("\t".join(r) + "\n" for r in rows)
    for r in rows:
        if r[0] in ("FAIL", "STALE"):
            print("  " + "  ".join(x for x in r if x))
    print("check_doc_snippets: " + ", ".join(f"{v} {k}" for k, v in counts.items()))
    return 1 if counts["FAIL"] or counts["STALE"] else 0


if __name__ == "__main__":
    sys.exit(main())
