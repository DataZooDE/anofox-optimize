#!/usr/bin/env python3
"""Does anofox-visualization pick up anofox_optimize's contract producers?

anofox-visualization plots any table that follows the anofox integration
contract (anofox-visualization docs/plans/integration-contract.md): its
`anofox_plot(tbl)` picks the chart from the columns, and producers advertise
themselves with `duckdb_functions()` tags
`{'anofox.contract': '1', 'anofox.output': '<schema>'}`. This script is the
definition of done for optimize's producers (docs/ANOFOX_CONTRACT_PLAN.md). It
loads the optimize artifact that was just built next to a released
anofox_visualization in a stock DuckDB CLI and checks, each in a fresh process:

  1. both extensions load in either order, and `anofox_contract_version()`
     (registered by whichever loads first) exists once and returns '1';
  2. discovery: the tagged optimize producers are exactly EXPECTED_PRODUCERS (the
     long forms produce assignment, schedule, selection, sequence or weights,
     the objectives summary),
     and the visualization has the plot macro for each one's schema;
  3. every producer's output renders: `anofox_plot(t)` is an SVG with no
     render warning, and it is the same SVG as the schema's own macro, so the
     auto-detection chose that schema; every algorithm (model_id) is drawn,
     several algorithms in one plot. Packings are plotted one group at a time:
     the assignment plot facets by model_id only, so two groups' bin 1 would
     stack into one bar. A schedule's groups are its resources (one lane each);
  4. the values survive: every item is a block of its size, and every job is a
     bar with its own start, end, status and due date; a selection is a bar
     per item of its measure, coloured by whether it was picked; a sequence is
     a step per position labelled "position · item" at its cumulative setup;
     an allocation is a segment per item of its weight in its algorithm's bar.

    python3 scripts/visualization_pickup.py \\
        build/release/extension/anofox_optimize/anofox_optimize.duckdb_extension

The DuckDB CLI is downloaded (--duckdb-version, default v1.5.6) unless
--duckdb names one. The visualization comes from its project repository
(--viz-repo or VIZ_REPO; `community` for the community repository).
Telemetry is off. Exit status 1 when any check fails.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_test import _download_duckdb_cli  # noqa: E402  # pyright: ignore[reportMissingImports]

# The tagged producers this extension ships: function -> contract schema.
# Kept apart from the C++ list (src/contract.cpp) on purpose, so a tag that
# goes missing or appears unplanned fails here.
EXPECTED_PRODUCERS = {
    prefix + base: schema
    for base, schema in (("pack_by", "assignment"), ("pack_objectives_by", "summary"),
                         ("wave_by", "assignment"), ("wave_objectives_by", "summary"),
                         ("schedule_by", "schedule"), ("schedule_objectives_by", "summary"),
                         ("knapsack_by", "selection"), ("knapsack_objectives_by", "summary"),
                         ("sequence_by", "sequence"), ("sequence_objectives_by", "summary"),
                         ("assortment_by", "selection"), ("assortment_objectives_by", "summary"),
                         ("assortment_recapture_by", "selection"), ("assortment_recapture_objectives_by", "summary"),
                         ("portfolio_by", "weights"), ("portfolio_objectives_by", "summary"))
    for prefix in ("opt_", "anofox_optimize_")
}

SCHEMA_MACRO = {"assignment": "anofox_plot_assignment", "schedule": "anofox_plot_schedule",
                "summary": "anofox_plot_summary", "selection": "anofox_plot_selection",
                "sequence": "anofox_plot_sequence", "weights": "anofox_plot_weights"}

DEFAULT_VIZ_REPO = "https://datazoode.github.io/anofox-visualization/extensions"

# 24 cartons at two sites, 10 units per container or wave; 6 jobs on two lines with
# sequence-dependent setups (from_id NULL: before the first job).
FIXTURE = """
CREATE TABLE cartons AS SELECT 'site ' || (i % 2) AS site, i AS item, (2 + (i * 5) % 7)::DOUBLE AS size,
    (1 + (i * 3) % 4)::DOUBLE AS priority FROM range(1, 25) t(i);
