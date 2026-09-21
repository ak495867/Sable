# SABLE v3.1

> A self-optimizing database engine that adapts its execution and storage strategies to observed workload behavior.

[![Python](https://img.shields.io/badge/python-3.10%2B-3776AB?logo=python&logoColor=white )](https://www.python.org/ )
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg )](LICENSE)
[![Version](https://img.shields.io/badge/version-3.1.0-blue.svg )](https://github.com/ak495867/Sable/releases )
[![Tests](https://img.shields.io/badge/tests-27%20passing-brightgreen.svg )](#testing)
[![SQLite](https://img.shields.io/badge/storage-SQLite-003B57?logo=sqlite&logoColor=white )](https://sqlite.org/ )
[![Status](https://img.shields.io/badge/status-research%20prototype-orange.svg )](#deliberate-v31-boundaries)
[![GitHub stars](https://img.shields.io/github/stars/ak495867/Sable?style=social )](https://github.com/ak495867/Sable/stargazers )
[![GitHub last commit](https://img.shields.io/github/last-commit/ak495867/Sable )](https://github.com/ak495867/Sable/commits/main )


SABLE v3.0 is a self-optimizing database engine with autonomous physical design, learned query planning, adaptive execution, and hot/warm/cold storage migration. It uses SQLite as a durable transactional storage kernel and adds the SABLE feedback loop around it: **observe → measure → model → propose → validate/commit → adapt**.

## What v3.0 includes

- Durable local database files with SQLite WAL and snapshot-safe transactions.
- SQL execution through `SableDB.execute()`.
- Key/value convenience API through `put()` and `get()`.
- Query fingerprints and a plan cache.
- Runtime telemetry for latency, rows scanned/returned, chosen plan, epoch, and index use.
- Workload model with point-lookup, scan, aggregate, join, write, phase, percentile, and empirical plan statistics.
- Deterministic heuristic adaptation in epochs with workload-phase change detection.
- Safe proposal/commit/reject workflow for key indexing, layout, compression, and prefetch policy changes.
- Explicit transactions with rollback and bulk ingestion.
- CLI status, query, put, approval, and telemetry export commands.
- Tests and a phase-changing benchmark harness.
- Research subsystems for durable row/column segments, NumPy vectorized execution, nested-loop/hash/merge joins, measured compression, shadow validation, UCB1 bandits, and tabular Q-learning.

### Autonomous Features (v3.0)

- **Adaptive Storage Layer** (`AccessTracker`, `AdaptiveStorageManager`) - Tracks access patterns (frequency/recency) for hot/warm/cold classification and manages tier migration with compression selection.
- **Query Planner** (`CostModel`, `AdaptivePlanner`) - Learned cost model using historical telemetry to estimate query cost based on rows, indexes, joins, and aggregates.
- **Execution Engine** (`AdaptiveExecutionEngine`) - Adaptive batch size selection based on latency feedback, dynamic parallelism, and memory limits.
- **Autonomous Physical Design** (`AutonomousPhysicalDesigner`) - Continuous validation using ShadowExecutor and auto-apply of validated physical design proposals.

### Integration

The autonomous components are integrated into `AdaptiveEngine` and `SableDB.execute()`. The adaptive planner, cost model, execution engine, and physical designer are automatically invoked during query execution. All autonomous features are accessible via the `SableDB` API:

```python
from sable import SableDB

db = SableDB("market.db", epoch_size=25)
db.put("trade:1", {"symbol": "AAPL", "price": 227.10})
print(db.get("trade:1"))
print(db.execute("SELECT key, value FROM sable_records WHERE key = ?", ("trade:1",)))
print(db.adaptive.status())

# Access autonomous components directly
print(db.access_tracker_status())
print(db.storage_manager_status())
print(db.cost_model_status())
print(db.execution_status())
print(db.physical_designer_status())
```

## Quickstart

```bash
cd Sable
python3 -m venv .venv
. .venv/bin/activate
pip install -e '.[dev]'
pytest -q

sable demo.db put symbol AAPL
sable demo.db status
sable demo.db query 'SELECT key, value FROM sable_records WHERE key = ?' AAPL
sable demo.db export telemetry.jsonl
```

Use the Python API:

```python
from sable import SableDB

db = SableDB("market.db", epoch_size=25)
db.put("trade:1", {"symbol": "AAPL", "price": 227.10})
print(db.get("trade:1"))
print(db.execute("SELECT key, value FROM sable_records WHERE key = ?", ("trade:1",)))
print(db.adaptive.status())
# Review and explicitly commit a proposed physical change:
print(db.adaptive.approve_proposal())
```

## Architecture

```text
SQL / API
   ↓
SableDB execution wrapper
   ↓
Plan choice + fingerprint/cache (AdaptivePlanner + CostModel)
   ↓
SQLite transactional kernel (WAL)
   ↓
Telemetry event
   ↓
Workload model
   ↓ epoch boundary
Safe adaptive proposal
   ↓ explicit approval
Physical change + cache invalidation
```

The v3 policy engine is deterministic and empirical. It compares observed plan latency when choosing among safe candidates, but it does not pretend to use ML. Later bandit and reinforcement-learning policies can implement the same `choose_plan`, `record`, and proposal interfaces.

SABLE records both its logical plan proposal and SQLite's verified `EXPLAIN QUERY PLAN` details. An index-backed logical plan is never proposed unless the physical index exists, and `Telemetry.index_used` reflects SQLite's actual plan.

Adaptive state is persisted in the internal `_sable_state` table, including telemetry, workload statistics, proposals, journals, plan cache, and policy settings. This lets a reopened database continue learning from its previous workload.

## Safety model

SABLE never silently mutates physical design from a heuristic observation. Adaptations are represented as proposals with a reason, details, and epoch. A caller must explicitly approve or reject a proposal. This is the v3 implementation of `proposal → estimate → validate → commit`.

## Research benchmark

```bash
python3 benchmarks/phase_change.py --rows 5000 --epoch-size 50
```

The benchmark runs point lookups, scans, aggregates, and writes in changing phases and prints JSON containing telemetry counts, epochs, proposals, and p50 latency. It generates measurements; it does not manufacture performance claims.

Run the research-feature benchmark with:

```bash
python3 benchmarks/research_features.py
```

The experimental APIs and their limits are documented in [`docs/research-features.md`](docs/research-features.md). The default database policy remains deterministic, but `db.adaptive.enable_policy("bandit")` or `enable_policy("q_learning")` activates measured experimental policy selection explicitly.

## Deliberate v3.1 boundaries

The full research vision includes native row/column storage, SIMD batches, multiple join algorithms, physically adaptive compression, buffer-policy experiments, shadow execution, bandits, RL, and distributed operation. Those are not falsely labeled complete here. v3.0 establishes stable interfaces, policy proposals for these physical dimensions, and a real end-to-end adaptive loop on a conservative transactional kernel.

## License

MIT. See `LICENSE`.
