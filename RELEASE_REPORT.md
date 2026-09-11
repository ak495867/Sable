# SABLE v1.0 — Complete Release Report

## Executive summary

SABLE v1.0 is a single-node adaptive database research engine. It combines a durable SQLite transaction kernel with adaptive planning, workload telemetry, physical-design proposals, durable row/column segments, vectorized execution, physical join algorithms, compression, shadow validation, UCB1 policy selection, and tabular Q-learning.

The release is designed as an experimental systems platform. It measures decisions, records their outcomes, and exposes the policy state required for reproducible research. It does not claim production parity with SQLite, PostgreSQL, DuckDB, or other established database systems.

## Architecture

```text
SQL / Python API
        |
        v
SableDB transaction and execution wrapper
        |
        +--> SQLite WAL transactional kernel
        |
        +--> Query fingerprint and plan cache
        |
        +--> Runtime telemetry
                      |
                      v
              Workload model
                      |
                      v
              Epoch adaptation engine
                      |
        +-------------+--------------+
        |                            |
        v                            v
 Physical proposals              Research policies
 index/layout/compression        heuristic / UCB1 / Q-learning
        |                            |
        +-------------+--------------+
                      v
               measured execution
```

The default database policy is deterministic and empirical. Bandit and Q-learning policies are explicitly enabled through the adaptive engine for controlled experiments.

## Implemented capabilities

| Subsystem | v1.0 implementation | Evidence |
|---|---|---|
| Transactions | SQLite WAL, explicit transactions, rollback | `tests/test_sable.py` |
| Query execution | SQL wrapper, point lookup, scan, aggregate classification | `sable/core.py` |
| Telemetry | Latency, rows, phase, plan, cache, index, bytes, candidate plans | `sable/core.py` |
| Workload model | Recent window, phase detection, percentiles, plan statistics | `sable/core.py` |
| Adaptive design | Proposal, approval, rejection, epoch adaptation | `sable/core.py` |
| Durable segments | Appendable row/column segments, manifest, migration | `sable/research.py` |
| Vector execution | NumPy backend with Python fallback | `sable/research.py` |
| Joins | Nested-loop, hash, merge, sorted-input merge path | `sable/research.py` |
| Compression | None, zlib, delta, run-length encoding | `sable/research.py` |
| Shadow execution | Repeated baseline/candidate execution and equality validation | `sable/research.py` |
| Online policy | UCB1 join-arm selection and measured rewards | `sable/research.py` |
| Reinforcement learning | Tabular Q-learning with epsilon decay and normalized rewards | `sable/research.py`, `benchmarks/policy_simulation.py` |
| Research tooling | Feature benchmark, long policy simulation, convergence analyzer | `benchmarks/` |

## Benchmark results

### 20,000-row adaptive database benchmark

The phase-changing database benchmark generated 5,602 telemetry events across 56 completed adaptation epochs. It detected a transition from point lookups to scans and produced proposals for a key index and column-oriented layout.

The run produced 5,597 plan-cache hits and 5 misses. The key-index proposal remained uncommitted, demonstrating that SABLE does not silently mutate physical state.

### Research feature benchmark

The feature benchmark measured the actual physical operators and storage components.

| Operation | Result |
|---|---:|
| Nested-loop join | approximately 76.8 ms on the comparison input |
| Hash join | approximately 3.6 ms |
| Merge join | approximately 5.5 ms |
| NumPy million-element vector run | approximately 5.6 ms in the recorded run |
| Delta-compressed monotonic integers | 142 bytes |
| Row segment size for 5,000 rows | 103,340 bytes |
| Migrated column segment size for 5,000 rows | 29,797 bytes |

These numbers are measurements from the included harness, not universal performance guarantees.

### Extreme-selectivity policy simulation

The final policy study replayed 120,000 decisions across 240 fluctuating phases and 500 decisions per phase. It included tiny, large, sorted, sparse-selective, and many-to-many join workloads.