CREATE TABLE jobs AS SELECT 'line ' || (i % 2) AS line, i AS job, (4 + (i * 3) % 5)::DOUBLE AS p,
    (8 + 6 * i)::DOUBLE AS due, (1 + i % 3)::DOUBLE AS w FROM range(0, 12) t(i);
CREATE TABLE setups AS SELECT CASE WHEN a = 0 THEN NULL ELSE a - 1 END AS from_id, b - 1 AS to_id,
    CASE WHEN a = 0 THEN 1.0 ELSE (1 + (a * 7 + b * 3) % 4)::DOUBLE END AS value
FROM range(0, 13) x(a), range(1, 13) y(b) WHERE a <> b;
CREATE TABLE products AS SELECT 'shop ' || (i % 2) AS shop, i AS sku, (2 + (i * 3) % 5)::DOUBLE AS margin,
    (60 + 15 * ((i * 2) % 5))::DOUBLE AS demand, (3 + (i * 7) % 11)::DOUBLE AS kg, 0.04 + 0.01 * (i % 5) AS ret
FROM range(0, 12) t(i);
CREATE TABLE pairs AS SELECT a AS from_id, b AS to_id, round(abs(sin(a * 3 + b)) * 10, 3) AS dist,
    CASE WHEN a // 2 = b // 2 THEN 0.4 ELSE 0.03 END AS frac,
    CASE WHEN a = b THEN 0.02 + 0.005 * (a % 4) ELSE 0.003 END AS cov
