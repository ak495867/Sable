# SABLE v3.2

> A self-optimizing, autonomous embedded database engine that adapts its physical design, query planning, and storage tiering to live workload behavior.

[![Python](https://img.shields.io/badge/python-3.10%2B-3776AB?logo=python&logoColor=white)](https://www.python.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-3.2.0-blue.svg)](https://github.com/ak495867/Sable/releases)
[![Tests](https://img.shields.io/badge/tests-33%20passing-brightgreen.svg)](#testing)
[![SQLite](https://img.shields.io/badge/storage-SQLite%20WAL-003B57?logo=sqlite&logoColor=white)](https://sqlite.org/)
[![Status](https://img.shields.io/badge/status-production%20ready-green.svg)](#where-sable-is-perfectly-used)

SABLE is a self-optimizing database engine with autonomous physical design, learned query planning, adaptive execution, LinUCB contextual bandits, statistical shadow validation, and hot/warm/cold storage tiering. It couples an ultra-fast SQLite WAL storage kernel with an autonomous closed-loop feedback controller: **observe → measure → model → propose → shadow validate → commit/rollback → adapt**.

---

## Where SABLE Is Perfectly Used

SABLE is tailored for environments that experience **dynamic, fluctuating workloads** and **cannot rely on a human Database Administrator (DBA)**:

### 1. Embedded AI Agent Memory & Execution Runtimes
- **The Challenge**: Autonomous coding agents, LLM assistants, and reasoning loops generate heavy write bursts (tool execution logs, scratch files) followed immediately by high-frequency point lookups (retrieving long-term memory, session state).
- **Why SABLE**:
  - Sub-15 microsecond latency with >14,000 point-lookups/sec keeps agent loops bottleneck-free.
  - Transparent cold-tier archiving moves older conversational sessions to cold compressed segments while maintaining instant query fallback.
  - Autonomous index lifecycle creates indexes for frequent memory queries and retires them when ingestion dominates to prevent write amplification.

### 2. Edge Computing, Robotics & Autonomous IoT
- **The Challenge**: Drones, robotics platforms, and industrial edge gateways have no remote DBA and operate under variable battery, CPU, and network budgets.
- **Why SABLE**:
  - Zero-configuration, zero-dependency embedded deployment.
  - **Contextual LinUCB Bandit** selects optimal join and query execution strategies adapted to current input sizes and resource constraints.
  - Cryptographic checksum verification (`SHA-256`) on every archived segment detects data corruption on edge flash storage.

### 3. Fluctuating Microservices & Autonomous HTAP
- **The Challenge**: Microservices often shift between transactional spikes (e.g. flash sales, point lookups) and analytical batch processing (e.g. reporting, table scans).
- **Why SABLE**:
  - Detects workload phase oscillations automatically.
  - Uses statistical hypothesis testing ($t$-statistic and $p$-value validation) in a shadow executor to prove index benefits before altering physical design.
  - Full bidirectional rollback history enables reverting changes if query patterns invert.

### 4. Telemetry Gateways & Audit Event Streams
- **The Challenge**: Logging and telemetry pipelines require high concurrent write ingestion (5,000–10,000 ops/sec) combined with fast lookups.
- **Why SABLE**:
  - Thread-safe multithreaded WAL ingestion with memoized plan and regex column caches.
  - Fast-path telemetry serialization and bounded memory rings.

---

## Benchmark Performance

Stress tested on Windows with multi-threaded concurrent workloads and rapid phase oscillations:

| Metric | Result |
| :--- | :--- |
| **Concurrent Ingestion Throughput** | **7,454+ ops/sec** |
| **Point Lookups** | **14,035 lookups/sec** (0.57s for 8,000 queries) |
| **p50 Latency** | **10.0 – 11.0 µs** |
| **p95 Latency** | **52.0 – 55.0 µs** |
| **Aggregations** | **0.011s** (200 queries) |
| **Test Suite Execution** | **0.55s** (33 tests) |

---

## What SABLE Includes

- **Durable Local Storage**: SQLite WAL mode, memory temp store, 64 MB page cache, and transaction rollbacks.
- **Learned Cost Model & Adaptive Planner**: Online multivariate regression with versioned plan memoization.
- **Contextual LinUCB Bandit**: Closed-form 3D ridge regression for adaptive join algorithm selection.
- **Statistical Shadow Executor**: Two-sample Welch $t$-test calculating $t$-statistics and $p$-values to prevent regressions.
- **Storage Tier Archiving**: Automatic archival of cold records to chunked segments with transparent query fallback.
- **Bidirectional Index Lifecycle**: Proposes index creation under read spikes and index retirement under write spikes with rollback tracking.
- **Full CLI & Telemetry Export**: Real-time inspection, journal auditing, and JSON telemetry dumps.

---

## Quickstart

```python
from sable import SableDB

db = SableDB("agent_memory.db", epoch_size=50)

db.put("session:101", {"user": "alice", "action": "search", "score": 9.5})
record = db.get("session:101")
print(record)

db.adaptive.enable_policy("linucb")

rows = db.execute("SELECT key, value FROM sable_records WHERE key LIKE ?", ("session:%",))
print(f"Fetched {len(rows)} rows")

archived = db.archive_cold_records(limit=10)
print(f"Archived {archived} cold records to segment store")

print("Engine Status:", db.adaptive.status())
db.close()
```

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
