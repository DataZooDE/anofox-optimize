#!/usr/bin/env bash
# scripts/extension-verify-upload.sh NAME ARCH
#
# Post-upload check (#9), run in the deploy job (_extension_deploy.yml)
# directly after scripts/extension-upload.sh. For a WASM arch, downloads every
# object that script just uploaded (per its manifest,
# $EXT_DIR/$NAME.duckdb_extension.wasm.uploaded) and compares its sha256 with
# the local .compressed file: byte identity proves the published object IS the
# artifact the pre-upload gate (scripts/verify-wasm-artifact.sh) loaded and
# tested, so there is nothing to re-run. An empty or missing manifest (branch
# or PR run, or no AWS key) means nothing was uploaded.
#
# A non-wasm ARCH has nothing to verify here and exits 0 immediately.
#
# This script runs inside the deploy job, which holds AWS OIDC credentials
# -- never enable shell command-echo tracing here, and
# never print environment variables.
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: extension-verify-upload.sh NAME ARCH" >&2
  exit 1
fi

NAME="$1"
ARCH="$2"

if [[ "$ARCH" != wasm* ]]; then
  echo "nothing to verify for non-wasm arch $ARCH"
  exit 0
fi

EXTENSION_UPLOAD_DIR="${EXTENSION_UPLOAD_DIR:-/tmp/extension}"
MANIFEST="$EXTENSION_UPLOAD_DIR/$NAME.duckdb_extension.wasm.uploaded"
COMPRESSED="$EXTENSION_UPLOAD_DIR/$NAME.duckdb_extension.wasm.compressed"

if [ ! -s "$MANIFEST" ]; then
  echo "nothing uploaded; the pre-upload gate verified these bytes"
  exit 0
fi

TMPDIR_LOCAL="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR_LOCAL"; }
trap cleanup EXIT

expected="$(sha256sum "$COMPRESSED" | cut -d' ' -f1)"
while IFS= read -r uri; do
  [ -z "$uri" ] && continue
  dl="$TMPDIR_LOCAL/downloaded"
  aws s3 cp "$uri" "$dl" --only-show-errors
  actual="$(sha256sum "$dl" | cut -d' ' -f1)"
  if [ "$actual" != "$expected" ]; then
    echo "error: $uri (sha256 $actual) is not the verified artifact $COMPRESSED (sha256 $expected)" >&2
    exit 1
  fi
  echo "verified $uri: identical to the gated artifact (sha256 $expected)"
done < "$MANIFEST"
