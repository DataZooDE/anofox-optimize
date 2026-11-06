#!/usr/bin/env python3
"""scripts/wasm_signature_sections.py

Read-only check of the `duckdb_signature` custom section that DuckDB's
extension loader requires at the end of every WASM extension (#9; ported from
anofox-forecast #272). A module is an 8-byte header (`\\0asm` + version) and
then sections `id:u8 size:LEB128 content`; a custom section (id 0) starts its
content with `name_len:LEB128 name`. scripts/extension-upload.sh replaces the
last 256 bytes of that section's payload with a fresh signature, so the
module must carry exactly one such section, ending at EOF, with at least 256
payload bytes.

CLI:
    wasm_signature_sections.py check FILE [FILE...]   exit 0 iff every file passes
    wasm_signature_sections.py is-module FILE         exit 0 iff FILE starts with `\\0asm`
    wasm_signature_sections.py --self-test            verdicts on synthetic modules
Exit codes: 0 ok, 1 a check / self-test failed, 2 bad usage.
"""
import sys

MAGIC = b"\0asm"
SIGNATURE_NAME = b"duckdb_signature"
MIN_PAYLOAD_LEN = 256


class WasmParseError(Exception):
    """A module too malformed or truncated to answer the question."""


def leb(b, i):
    """Decode one unsigned LEB128 value from `b` at offset `i`: (value, next offset)."""
    r = s = 0
    while True:
        if i >= len(b):
            raise WasmParseError(f"LEB128 integer read past end of file at offset {i}")
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return r, i


def leb_encode(value):
    """Unsigned LEB128 bytes of `value` (for --self-test)."""
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def signature_sections(b):
    """(payload_start, end) of every `duckdb_signature` custom section in module `b`."""
    n = len(b)
    if n < 8:
        raise WasmParseError(f"file too short to contain a WASM header ({n} bytes)")
    if b[:4] != MAGIC:
        raise WasmParseError("missing WASM magic bytes (expected b'\\0asm')")
    found = []
    i = 8
    while i < n:
        start, sid = i, b[i]
        size, i = leb(b, i + 1)
        end = i + size
        if end > n:
            raise WasmParseError(
                f"section id={sid} at offset {start} declares size {size} "
                f"but that runs past EOF (would end at {end}, file is {n} bytes)"
            )
        if sid == 0:
            name_len, j = leb(b, i)
            if j + name_len > end:
                raise WasmParseError(
                    f"custom section at offset {start} declares name length {name_len} "
                    f"that runs past its own section end ({end})"
                )
            if b[j : j + name_len] == SIGNATURE_NAME:
                found.append((j + name_len, end))
        i = end
    return found


def check_bytes(b):
    """(ok, message) for an in-memory module; never raises on malformed input."""
    try:
        sigs = signature_sections(b)
    except WasmParseError as e:
        return False, str(e)
    if len(sigs) != 1:
        return False, f"found {len(sigs)} duckdb_signature sections, expected exactly 1"
    payload_start, end = sigs[0]
    if end != len(b):
        return False, (
            f"duckdb_signature section ends at offset {end} but the file is "
            f"{len(b)} bytes -- it is not the last section / does not end exactly at EOF"
        )
    # `>=`, not `==`: the section may carry metadata ahead of the 256-byte
    # signature, and the upload script only ever replaces the trailing 256.
    payload_len = end - payload_start
    if payload_len < MIN_PAYLOAD_LEN:
        return False, f"duckdb_signature payload is {payload_len} bytes, expected at least {MIN_PAYLOAD_LEN}"
    return True, "ok"


def check_file(path):
    try:
        with open(path, "rb") as f:
            b = f.read()
    except OSError as e:
        return False, f"cannot read file: {e}"
    return check_bytes(b)


def cmd_check(args):
    if not args:
        print("usage: wasm_signature_sections.py check FILE [FILE...]", file=sys.stderr)
        return 2
    failed = False
    for path in args:
        ok, msg = check_file(path)
        if ok:
            print(f"{path}: 1 duckdb_signature section (ok)")
        else:
            print(f"{path}: error: {msg}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


def cmd_is_module(args):
    if len(args) != 1:
        print("usage: wasm_signature_sections.py is-module FILE", file=sys.stderr)
        return 2
    with open(args[0], "rb") as f:
        return 0 if f.read(4) == MAGIC else 1


def _custom_section(name, payload):
    content = leb_encode(len(name)) + name + payload
    return bytes([0]) + leb_encode(len(content)) + content


def self_test():
    failures = []

    def expect(name, module, want_ok, want_in_msg=""):
        try:
            ok, msg = check_bytes(module)
        except Exception as e:  # noqa: BLE001 - raising IS a failure under test
            failures.append(f"{name}: raised {type(e).__name__}: {e}")
            return
        if ok != want_ok or want_in_msg not in msg:
            failures.append(f"{name}: expected ok={want_ok} (message with {want_in_msg!r}), got ok={ok}: {msg}")

    header = MAGIC + bytes([1, 0, 0, 0])
    types = bytes([1, 1, 0])  # a non-custom section: an empty type vector
    sig = _custom_section(SIGNATURE_NAME, bytes(256))

    cases = [
        ("single trailing signature section", header + types + sig, True, ""),
        ("two signature sections", header + types + sig + sig, False, "found 2"),
        ("no signature section", header + types, False, "found 0"),
        ("signature then another section", header + types + sig + _custom_section(b"other", b"x"), False, "EOF"),
        ("section size past EOF", header + bytes([0]) + leb_encode(1000) + b"short", False, "past EOF"),
        # Pins the `>=`: a metadata prefix ahead of the 256-byte signature passes.
        ("metadata-prefixed payload", header + types + _custom_section(SIGNATURE_NAME, b"META" + bytes(256)), True, ""),
        ("255-byte payload", header + types + _custom_section(SIGNATURE_NAME, bytes(255)), False, "at least 256"),
    ]
    for case in cases:
        expect(*case)

    if failures:
        print("SELF-TEST FAILURES:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"self-test: {len(cases)} synthetic module verdicts passed")
    return 0


def main(argv):
    commands = {"check": cmd_check, "is-module": cmd_is_module}
    if argv and argv[0] == "--self-test":
        return self_test()
    if not argv or argv[0] not in commands:
        print("usage: wasm_signature_sections.py {check FILE [FILE...] | is-module FILE | --self-test}", file=sys.stderr)
        return 2
    return commands[argv[0]](argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