| Policy | Mean latency | Execution latency | Switching overhead | Mean regret | Switches |
|---|---:|---:|---:|---:|---:|
| UCB1 | 19,851.04 µs | 19,851.03 µs | 1,900 µs | 21.40 µs | 95 |
| Q-learning | 19,873.45 µs | 19,873.33 µs | 15,500 µs | 43.81 µs | 775 |

UCB1 won this run because hash join was the fastest measured strategy in every phase. Q-learning nevertheless improved substantially after state enrichment and reward shaping. Its mean regret decreased from approximately 712.25 µs in the earlier configuration to 43.81 µs, and switching overhead decreased from 75,360 µs to 15,500 µs.

The final Q-learning state was:

```text
(profile, size_bin, selectivity_bin)
```

The final reward was:

```text
-(execution_latency / oracle_latency)
- 0.25 * (switch_penalty / oracle_latency)
```

The convergence logs show that Q-learning’s remaining disadvantage came primarily from cold-start exploration and policy switching, rather than persistent selection of a bad join algorithm.

## Reproducibility

From the extracted project directory:

```bash
python3 verify.py
python3 -m compileall -q sable benchmarks tests
python3 benchmarks/phase_change.py --rows 20000 --epoch-size 100 \
  --telemetry benchmark-telemetry-20k.jsonl \
  --summary benchmark-summary-20k.json
python3 benchmarks/analyze_telemetry.py benchmark-telemetry-20k.jsonl \
  --output telemetry-analysis-20k.md
python3 benchmarks/research_features.py
python3 benchmarks/policy_simulation.py \
  --phases 240 \
  --episodes 500 \
  --seed 20260911 \
  --switch-cost-us 20 \
  --output policy-simulation-extreme-120k.json \
  --report policy-simulation-extreme-120k.md
python3 benchmarks/analyze_policy_simulation.py \
  policy-simulation-extreme-120k.json \
  --output policy-convergence-analysis.md
```

The test suite currently contains nine tests and is dependency-free beyond the runtime packages already used by the project. NumPy is used when installed for vectorized execution; the vector executor retains a Python fallback.

## Public API examples

```python
from sable import SableDB, SegmentStore, JoinExecutor

db = SableDB("sable.db", epoch_size=100)
db.put("trade:1", {"symbol": "AAPL", "price": 227.10})
print(db.get("trade:1"))
print(db.adaptive.status())

# Explicitly select a research policy.
db.adaptive.enable_policy("bandit")
algorithm = db.adaptive.choose_join_algorithm(left_rows, right_rows, "id")
joined = JoinExecutor.execute(left_rows, right_rows, "id", algorithm)
```

## Known limitations

SABLE v1.0 is a research platform rather than a production database. The durable transactional kernel remains SQLite. The segment store is a single-node physical-storage subsystem and does not yet provide MVCC integration with segment migration. The vector path uses NumPy’s compiled backend rather than a custom SABLE AVX2 or AVX-512 operator library. The learning policies are tabular and do not yet provide function approximation, persistent model checkpoints, or statistically rigorous confidence intervals.

The policy study also demonstrates an important limitation: a global UCB1 policy can outperform contextual Q-learning when one physical strategy dominates all measured phases. More discriminative workloads, calibrated exploration budgets, and controlled warm-start protocols are required to establish when contextual learning provides a reliable advantage.

## Release verification

The release was verified with:

```text
9/9 automated tests passed
Python compilation passed
20,000-row telemetry benchmark passed
Research feature benchmark passed
120,000-decision policy simulation passed
Convergence analysis generated
```

## References

[1]: README.md "SABLE v1.0 project documentation"
[2]: docs/research-features.md "SABLE research feature documentation"
[3]: sable/core.py "SABLE adaptive engine integration"
[4]: sable/research.py "SABLE research subsystems"
[5]: benchmarks/policy_simulation.py "SABLE UCB1 versus Q-learning simulation"
