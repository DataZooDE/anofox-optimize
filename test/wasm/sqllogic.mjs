// sqllogictest-subset parser + runner for the DuckDB-Wasm harness (run.mjs).
//
// Not a full sqllogictest implementation, but what it does support follows
// DuckDB's runner (test/sqlite/sqllogic_test_runner.cpp, result_helper.cpp), so
// "passed in DuckDB-Wasm" means what "passed natively" means:
//   - `require <ext>`, `require-env`, `load`, `restart` -> directives the harness
//     handles itself
//   - `statement ok`
//   - `statement error [msg]`, the expected message optionally after `----`:
//     a substring of the error, or `<REGEX>:pattern` matching the whole message
//   - `query <types> [nosort|rowsort|valuesort] [label]`, results after `----`,
//     one row per line with tab-separated columns or one value per line, or
//     `N values hashing to <md5>`
//   - `mode skip` / `mode unskip`, `halt`, `#` comments
// Any other record keyword (loop, foreach, onlyif, skipif, ...) is a FAILURE
// naming the file and line: silently skipping it would run the records inside
// it unguarded, or not at all.
//
// Comparison (DuckDB semantics, stricter for floating point):
//   - values are rendered by DuckDB itself (COLUMNS(*)::VARCHAR), NULL as
//     `NULL` and the empty string as `(empty)`;
//   - the column count must equal the length of the type string, and the
//     number of rows must match, before any value is compared;
//   - identical text is equal; otherwise the expected value is interpreted in
//     the column's actual SQL type (from DESCRIBE): integers exactly (BigInt, so
//     BIGINT keeps its precision), BOOLEAN as true/false/1/0, DOUBLE/FLOAT/
//     DECIMAL within FLOAT_REL_TOL relative, everything else (VARCHAR, LIST,
//     STRUCT, ...) as exact text. DuckDB allows 1% on doubles; this harness
//     allows 1e-9, enough for Emscripten's libm and nothing more.
//
// `node sqllogic.mjs --self-test` checks the comparator against known-good and
// known-bad shapes without booting DuckDB.

import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

export const FLOAT_REL_TOL = 1e-9;
export const FLOAT_ABS_TOL = 1e-12; // only for values that are ~0
export const RECORD_TIMEOUT_MS = 60_000;

// ---- Parsing -------------------------------------------------------------

const DIRECTIVES = new Set(['require', 'require-env', 'load', 'restart']);
const SORT_MODES = new Set(['nosort', 'rowsort', 'valuesort']);

export function parseTest(text) {
  const lines = text.split(/\r?\n/);
  const records = [];
  let i = 0;
  let skipMode = false;

  const isBlank = (l) => l.trim() === '';
  const isComment = (l) => l.trimStart().startsWith('#');

  while (i < lines.length) {
    const line = lines[i];
    if (isBlank(line) || isComment(line)) { i++; continue; }

    const lineNo = i + 1;
    const tokens = line.trim().split(/\s+/);
    const kw = tokens[0];

    if (kw === 'mode') {
      if (tokens[1] === 'skip') skipMode = true;
      else if (tokens[1] === 'unskip') skipMode = false;
      else records.push({ type: 'unknown', kw: line.trim(), line: lineNo, skip: false });
      i++;
      continue;
    }

    if (DIRECTIVES.has(kw)) {
      records.push({ type: 'directive', kw, args: tokens.slice(1), line: lineNo, skip: skipMode });
      i++;
      continue;
    }

    if (kw === 'halt') {
      records.push({ type: 'halt', line: lineNo, skip: skipMode });
      break;
    }

    if (kw === 'statement') {
      if (tokens[1] !== 'ok' && tokens[1] !== 'error') {
        records.push({ type: 'unknown', kw: line.trim(), line: lineNo, skip: false });
      }
      const expectOk = tokens[1] === 'ok';
      let errorSubstr = tokens[1] === 'error' ? tokens.slice(2).join(' ').trim() || null : null;
      i++;
      const sql = [];
      while (i < lines.length && !isBlank(lines[i]) && lines[i].trim() !== '----') { sql.push(lines[i]); i++; }
      // The expected message after `----` (the form every anofox-optimize test
      // uses). Folding it into the SQL would make the record fail on a syntax
      // error and "pass" without running the real statement.
      if (i < lines.length && lines[i].trim() === '----') {
        i++;
        const msg = [];
        while (i < lines.length && !isBlank(lines[i])) { msg.push(lines[i]); i++; }
        if (msg.length) errorSubstr = msg.join('\n').trim();
      }
      if (tokens[1] === 'ok' || tokens[1] === 'error') {
        records.push({ type: 'statement', expectOk, errorSubstr, sql: sql.join('\n'), line: lineNo, skip: skipMode });
      }
      continue;
    }

    if (kw === 'query') {
      const types = tokens[1] || '';
      let sortMode = 'nosort';
      if (tokens[2] !== undefined) {
        if (SORT_MODES.has(tokens[2])) sortMode = tokens[2];
        else if (tokens.length > 3) {
          // A third token that is not a sort mode, followed by more: not a form we know.
          records.push({ type: 'unknown', kw: line.trim(), line: lineNo, skip: false });
        }
        // `query I label` (label without sort mode) is accepted, label ignored.
      }
      if (!/^[ITR]+$/.test(types)) {
        records.push({ type: 'unknown', kw: line.trim(), line: lineNo, skip: false });
      }
      i++;
      const sql = [];
      while (i < lines.length && !isBlank(lines[i]) && lines[i].trim() !== '----') { sql.push(lines[i]); i++; }
      let expected = null;
      if (i < lines.length && lines[i].trim() === '----') {
        i++;
        expected = [];
        while (i < lines.length && !isBlank(lines[i])) { expected.push(lines[i]); i++; }
      }
      records.push({ type: 'query', types, sortMode, sql: sql.join('\n'), expected, line: lineNo, skip: skipMode });
      continue;
    }

    // Unknown record keyword: fail loudly, then skip to the end of the record.
    records.push({ type: 'unknown', kw: line.trim(), line: lineNo, skip: false });
    i++;
    while (i < lines.length && !isBlank(lines[i])) i++;
  }

  return records;
}

