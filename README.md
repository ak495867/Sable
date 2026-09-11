# SABLE v1.0

> A self-optimizing database engine that adapts its execution and storage strategies to observed workload behavior.

SABLE v1.0 is a runnable single-node research engine. It uses SQLite as a durable transactional storage kernel and adds the SABLE feedback loop around it: **observe → measure → model → propose → validate/commit → adapt**.

## What v1.0 includes

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

This is intentionally an **experimental v1.0**, not a claim to replace PostgreSQL, DuckDB, or SQLite. The research goal is to make physical decisions adaptive and measurable.

## Quickstart

```bash
cd sable
python3 -m venv .venv
. .venv/bin/activate
pip install -e .
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
Plan choice + fingerprint/cache
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

The v1 policy engine is deterministic and empirical. It compares observed plan latency when choosing among safe candidates, but it does not pretend to use ML. Later bandit and reinforcement-learning policies can implement the same `choose_plan`, `record`, and proposal interfaces.

## Safety model

SABLE never silently mutates physical design from a heuristic observation. Adaptations are represented as proposals with a reason, details, and epoch. A caller must explicitly approve or reject a proposal. This is the v1 implementation of `proposal → estimate → validate → commit`.

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

## Deliberate v1.0 boundaries

The full research vision includes native row/column storage, SIMD batches, multiple join algorithms, physically adaptive compression, buffer-policy experiments, shadow execution, bandits, RL, and distributed operation. Those are not falsely labeled complete here. v1.0 establishes stable interfaces, policy proposals for these physical dimensions, and a real end-to-end adaptive loop on a conservative transactional kernel.

## License

MIT. See `LICENSE`.
