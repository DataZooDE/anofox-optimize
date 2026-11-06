#!/usr/bin/env bash
# scripts/verify-wasm-artifact.sh FILE ARCH DUCKDB_VERSION
#
# Post-upload WASM verification (#9; ported from anofox-forecast #272). Given
# one WASM artifact (a raw .wasm module or a brotli-compressed .wasm.br
# download), this decompresses it if needed, asserts it carries exactly one
# duckdb_signature custom section (scripts/wasm_signature_sections.py check),
# and -- only for the engine/arch combinations the DuckDB-Wasm harness can
# actually LOAD -- runs the Node harness against it with
# test/wasm/post_upload_smoke.test. For every other engine/arch combination it
# records that only the section count was verified and exits 0; it never
# silently skips a failure.
#
# The harness bundle is the arch without its wasm_ prefix (wasm_eh -> eh,
# wasm_mvp -> mvp); run.mjs rejects a bundle it does not know. wasm_threads
# has no Node bundle in @duckdb/duckdb-wasm, so section count only.
#
# This script runs inside the deploy job, which holds AWS OIDC credentials --
# never enable shell command-echo tracing here, and never print environment
# variables.
#
# Usage: ./verify-wasm-artifact.sh FILE ARCH DUCKDB_VERSION
set -euo pipefail

if [ $# -ne 3 ]; then
  echo "usage: verify-wasm-artifact.sh FILE ARCH DUCKDB_VERSION" >&2
  exit 1
fi

FILE="$1"
ARCH="$2"
DUCKDB_VERSION="$3"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Engine bundled by the @duckdb/duckdb-wasm version pinned in
# test/wasm/package.json -- must equal the engine bundled by that pin (see
# test/wasm/README.md DEP-01: Version-verification procedure); bump together.
WASM_HARNESS_ENGINE=v1.5.6

if [ ! -f "$FILE" ]; then
  echo "error: file not found: $FILE" >&2
  exit 1
fi

TMPDIR_LOCAL="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_LOCAL"; }
trap cleanup EXIT

MODULE="$TMPDIR_LOCAL/module.wasm"

# A raw WASM module starts with the 4-byte magic \0asm. A brotli-compressed
# download (content-encoding: br) does not -- decompress it first.
if python3 "$REPO_ROOT/scripts/wasm_signature_sections.py" is-module "$FILE"; then
  cp "$FILE" "$MODULE"
else
  if ! brotli -d -c "$FILE" > "$MODULE" 2>"$TMPDIR_LOCAL/brotli.err"; then
    echo "error: $FILE is neither a raw WASM module nor brotli-decompressible:" >&2
    cat "$TMPDIR_LOCAL/brotli.err" >&2
    exit 1
  fi
fi

if ! python3 "$REPO_ROOT/scripts/wasm_signature_sections.py" check "$MODULE"; then
  echo "error: $FILE failed the duckdb_signature section check" >&2
  exit 1
fi

if [ "$ARCH" != wasm_threads ] && [ "$DUCKDB_VERSION" = "$WASM_HARNESS_ENGINE" ]; then
  if [ ! -d "$REPO_ROOT/test/wasm/node_modules" ]; then
    echo "error: $REPO_ROOT/test/wasm/node_modules is missing -- run: npm --prefix test/wasm ci --ignore-scripts" >&2
    exit 1
  fi
  if ! env -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY -u AWS_SESSION_TOKEN \
      node "$REPO_ROOT/test/wasm/run.mjs" --bundle "${ARCH#wasm_}" --ext "$MODULE" --file test/wasm/post_upload_smoke.test; then
    echo "error: $FILE failed to LOAD / pass test/wasm/post_upload_smoke.test in the DuckDB-Wasm Node harness" >&2
    exit 1
  fi
else
  MSG="LOAD check skipped for $ARCH $DUCKDB_VERSION: no @duckdb/duckdb-wasm build bundles this engine/arch (harness engine $WASM_HARNESS_ENGINE; wasm_threads has no Node bundle); duckdb_signature section count verified only"
  echo "$MSG"
  if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    echo "::notice::$MSG"
  fi
fi