// ---- Value comparison ----------------------------------------------------

const INTEGER_TYPES = /^(TINYINT|SMALLINT|INTEGER|BIGINT|HUGEINT|UTINYINT|USMALLINT|UINTEGER|UBIGINT|UHUGEINT)$/;
const FLOAT_TYPES = /^(FLOAT|DOUBLE|REAL|DECIMAL(\(\d+,\s*\d+\))?)$/;
const INT_RE = /^[+-]?\d+$/;
const NUM_RE = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;

// Render one cell the way DuckDB's sqllogictest runner does.
export function renderCell(cell) {
  if (cell === null || cell === undefined) return 'NULL';
  const s = typeof cell === 'bigint' ? cell.toString() : String(cell);
  return s === '' ? '(empty)' : s;
}

function boolOf(s) {
  const l = s.toLowerCase();
  if (l === 'true' || l === '1') return true;
  if (l === 'false' || l === '0') return false;
  return null;
}

function floatsEqual(a, b) {
  const x = Number(a), y = Number(b);
  if (!Number.isFinite(x) || !Number.isFinite(y)) return false;
  const diff = Math.abs(x - y);
  return diff <= FLOAT_ABS_TOL || diff <= FLOAT_REL_TOL * Math.max(Math.abs(x), Math.abs(y));
}

// `sqlType` is the column's DuckDB type from DESCRIBE, or null when unknown
// (then the sqllogictest type letter decides: I integer, R float, T text).
export function valuesEqual(sqlType, letter, expected, actual) {
  if (expected === actual) return true;
  if (expected === 'NULL' || actual === 'NULL') return false;

  const t = sqlType ? sqlType.toUpperCase() : null;
  if (t === 'BOOLEAN') {
    const e = boolOf(expected), a = boolOf(actual);
    return e !== null && e === a;
  }
  if (t ? INTEGER_TYPES.test(t) : letter === 'I') {
    if (INT_RE.test(expected) && INT_RE.test(actual)) return BigInt(expected) === BigInt(actual);
    if (!t) {
      // Unknown type under `query I`: booleans and floats are legal here too.
      const e = boolOf(expected), a = boolOf(actual);
      if (e !== null && a !== null) return e === a;
      if (NUM_RE.test(expected) && NUM_RE.test(actual) && !(INT_RE.test(expected) && INT_RE.test(actual))) {
        return floatsEqual(expected, actual);
      }
    }
    return false;
  }
  if (t ? FLOAT_TYPES.test(t) : letter === 'R') {
    return NUM_RE.test(expected) && NUM_RE.test(actual) && floatsEqual(expected, actual);
  }
  return false; // text, nested and every other type: exact text only
}

