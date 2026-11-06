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

`test/wasm/post_upload_smoke.test` is the small case the deploy job runs
against the bytes it just uploaded (`scripts/verify-wasm-artifact.sh`): the
Falkenauer triplet instance with its known spread (first-fit-decreasing 12,
bin completion 11, optimum 10), a feasibility check, and an `opt_pack_by` row
count.

## wasm_mvp

A wasm_mvp build (emsdk 3.1.71) LOADs under `--bundle mvp` and passes every
query of the suite, and `post_upload_smoke.test` passes, so the deploy gates
wasm_mvp on it as forecast does. It is not clean: every error the extension
raises (all 33 `statement error` records, 2026-11-06) reaches the caller as
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

A sqllogictest subset, not the real runner: `require` (ignored),
`statement ok`, `statement error` with the expected message after `----`
(substring, or `<REGEX>:` matched against the whole message), `query <types>
[sort]`, `mode skip`/`unskip`, `halt`. Numbers compare within 1e-6 absolute
or 1e-4 relative; booleans compare as `1`/`0`; results are formatted through
DuckDB's own `::VARCHAR`. The forecast version folded `----` and the message
into the SQL of a `statement error` record, so each such record "passed" on a
syntax error without running the statement; this port splits them.

## Sources

The Node recipe follows
[query-farm/haybarn-extension-wasm-tester](https://github.com/query-farm/haybarn-extension-wasm-tester),
by way of the anofox-statistics and anofox-forecast harnesses.