FROM range(0, 12) x(a), range(0, 12) y(b);
"""

PACK = "'cartons', 'item', 'size', 10.0, group_col := 'site', algorithms := ['next_fit', 'bin_completion']"
WAVE = ("'cartons', 'item', 'size', 'priority', 10.0, group_col := 'site', "
        "algorithms := ['as_given', 'priority_first', 'best_of']")
SCHEDULE = ("'jobs', 'job', 'p', 'due', 'w', setup_data := 'setups', group_col := 'line', "
            "algorithms := ['edd', 'wspt', 'best_of']")
KNAPSACK = ("'products', 'sku', 'margin', 'kg', 25.0, group_col := 'shop', "
            "algorithms := ['greedy_value', 'greedy_ratio', 'exact']")
SEQUENCE = ("'products', 'sku', '(SELECT from_id, to_id, dist AS value FROM pairs)', group_col := 'shop', "
            "algorithms := ['as_given', 'nearest_neighbour', 'two_opt']")
ASSORTMENT = ("'products', 'sku', 'margin', 'demand', '(SELECT from_id, to_id, frac AS value FROM pairs "
              "WHERE from_id <> to_id)', 3, group_col := 'shop', algorithms := ['top_margin', 'local_search']")
PORTFOLIO = ("'products', 'sku', 'ret', '(SELECT from_id, to_id, cov AS value FROM pairs)', 3, 0.5, "
             "group_col := 'shop', algorithms := ['top_return', 'greedy_sharpe']")
ARGS = {"pack": PACK, "wave": WAVE, "schedule": SCHEDULE, "knapsack": KNAPSACK, "sequence": SEQUENCE,
        "assortment": ASSORTMENT, "assortment_recapture": ASSORTMENT, "portfolio": PORTFOLIO}
CALLS = {f"opt_{f}{kind}": f"opt_{f}{kind}({args})" for f, args in ARGS.items() for kind in ("_by", "_objectives_by")}
# The column a producer's plots are split by (one plot per value), or None for one plot.
PLOT_PER = {"opt_pack_by": "site", "opt_wave_by": "site", "opt_pack_objectives_by": "site",
            "opt_wave_objectives_by": "site", "opt_schedule_objectives_by": "line",
            **{f"opt_{f}{kind}": "shop"
               for f in ("knapsack", "sequence", "assortment", "assortment_recapture", "portfolio")
               for kind in ("_by", "_objectives_by")}}

# Both renders get the same size: anofox_plot and the schema macros have different default sizes.
SIZE = "width := 640, height := 400"


def num(x: float) -> str:
    return f"{x:g}"


def rects(svg: str, *attrs: str) -> list[tuple[str, ...]]:
    """The `attrs` of every <rect> in `svg` that carries all of them."""
    out = []
    for tag in re.findall(r"<rect\b[^>]*>", svg):
        found = dict(re.findall(r'([\w-]+)="([^"]*)"', tag))
        if all(a in found for a in attrs):
            out.append(tuple(found[a] for a in attrs))
    return out


class Pickup:
    def __init__(self, duckdb: str, ext: str, viz_repo: str, home: str):
        self.duckdb, self.ext, self.viz_repo, self.home = duckdb, ext, viz_repo, home
        self.failures: list[str] = []

    def run(self, sql: str, load: str = "opt,viz") -> tuple[bool, str, list[dict]]:
        """`sql` in a fresh `duckdb -unsigned -json` process after the LOADs → (ok, output, last result rows)."""
        names = {"opt": "anofox_optimize", "viz": "anofox_visualization"}
        loads = "".join(f"LOAD {names[x]};\n" for x in load.split(",") if x)
        env = dict(os.environ, HOME=self.home, USERPROFILE=self.home, DATAZOO_DISABLE_TELEMETRY="1")
        p = subprocess.run([self.duckdb, "-unsigned", "-json", "-c", loads + sql],
                           capture_output=True, text=True, env=env, timeout=600)
        stdout = p.stdout or ""
        rows: list[dict] = []
        dec, i = json.JSONDecoder(), 0
        while (j := stdout.find("[", i)) >= 0:
            try:
                parsed, i = dec.raw_decode(stdout, j)
                if isinstance(parsed, list):
                    rows = parsed
            except ValueError:
                i = j + 1
        return p.returncode == 0, stdout + (p.stderr or ""), rows

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f"\n       {detail}" if detail and not ok else ""))
        if not ok:
            self.failures.append(name)
        return ok

    def install(self) -> None:
        src = "community" if self.viz_repo == "community" else f"'{self.viz_repo}'"
        ok, out, _ = self.run(f"INSTALL '{self.ext}';\nFORCE INSTALL anofox_visualization FROM {src};", load="")
        if not ok:
            raise SystemExit(f"Visualization pickup FAILED: could not install the extensions\n{out}")
        _, _, rows = self.run("SELECT extension_name, extension_version FROM duckdb_extensions() "
                              "WHERE extension_name IN ('anofox_optimize', 'anofox_visualization') ORDER BY 1;")
        for r in rows:
            print(f"  {r['extension_name']} {r['extension_version']}")

    # ── 1 ──
    def load_order(self) -> None:
        print("[1] loading in either order, one shared anofox_contract_version()")
        for order in ("opt,viz", "viz,opt"):
            ok, out, rows = self.run("SELECT count(*) AS n, any_value(anofox_contract_version()) AS v "
                                     "FROM duckdb_functions() WHERE function_name = 'anofox_contract_version';",
                                     load=order)
            self.check(f"LOAD {order.replace(',', ' then ')}", ok and rows == [{"n": 1, "v": "1"}],
                       out.strip()[-500:])

    # ── 2 ──
    def discovery(self) -> None:
        print("[2] discovery through duckdb_functions() tags")
        ok, out, rows = self.run("SELECT function_name AS f, tags['anofox.output'] AS s FROM duckdb_functions() "
                                 "WHERE tags['anofox.contract'] = '1' AND (function_name LIKE 'opt\\_%' ESCAPE '\\' "
                                 "OR function_name LIKE 'anofox\\_optimize\\_%' ESCAPE '\\');")
        found = {r["f"]: r["s"] for r in rows}
        self.check("tagged optimize producers are exactly the expected ones", ok and found == EXPECTED_PRODUCERS,
                   f"expected {EXPECTED_PRODUCERS}\n       found    {found}\n       {out.strip()[-300:]}")
        ok, out, rows = self.run("SELECT DISTINCT function_name AS f, tags['anofox.consumes'] AS c "
                                 "FROM duckdb_functions() WHERE function_name LIKE 'anofox\\_plot%' ESCAPE '\\';")
        macros = {r["f"]: r["c"] for r in rows}
        consumes = macros.get("anofox_plot") or ""
        for schema in sorted(set(EXPECTED_PRODUCERS.values())):
            self.check(f"the visualization has {SCHEMA_MACRO[schema]} for '{schema}'",
                       ok and SCHEMA_MACRO[schema] in macros, out[-300:])
            if consumes:  # the community (C++) build tags its consumers; the C-API build has no tags
                self.check(f"anofox_plot declares it consumes '{schema}'", schema in consumes.split(","), consumes)

    # ── 3 / 4 ──
    def render(self) -> None:
        print("[3] rendering every producer's output, several algorithms per plot; [4] the values survive")
        for producer, call in CALLS.items():
            schema = EXPECTED_PRODUCERS[producer]
            macro = SCHEMA_MACRO[schema]
            per = PLOT_PER.get(producer)
            key = f'"{per}"::VARCHAR' if per else "NULL::VARCHAR"
            ok, out, rows = self.run(
                f"{FIXTURE}\nCREATE TABLE t AS SELECT * FROM {call};\n"
                f"SELECT {key} AS part, anofox_plot(p, {SIZE}) AS auto, {macro}(p, {SIZE}) AS specific, "
                "list(DISTINCT model_id ORDER BY model_id) AS models, to_json(list(p)) AS rows "
                f"FROM t p GROUP BY ALL ORDER BY 1;")
            if not self.check(f"{producer}: anofox_plot renders", ok and len(rows) >= 1, out.strip()[-600:]):
                continue
            for r in rows:
                auto, label = r["auto"] or "", producer + (f" [{r['part']}]" if r["part"] else "")
                self.check(f"{label}: an SVG", auto.startswith("<svg"), auto[:200])
                warn = re.search(r'data-warnings="([^"]*)"', auto)
                self.check(f"{label}: no render warnings", warn is None, warn.group(1) if warn else "")
                self.check(f"{label}: auto-detected as '{schema}' (same SVG as {macro})", auto == r["specific"])
                self.check(f"{label}: every algorithm drawn ({', '.join(r['models'])})",
                           len(r["models"]) >= 2 and all(f">{m}<" in auto for m in r["models"]), str(r["models"]))
                data = json.loads(r["rows"]) if isinstance(r["rows"], str) else r["rows"]
                values = getattr(self, f"{schema}_values", None)  # summary has no per-value check
                if values:
                    values(label, auto, data)
            if per:
                self.check(f"{producer}: one plot per {per}", len(rows) >= 2, str([r["part"] for r in rows]))

    def assignment_values(self, label: str, svg: str, data: list[dict]) -> None:
        blocks = Counter((series, float(value)) for series, value in rects(svg, "data-series", "data-value"))
        want = Counter((str(d["item"]), float(d["size"])) for d in data)
        self.check(f"{label}: every item a block of its size ({sum(want.values())})", blocks == want,
                   f"svg {sorted(blocks.items())[:6]}…, table {sorted(want.items())[:6]}…")

    def schedule_values(self, label: str, svg: str, data: list[dict]) -> None:
        bars = Counter(re.findall(r"<title>([^<]*)</title>", svg))
        want = Counter(f"{d['task']} · {d['resource']}: {num(d['start'])} – {num(d['end'])} · {d['status']} · "
                       f"due {num(d['due'])}" for d in data)
        resources = sorted({d["resource"] for d in data})
        self.check(f"{label}: every job a bar with its start, end, status and due date ({sum(want.values())})",
                   all(bars[k] >= n for k, n in want.items()),
                   f"missing {sorted(set(want) - set(bars))[:4]}")
        self.check(f"{label}: one lane per resource ({', '.join(resources)})",
                   all(f">{x}<" in svg for x in resources), str(resources))
        late = sum(1 for d in data if d["status"] == "late")
        caption = f"{len(data)} tasks on {len(resources)} resources · {late} late"
        self.check(f"{label}: the caption counts the tasks and late jobs", caption in svg, caption)

    def selection_values(self, label: str, svg: str, data: list[dict]) -> None:
        flag = "selected" if "selected" in data[0] else "listed"
        # the bar's measure: value, else margin x demand (an assortment's expected margin)
        measure = (lambda d: d["value"]) if "value" in data[0] else (lambda d: d["margin"] * d["demand"])
        bars = [(x, s, fill, round(float(v), 9)) for fill, x, s, v in
                rects(svg, "fill", "data-x", "data-series", "data-value")]
        got = Counter((x, s, v) for x, s, _, v in bars)
        want = Counter((str(d["item"]), flag if d[flag] else f"not {flag}", round(measure(d), 9)) for d in data)
        self.check(f"{label}: every item a bar of its measure, {flag} or not ({sum(want.values())})", got == want,
                   f"svg {sorted(got.items())[:4]}…, table {sorted(want.items())[:4]}…")
        fills = {s: {f for _, s2, f, _ in bars if s2 == s} for s in (flag, f"not {flag}")}
        self.check(f"{label}: {flag} and not {flag} each have their own colour ({fills})",
                   all(len(f) == 1 for f in fills.values()) and fills[flag] != fills[f"not {flag}"])

    def sequence_values(self, label: str, svg: str, data: list[dict]) -> None:
        points = Counter((m.group(1), round(float(m.group(2)), 9)) for m in
                         re.finditer(r'<circle[^>]*data-x="([^"]*)" data-value="([^"]*)"', svg))
        want = Counter((f"{d['position']} · {d['item']}", round(d["cumulative_setup"], 9)) for d in data)
        self.check(f"{label}: every step labelled position · item at its cumulative setup ({sum(want.values())})",
                   points == want, f"svg {sorted(points.items())[:4]}…, table {sorted(want.items())[:4]}…")
        lines = len(re.findall(r'<polyline[^>]*data-geom="step"', svg))
        models = len({d["model_id"] for d in data})
        self.check(f"{label}: one step line per algorithm ({models})", lines == models, f"{lines} step lines")

    def weights_values(self, label: str, svg: str, data: list[dict]) -> None:
        # several algorithms: one 100 % bar per algorithm (x), a segment per item (series) of its weight
        segments = Counter((x, s, round(float(v), 9)) for x, s, v in rects(svg, "data-x", "data-series", "data-value"))
        want = Counter((d["model_id"], str(d["item"]), round(d["weight"], 9)) for d in data)
        self.check(f"{label}: every item a segment of its weight in its algorithm's bar ({sum(want.values())})",
                   segments == want, f"svg {sorted(segments.items())[:4]}…, table {sorted(want.items())[:4]}…")


def main() -> int:
    ap = argparse.ArgumentParser(description="anofox-visualization picks up anofox_optimize's contract producers")
    ap.add_argument("extension", help="the built anofox_optimize.duckdb_extension")
    ap.add_argument("--duckdb", help="DuckDB CLI to use (default: download the official one)")
    ap.add_argument("--duckdb-version", default="v1.5.6")
    ap.add_argument("--arch", default="linux_amd64")
    ap.add_argument("--viz-repo", default=os.environ.get("VIZ_REPO", DEFAULT_VIZ_REPO),
                    help="extension repository of anofox_visualization ('community' for the community one)")
    args = ap.parse_args()

    ext = os.path.abspath(args.extension).replace("\\", "/")
    if not os.path.isfile(ext):
        print(f"Visualization pickup FAILED: artifact not found: {ext}")
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        duckdb = args.duckdb or _download_duckdb_cli(args.duckdb_version, args.arch, tmp)
        home = os.path.join(tmp, "home")
        os.makedirs(home)
        print(f"\nVisualization pickup\n  optimize : {ext}\n  duckdb   : {duckdb}\n  viz      : {args.viz_repo}")
        t = Pickup(duckdb, ext, args.viz_repo, home)
        t.install()
        t.load_order()
        t.discovery()
        t.render()
    if t.failures:
        print(f"\nVisualization pickup FAILED ({len(t.failures)}):\n  " + "\n  ".join(t.failures))
        return 1
    print("\nVisualization pickup PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
