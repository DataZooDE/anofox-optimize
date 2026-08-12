# Third-party notices

- **DuckDB** — MIT. https://github.com/duckdb/duckdb
- **datazoo-banner** — DataZoo GmbH, vendored as a submodule.
- **posthog-telemetry** — DataZoo GmbH, vendored as a submodule.

No third-party algorithm implementations are vendored: the packing
algorithms in `src/packing.cpp` are written for this repository. Where an
algorithm is named after published work (first-fit-decreasing, bin
completion), the name refers to the published method, not to any
third-party code.
