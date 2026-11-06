# anofox_optimize — DuckDB-Wasm Node harness

Proves the built `.wasm` extension **loads and runs correctly in DuckDB-Wasm
under Node.js**, which a compile+link CI leg cannot. Ported from
anofox-forecast's `test/wasm` (#9).

Why it exists: until #9 every published anofox_optimize wasm artifact failed
`LOAD` in DuckDB-Wasm, for two independent reasons, and CI was green throughout:

1. **Unresolved OpenSSL imports.** The PostHog telemetry client was compiled
   into the Emscripten build. DuckDB-Wasm ships no OpenSSL, so `SSL_CTX_new`,
   `TLS_client_method` and the rest stayed as imports of the side module and
   `LOAD` failed with `TypeError: t is not a function`. Telemetry is now off
   under Emscripten (`CMakeLists.txt`) and vcpkg skips `openssl` for `wasm32`.
2. **Two `duckdb_signature` sections.** `scripts/extension-upload.sh` appended a
   second signature section to an artifact that already ended with one, and
   DuckDB-Wasm read the wrong bytes as metadata:
   `Unknown ABI type of value '208'`. The script now replaces the 256-byte
   placeholder in place and checks the section count before and after
   (`scripts/wasm_signature_sections.py`).

## Requirements

- Node.js >= 18
- A wasm_eh build, e.g. from `make wasm_eh` under emsdk **3.1.71** (the version
  extension-ci-tools v1.5-variegata pins; @duckdb/duckdb-wasm 1.33.1-dev65.0 is
  built with it). Without a local emsdk:

  ```bash
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$PWD:$PWD" -w "$PWD" emscripten/emsdk:3.1.71 make wasm_eh
  ```

  No vcpkg is needed for the wasm build: OpenSSL is its only vcpkg dependency.

## Running

```bash
npm --prefix test/wasm ci --ignore-scripts     # pinned dependencies, once
make test_wasm                                 # whole suite vs build/wasm_eh
node test/wasm/run.mjs --ext path/to/anofox_optimize.duckdb_extension.wasm
node test/wasm/run.mjs --ext <wasm> --file test/sql/packing.test
node test/wasm/run.mjs --ext <wasm> --bundle mvp   # against a wasm_mvp build
```

| Option | Meaning |
|---|---|
| (default) | every `test/sql/**/*.test`, derived from the directory |
| `--file <f.test>` | one file, relative to the repo root (repeatable); a missing file fails |
| `--ext <path>` | the artifact (also `ANOFOX_WASM_EXT`); otherwise searched under `build/` |
| `--bundle eh\|mvp` | DuckDB-Wasm bundle to boot (also `ANOFOX_WASM_BUNDLE`), default `eh` |
| `--all` | accepted for parity with the sibling harnesses; same as the default |

Exit code 0 on success, 1 on any assertion failure or harness error. The
DuckDB-Wasm worker prints a stack trace for every error it raises, including
the ones a `statement error` record expects; those lines are noise, the
per-file `✓`/`✗` lines and the totals are the result.

`SKIP_FILES` in `run.mjs` is empty: every test is pure in-memory SQL with no
second extension. Add a file there only if it is structurally infeasible in
DuckDB-Wasm (filesystem, network, threads), never because it fails.

`test/wasm/post_upload_smoke.test` is the small case the deploy job runs on
the exact bytes it is about to upload, and again on what it uploaded
(`scripts/verify-wasm-artifact.sh`): the
Falkenauer triplet instance with its known spread (first-fit-decreasing 12,
bin completion 11, optimum 10), a feasibility check, and an `opt_pack_by` row
count.

## wasm_mvp

A wasm_mvp build (emsdk 3.1.71) LOADs under `--bundle mvp` and passes every
query of the suite, and `post_upload_smoke.test` passes, so the deploy gates
wasm_mvp on it as forecast does. It is not clean: every error the extension
raises (every failure in the suite under `--bundle mvp`, 2026-11-06) reaches the caller as
`ReferenceError: _setThrew is not defined` instead of its message. The
statement still fails and the session keeps working, but a wasm_mvp user does
not see why their input was rejected. The mvp bundle's JS glue does not provide
the `setThrew` that Emscripten's emulated C++ exceptions need; wasm_eh, which
browsers with wasm exception support get, is not affected. CI runs the full
suite on wasm_eh only.

## Version alignment

The `@duckdb/duckdb-wasm` npm version is not the DuckDB engine version it
bundles, and DuckDB-Wasm rejects an extension built for another engine.

| `@duckdb/duckdb-wasm` | Bundled engine |
|---|---|
| 1.33.1-dev65.0 | v1.5.6 |

No duckdb-wasm build bundles v2.0.0 yet, so the harness and the CI job test the
v1.5.6 wasm_eh artifact. When one ships, bump `package.json` (regenerate the
lock file), `WASM_HARNESS_ENGINE` in `scripts/verify-wasm-artifact.sh`, and the
`duckdb-latest-wasm-runtime-test` job's `needs` and artifact name together.

## Parser notes (`sqllogic.mjs`)

A sqllogictest subset, but with the semantics of DuckDB's own runner, so a
pass here means what a native pass means.

- **Records:** `require` (handled by the harness), `statement ok`,
  `statement error` with the expected message after `----` (a substring, or
  `<REGEX>:` matched against the whole message), `query <types>
  [nosort|rowsort|valuesort] [label]`, `mode skip`/`unskip`, `halt`. Any other
  keyword (`loop`, `foreach`, `onlyif`, `skipif`, ...) **fails** with its file
  and line, rather than being skipped and leaving its body unguarded.
- **Shape first:** the result's column count must equal the length of the type
  string, and the row count must match. Expected rows are either one per line
  (tab-separated) or one value per line, as in DuckDB. `N values hashing to
  <md5>` is supported.
- **Values:** DuckDB renders them (`COLUMNS(*)::VARCHAR`; `NULL`, `(empty)` for
  the empty string), and `DESCRIBE` supplies each column's real type. Identical
  text is equal. Otherwise integers compare exactly as BigInt, BOOLEAN as
  true/false/1/0, DOUBLE/FLOAT/DECIMAL within 1e-9 relative (DuckDB itself
  allows 1%), and everything else, text included, only as identical text. The
  run prints how many queries were compared without types (DESCRIBE failed) or
  ran unwrapped; both are 0 for the current suite.
- **Raw retry:** a query is run without the `::VARCHAR` wrap only when the wrap
  raises a Binder or Parser error, so a runtime error is reported once and
  never hidden by a second run.
- **Timeout:** every statement and query has 60 s
  (`ANOFOX_WASM_RECORD_TIMEOUT_MS` overrides it). A timeout names the file and
  line and aborts the run, since the hung query still occupies the worker.

`node test/wasm/sqllogic.mjs --self-test` (also in `make audit` and the CI job)
checks the comparator without booting DuckDB: 100000 vs 100001, BIGINT beyond
2^53, text `001` vs `1` and a one-column result under `query II` must all fail.

The forecast harness this came from compared any two numbers within 1e-4
relative (so 100000 matched 100001) and text that parsed as numbers the same
way, flattened results before checking their shape, and folded a
`statement error` record's `----` and message into its SQL, so the record
"passed" on a syntax error without running the statement.

## Sources

The Node recipe follows
[query-farm/haybarn-extension-wasm-tester](https://github.com/query-farm/haybarn-extension-wasm-tester),
by way of the anofox-statistics and anofox-forecast harnesses.