// Split the expected block into rows of cells, DuckDB style: one row per line
// with tab-separated columns, or (when that does not fit) one value per line.
function expectedRows(lines, nCols, nActualRows) {
  const split = lines.map((l) => l.split('\t'));
  if (split.every((r) => r.length === nCols)) return { rows: split };
  if (split.every((r) => r.length === 1) && lines.length === nActualRows * nCols) {
    const rows = [];
    for (let k = 0; k < lines.length; k += nCols) rows.push(lines.slice(k, k + nCols));
    return { rows };
  }
  const bad = split.findIndex((r) => r.length !== nCols);
  return { error: `expected line ${bad + 1} has ${split[bad].length} column(s), the query declares ${nCols}` };
}

const HASH_RE = /^(\d+) values hashing to ([0-9a-f]{32})$/;

// Row ordering in DuckDB's runner: rowsort sorts whole rows by their cells in
// column order (string comparison), valuesort sorts every value; both sides.
function cmpRows(a, b) {
  for (let k = 0; k < Math.min(a.length, b.length); k++) {
    if (a[k] < b[k]) return -1;
    if (a[k] > b[k]) return 1;
  }
  return a.length - b.length;
}

// record: { types, sortMode, expected }. rows: array of arrays of cells.
// sqlTypes: array of column type names, or null.
export function compareQuery(record, rows, sqlTypes = null) {
  if (record.expected === null) return { ok: true }; // no expected block: must just run

  const nCols = record.types.length;
  let actual = rows.map((r) => r.map(renderCell));
  const wrongWidth = actual.findIndex((r) => r.length !== nCols);
  if (wrongWidth >= 0) {
    return { ok: false, reason: `result has ${actual[wrongWidth].length} column(s), the query declares ${nCols} (${record.types})` };
  }

  if (record.sortMode === 'rowsort') actual.sort(cmpRows);
  else if (record.sortMode === 'valuesort') {
    const vals = actual.flat().sort();
    actual = [];
    for (let k = 0; k < vals.length; k += nCols) actual.push(vals.slice(k, k + nCols));
  }

  const hash = record.expected.length === 1 && HASH_RE.exec(record.expected[0]);
  if (hash) {
    const flat = actual.flat();
    const digest = createHash('md5').update(flat.map((v) => v + '\n').join('')).digest('hex');
    if (Number(hash[1]) !== flat.length) return { ok: false, reason: `expected ${hash[1]} values, got ${flat.length}` };
    return digest === hash[2] ? { ok: true } : { ok: false, reason: `hash mismatch: expected ${hash[2]}, got ${digest}` };
  }

  const exp = expectedRows(record.expected, nCols, actual.length);
  if (exp.error) return { ok: false, reason: exp.error };
  let expected = exp.rows;
  if (expected.length !== actual.length) {
    return { ok: false, reason: `row count mismatch: expected ${expected.length}, got ${actual.length}` };
  }
  if (record.sortMode === 'rowsort') expected = [...expected].sort(cmpRows);
  else if (record.sortMode === 'valuesort') {
    const vals = expected.flat().sort();
    expected = [];
    for (let k = 0; k < vals.length; k += nCols) expected.push(vals.slice(k, k + nCols));
  }

  for (let r = 0; r < expected.length; r++) {
    for (let c = 0; c < nCols; c++) {
      const t = sqlTypes ? sqlTypes[c] : null;
      if (!valuesEqual(t, record.types[c], expected[r][c], actual[r][c])) {
        return {
          ok: false,
          reason: `row ${r + 1} column ${c + 1}${t ? ` (${t})` : ''}: expected "${expected[r][c]}", got "${actual[r][c]}"`,
        };
      }
    }
  }
  return { ok: true };
}

// ---- Running -------------------------------------------------------------

// Native sqllogictest semantics: a plain expected message is a substring of the
// error; `<REGEX>:pattern` must match the whole message (dot matches newlines).
export function errorMatches(expected, message) {
  if (!expected) return true;
  if (expected.startsWith('<REGEX>:')) {
    return new RegExp(`^(?:${expected.slice('<REGEX>:'.length)})$`, 's').test(message);
  }
  return message.includes(expected);
}

export class RecordTimeout extends Error {}

