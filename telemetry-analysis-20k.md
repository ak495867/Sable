# SABLE Telemetry Analysis

**Events:** 5602

## By plan

| Plan | Events | p50 (µs) | p95 (µs) | Mean (µs) |
|---|---:|---:|---:|---:|
| point_lookup | 5000 | 10.0 | 21.0 | 11.67 |
| scan | 600 | 12601.5 | 35265.8 | 15246.14 |
| write | 2 | 81904.5 | 149356.0 | 81904.50 |

## By detected workload phase

| Phase | Events | p50 (µs) | p95 (µs) |
|---|---:|---:|---:|
| mixed | 1 | 29.0 | 29.0 |
| point_lookups | 5048 | 10.0 | 22.0 |
| scans | 550 | 13.5 | 35472.4 |
| writes | 3 | 14453.0 | 149356.0 |

## Adaptation and instrumentation

- Adaptation epochs observed: **57**
- Index-used events: **0**; not indexed: **5602**
- Plan-cache hits: **5597**; misses: **5**

## Interpretation

The report is descriptive rather than a manufactured performance claim. Compare the phase and plan distributions against a fixed-policy run before drawing causal conclusions. A healthy adaptive run should show phase changes in the recent workload model, proposals at epoch boundaries, and index-used events only after the key-index proposal is explicitly committed.

## References

[1]: ../benchmarks/phase_change.py "SABLE phase-change benchmark harness"
[2]: ../sable/core.py "SABLE adaptive engine and telemetry schema"
