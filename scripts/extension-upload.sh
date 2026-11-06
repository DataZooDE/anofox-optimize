#!/bin/bash

# Extension upload script

# Usage: ./extension-upload.sh <name> <extension_version> <duckdb_version> <architecture> <s3_bucket> <copy_to_latest> <copy_to_versioned>
# <name>                : Name of the extension
# <extension_version>   : Version (commit / version tag) of the extension
# <duckdb_version>      : Version (commit / version tag) of DuckDB
# <architecture>        : Architecture target of the extension binary
# <s3_bucket>           : S3 bucket to upload to
# <copy_to_latest>      : Set this as the latest version ("true" / "false", default: "false")
# <copy_to_versioned>   : Set this as a versioned version that will prevent its deletion
#
# Local dry run (no upload happens without AWS_ACCESS_KEY_ID; nothing is signed
# without DUCKDB_EXTENSION_SIGNING_PK):
#   EXTENSION_UPLOAD_DIR=<scratch dir holding the artifact> \
#     ./scripts/extension-upload.sh anofox_optimize <ver> v1.5.6 wasm_eh none false false

set -euo pipefail

# Directory holding the downloaded build artifact. Defaults to the CI path
# (/tmp/extension); overridable for local dry runs against a scratch copy.
EXTENSION_UPLOAD_DIR="${EXTENSION_UPLOAD_DIR:-/tmp/extension}"

if [[ $4 == wasm* ]]; then
  ext="$EXTENSION_UPLOAD_DIR/$1.duckdb_extension.wasm"
else
  ext="$EXTENSION_UPLOAD_DIR/$1.duckdb_extension"
fi

echo "$ext"

script_dir="$(dirname "$(readlink -f "$0")")"

# Artifact shape (#9): native AND wasm build artifacts already end with a
# complete `duckdb_signature` section -- metadata plus a 256-byte signature
# placeholder, zero-filled when unsigned. For both we strip that 256-byte
# placeholder before hashing and re-append a fresh signature in its place; we
# never append a new section header. For native binaries the placeholder is
# the file's last 256 bytes. For wasm it is the last 256 bytes of the trailing
# `duckdb_signature` custom section, whose own LEB128 size already counts
# them, so truncating and re-appending exactly 256 bytes keeps the module
# well-formed. A wasm artifact that does not already end with exactly one
# section aborts here rather than being patched into an unverified layout.
if [[ $4 == wasm* ]]; then
  python3 "$script_dir/wasm_signature_sections.py" check "$ext"
fi

cat "$ext" > "$ext.append"
truncate -s -256 "$ext.append"

# (Optionally) Sign binary
if [ -n "${DUCKDB_EXTENSION_SIGNING_PK:-}" ]; then
  echo "$DUCKDB_EXTENSION_SIGNING_PK" > private.pem
  "$script_dir/../duckdb/scripts/compute-extension-hash.sh" "$ext.append" > "$ext.hash"
  openssl pkeyutl -sign -in "$ext.hash" -inkey private.pem -pkeyopt digest:sha256 -out "$ext.sign"
  rm -f private.pem
fi

# Signature is always there, potentially defaulting to 256 zeros
truncate -s 256 "$ext.sign"

# append signature to extension binary
cat "$ext.sign" >> "$ext.append"

# compress extension binary
if [[ $4 == wasm_* ]]; then
  brotli < "$ext.append" > "$ext.compressed"
else
  gzip < "$ext.append" > "$ext.compressed"
fi

# Pre-upload gate: the exact bytes about to be published must LOAD in
# DuckDB-Wasm and pass test/wasm/post_upload_smoke.test (on the engine/arch
# pairs the harness can boot; elsewhere the section count only). An artifact
# that fails never reaches the versioned or the latest path, where the
# post-upload check could only report it after the fact. It also re-checks
# the duckdb_signature section of the processed bytes. Needs
# test/wasm/node_modules (the deploy workflow installs it for wasm archs).
if [[ $4 == wasm* ]]; then
  "$script_dir/verify-wasm-artifact.sh" "$ext.compressed" "$4" "$3"
fi

# Upload manifest: truncated fresh for this invocation, then one s3:// URI per
# successful upload below. scripts/extension-verify-upload.sh reads it to know
# exactly which objects this run published.
: > "$ext.uploaded"

# Abort if AWS key is not set
if [ -z "${AWS_ACCESS_KEY_ID:-}" ]; then
    echo "No AWS key found, skipping.."
    exit 0
fi

# upload versioned version
if [[ $7 = 'true' ]]; then
  if [[ $4 == wasm* ]]; then
    dest="s3://$5/$1/$2/$3/$4/$1.duckdb_extension.wasm"
    aws s3 cp "$ext.compressed" "$dest" --acl public-read --content-encoding br --content-type="application/wasm"
  else
    dest="s3://$5/$1/$2/$3/$4/$1.duckdb_extension.gz"
    aws s3 cp "$ext.compressed" "$dest" --acl public-read
  fi
  echo "$dest" >> "$ext.uploaded"
fi

# upload to latest version
if [[ $6 = 'true' ]]; then
  if [[ $4 == wasm* ]]; then
    dest="s3://$5/$3/$4/$1.duckdb_extension.wasm"
    aws s3 cp "$ext.compressed" "$dest" --acl public-read --content-encoding br --content-type="application/wasm"
  else
    dest="s3://$5/$3/$4/$1.duckdb_extension.gz"
    aws s3 cp "$ext.compressed" "$dest" --acl public-read
  fi
  echo "$dest" >> "$ext.uploaded"
fi