// A query that never returns would otherwise surface only as the CI job's
// timeout, with no record named. The hung query keeps the worker busy, so the
// caller must abort the run on a RecordTimeout rather than continue.
function withTimeout(promise, ms, what) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new RecordTimeout(`timed out after ${ms / 1000} s: ${what}`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

const RETRY_RAW = /Binder Error|Parser Error/;

// `runQuery(sql)` must return an array of rows, each an array of cells in
// column order (not keyed by name: duplicate column names must survive).
export async function runRecords(records, runQuery, { file, log, timeoutMs = RECORD_TIMEOUT_MS }) {
  const result = { file, passed: 0, failed: 0, skipped: 0, untyped: 0, raw: 0, failures: [] };
  const fail = (rec, reason) => {
    result.failed++;
    result.failures.push({ where: `${file}:${rec.line}`, sql: rec.sql || rec.kw || '', reason });
  };

  for (const rec of records) {
    if (rec.skip) { result.skipped++; continue; }
    if (rec.type === 'directive' || rec.type === 'halt') continue;
    if (rec.type === 'unknown') { fail(rec, `unsupported record "${rec.kw}"`); continue; }

    const what = `${file}:${rec.line}`;
    const run = (sql) => withTimeout(runQuery(sql), timeoutMs, what);

    if (rec.type === 'statement') {
      try {
        await run(rec.sql);
        if (rec.expectOk) result.passed++;
        else fail(rec, 'expected an error, but the statement succeeded');
      } catch (err) {
        if (err instanceof RecordTimeout) throw err;
        const msg = String(err.message || err);
        if (!rec.expectOk && errorMatches(rec.errorSubstr, msg)) result.passed++;
        else if (rec.expectOk) fail(rec, `unexpected error: ${msg}`);
        else fail(rec, `error message mismatch: expected "${rec.errorSubstr}", got "${msg}"`);
      }
      continue;
    }

    if (rec.type === 'query') {
      try {
        // Render through DuckDB's own ::VARCHAR, as native sqllogictest does,
        // rather than through Arrow-JS (which mis-renders DECIMAL). The column
        // types come from DESCRIBE, which only binds. Only a query the wrap
        // cannot bind or parse (PRAGMA, SHOW, ...) is retried raw; a runtime
        // error is the query's own and is reported, not run a second time.
        const inner = rec.sql.trim().replace(/;\s*$/, '');
        let rows;
        let sqlTypes = null;
        try {
          rows = await run(`SELECT COLUMNS(*)::VARCHAR FROM (\n${inner}\n) AS _wrap`);
          try {
            const d = await run(`DESCRIBE SELECT * FROM (\n${inner}\n) AS _wrap`);
            sqlTypes = d.map((r) => String(r[1]));
          } catch (err) {
            if (err instanceof RecordTimeout) throw err;
          }
        } catch (err) {
          if (err instanceof RecordTimeout || !RETRY_RAW.test(String(err.message || err))) throw err;
          result.raw++;
          rows = await run(rec.sql);
        }
        if (!sqlTypes) result.untyped++;
        const cmp = compareQuery(rec, rows, sqlTypes);
        if (cmp.ok) result.passed++;
        else fail(rec, cmp.reason);
      } catch (err) {
        if (err instanceof RecordTimeout) throw err;
        fail(rec, `query threw: ${err.message || err}`);
      }
      continue;
    }
  }

  if (log && result.failed > 0) {
    for (const f of result.failures) {
      log(`    ✗ ${f.where}: ${f.reason}\n      SQL: ${f.sql.replace(/\n/g, ' ').slice(0, 160)}`);
    }
  }
  return result;
}

// ---- Self-test -----------------------------------------------------------

export function selfTest() {
  const q = (types, expected, sortMode = 'nosort') => ({ types, sortMode, expected });
  const cases = [
    // [name, record, rows, sqlTypes, expect ok]
    ['integers exact: 100000 vs 100001', q('I', ['100000']), [['100001']], ['BIGINT'], false],
    ['integers exact, no type info: 100000 vs 100001', q('I', ['100000']), [['100001']], null, false],
    ['BIGINT precision: 9007199254740993 vs ...992', q('I', ['9007199254740993']), [['9007199254740992']], ['BIGINT'], false],
    ['BIGINT equal beyond 2^53', q('I', ['9007199254740993']), [['9007199254740993']], ['BIGINT'], true],
    ['text exact: "001" vs "1"', q('T', ['001']), [['1']], ['VARCHAR'], false],
    ['text exact, no type info: "001" vs "1"', q('T', ['001']), [['1']], null, false],
    ['shape: rows [[1],[2]] vs "1\\t2" under query II', q('II', ['1\t2']), [['1'], ['2']], null, false],
    ['shape: one column returned, II declared', q('II', ['1', '2']), [['1'], ['2']], null, false],
    ['row count: 2 expected, 3 returned', q('I', ['1', '2']), [['1'], ['2'], ['3']], ['INTEGER'], false],
    ['two columns, tab separated', q('II', ['1\t2']), [['1', '2']], ['INTEGER', 'INTEGER'], true],
    ['two columns, one value per line', q('II', ['1', '2']), [['1', '2']], ['INTEGER', 'INTEGER'], true],
    ['double within 1e-9', q('R', ['0.30000000000000004']), [['0.3']], ['DOUBLE'], true],
    ['double off by 1e-6 relative', q('R', ['1.000001']), [['1.0']], ['DOUBLE'], false],
    ['DECIMAL scale: 1.0 vs 1.00', q('R', ['1.0']), [['1.00']], ['DECIMAL(18,2)'], true],
    ['boolean: query I true vs 1', q('I', ['1']), [['true']], ['BOOLEAN'], true],
    ['boolean: true vs false', q('I', ['true']), [['false']], ['BOOLEAN'], false],
    ['NULL vs NULL', q('I', ['NULL']), [[null]], ['INTEGER'], true],
    ['NULL vs 0', q('I', ['NULL']), [['0']], ['INTEGER'], false],
    ['empty string renders as (empty)', q('T', ['(empty)']), [['']], ['VARCHAR'], true],
    ['empty string vs NULL', q('T', ['(empty)']), [[null]], ['VARCHAR'], false],
    ['list text exact', q('T', ['[1, 2]']), [['[1,2]']], ['BIGINT[]'], false],
    ['rowsort sorts both sides', q('II', ['b\t1', 'a\t2'], 'rowsort'), [['a', '2'], ['b', '1']], ['VARCHAR', 'INTEGER'], true],
    ['nosort keeps order', q('I', ['2', '1']), [['1'], ['2']], ['INTEGER'], false],
    ['valuesort', q('I', ['3', '1', '2'], 'valuesort'), [['2'], ['3'], ['1']], ['INTEGER'], true],
    ['hash match', q('I', ['2 values hashing to ' + createHash('md5').update('1\n2\n').digest('hex')]), [['1'], ['2']], ['INTEGER'], true],
    ['hash mismatch', q('I', ['2 values hashing to ' + createHash('md5').update('1\n3\n').digest('hex')]), [['1'], ['2']], ['INTEGER'], false],
  ];
  const failures = [];
  for (const [name, rec, rows, types, want] of cases) {
    const got = compareQuery(rec, rows, types).ok;
    if (got !== want) failures.push(`${name}: expected ${want ? 'accept' : 'reject'}, got ${got ? 'accept' : 'reject'}`);
  }

  // Parser: unknown keywords become failing records with their line; the
  // `statement error` message is split from the SQL.
  const parsed = parseTest('statement error\nSELECT 1/0;\n----\nboom\n\nloop i 0 3\nquery I\nSELECT 1\n----\n1\nendloop\n\nskipif wasm\nquery I\nSELECT 1\n');
  const unknown = parsed.filter((r) => r.type === 'unknown').map((r) => r.line);
  if (parsed[0].sql !== 'SELECT 1/0;' || parsed[0].errorSubstr !== 'boom') failures.push('statement error: SQL/message not split');
  if (unknown.join(',') !== '6,13') failures.push(`unknown keywords: expected lines 6,13, got ${unknown.join(',')}`);
  if (!errorMatches('<REGEX>:.*cap.*', 'x cap y') || errorMatches('<REGEX>:cap', 'x cap y')) failures.push('REGEX matching');

  if (failures.length) {
    for (const f of failures) process.stderr.write(`SELF-TEST FAILURE: ${f}\n`);
    return 1;
  }
  process.stdout.write(`sqllogic self-test: ${cases.length} comparator cases and 3 parser checks passed\n`);
  return 0;
}

if (process.argv[1] === fileURLToPath(import.meta.url) && process.argv.includes('--self-test')) {
  process.exit(selfTest());
}
