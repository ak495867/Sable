# SABLE roadmap

## v1.0 — complete in this repository

The release provides a durable local engine wrapper, SQL execution, query fingerprints, telemetry, workload ratios, plan caching, epoch-based deterministic heuristics, proposal-based index adaptation, explicit commit, CLI, tests, and a changing-workload benchmark.

## v1.1 — physical design hardening

Add typed schema metadata, column statistics, index benefit estimates, proposal validation against shadow samples, rollback records, and an append-only adaptation journal.

## v2.0 — learning policies

Add a multi-armed bandit policy with bounded exploration, confidence intervals, model decay, and direct comparison against the deterministic heuristic policy and an oracle replay runner.

## v3.0 — adaptive physical storage

Introduce native row/column/hybrid segments, hot/warm/cold classification, compression selection, vectorized batches, and buffer/prefetch policy experiments. Each change must preserve the safety rails and expose telemetry.
