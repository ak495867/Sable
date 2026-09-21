"""Autonomous subsystems for SABLE v3.0.

Provides the four advanced autonomous features requested for v3.0:

* Adaptive Storage Layer - hot/warm/cold tier migration with compression
* Query Planner - learned cost models for plan selection
* Execution Engine - adaptive batch sizes, parallelism, and memory limits
* Autonomous Physical Design - continuous validation and auto-apply of
  physical design decisions
"""

from __future__ import annotations
import math, statistics, time
from dataclasses import dataclass, field
from typing import Any, Iterable, Optional


@dataclass
class AccessTracker:
    """Tracks per-table and per-column access patterns with bounded windows."""

    def __init__(self, window: int = 10000):
        self.window = max(1, window)
        self.table_access: dict[str, list[dict[str, Any]]] = {}
        self.column_access: dict[str, list[dict[str, Any]]] = {}
        self.tier_counts: dict[str, int] = {"hot": 0, "warm": 0, "cold": 0}

    def record(
        self,
        table: str,
        columns: Iterable[str] = (),
        operation: str = "query",
        access_freq: float = 1.0,
        timestamp: float | None = None,
    ):
        ts = time.time() if timestamp is None else timestamp
        entry = {
            "table": table,
            "columns": tuple(columns),
            "operation": operation,
            "access_freq": access_freq,
            "timestamp": ts,
        }
        self.table_access.setdefault(table, []).append(entry)
        self.table_access[table] = self.table_access[table][-self.window :]
        for col in columns:
            self.column_access.setdefault(col, []).append(dict(entry, column=col))
            self.column_access[col] = self.column_access[col][-self.window :]
        if access_freq > 0.5:
            self.tier_counts["hot"] += 1
        elif access_freq > 0.1:
            self.tier_counts["warm"] += 1
        else:
            self.tier_counts["cold"] += 1

    def access_frequency(self, table: str, column: str | None = None) -> float:
        entries = (
            self.column_access.get(column, [])
            if column
            else self.table_access.get(table, [])
        )
        if not entries:
            return 0.0
        recent = entries[-min(len(entries), 100) :]
        return sum(e.get("access_freq", 0.0) for e in recent) / max(len(recent), 1)

    def hot_columns(self, table: str, top_n: int = 3) -> list[str]:
        cols = [
            c
            for c in self.column_access
            if any(e["table"] == table for e in self.column_access[c])
        ]
        return sorted(
            cols, key=lambda c: self.access_frequency(table, c), reverse=True
        )[:top_n]

    def tier_for_access(self, access_freq: float) -> str:
        return "hot" if access_freq > 0.5 else ("warm" if access_freq > 0.1 else "cold")

    def status(self) -> dict[str, Any]:
        return {
            "window": self.window,
            "tier_counts": self.tier_counts,
            "tracked_tables": len(self.table_access),
            "tracked_columns": len(self.column_access),
        }


@dataclass
class AdaptiveStorageManager:
    """Hot/warm/cold tier migration with compression selection."""

    def __init__(
        self,
        classifier: Any = None,
        hot_columns: Iterable[str] = (),
        compression: str = "zlib",
    ):
        self.classifier = classifier
        self.hot_columns = set(hot_columns)
        self.compression = compression
        self.tier_stats: dict[str, dict[str, Any]] = {}

    def classify(
        self, access_freqs: list[float], rows: list[dict[str, Any]]
    ) -> dict[str, Any]:
        if self.classifier is None:
            from .research import HotWarmColdStore

            self.classifier = HotWarmColdStore(segment_rows=1024)
        result = self.classifier.reclassify(access_freqs, rows)
        self.tier_stats = {
            "hot": result.get("hot", 0),
            "warm": result.get("warm", 0),
            "cold": result.get("cold", 0),
        }
        return result

    def migrate(self, tier: str, rows: list[dict[str, Any]]):
        if tier not in ("hot", "warm", "cold"):
            raise ValueError("tier must be hot, warm, or cold")
        if self.classifier is None:
            from .research import HotWarmColdStore

            self.classifier = HotWarmColdStore(segment_rows=1024)
        if tier == "hot":
            self.classifier.hot_store.append(rows)
        elif tier == "warm":
            self.classifier.warm_store.append(rows)
        else:
            self.classifier.cold_store.append(rows)
        self.tier_stats[tier] = self.tier_stats.get(tier, 0) + len(rows)
        return self.tier_stats

    def promote(self, tier: str, rows: list[dict[str, Any]]):
        if tier not in ("hot", "warm"):
            raise ValueError("tier must be hot or warm")
        self.migrate(tier, rows)
        return self.tier_stats

    def demote(self, tier: str, rows: list[dict[str, Any]]):
        if tier not in ("warm", "cold"):
            raise ValueError("tier must be warm or cold")
        self.migrate(tier, rows)
        return self.tier_stats

    def recommend_compression(self, values: list[Any]) -> str:
        if not values:
            return "none"
        try:
            from .research import CompressionCodec

            return CompressionCodec.choose(values, access_frequency=0.8, cpu_budget=0.5)
        except Exception:
            return self.compression

    def status(self) -> dict[str, Any]:
        mode = self.classifier.mode if self.classifier else "disabled"
        return {
            "mode": mode,
            "hot_columns": sorted(self.hot_columns),
            "compression": self.compression,
            "tier_stats": self.tier_stats,
        }


@dataclass
class CostModel:
    """Learned cost model for estimating query latency."""

    def __init__(self, window: int = 1000):
        self.window = max(1, window)
        self.samples: list[dict[str, Any]] = []
        self.parameters: dict[str, float] = {
            "base": 50.0,
            "per_row": 0.01,
            "index_bonus": -0.05,
        }

    def observe(
        self,
        rows_scanned: int,
        rows_returned: int,
        latency_us: int,
        index_used: bool = False,
        join: bool = False,
        aggregate: bool = False,
    ):
        self.samples.append(
            {
                "rows_scanned": rows_scanned,
                "rows_returned": rows_returned,
                "latency_us": latency_us,
                "index_used": index_used,
                "join": join,
                "aggregate": aggregate,
            }
        )
        self.samples = self.samples[-self.window :]
        self._fit()

    def _fit(self):
        if len(self.samples) < 3:
            return
        n = len(self.samples)
        sx = sum(s["rows_scanned"] for s in self.samples)
        sy = sum(s["latency_us"] for s in self.samples)
        sxx = sum(s["rows_scanned"] ** 2 for s in self.samples)
        sxy = sum(s["rows_scanned"] * s["latency_us"] for s in self.samples)
        denom = n * sxx - sx * sx
        if denom != 0:
            slope = (n * sxy - sx * sy) / denom
            self.parameters["per_row"] = max(0.001, min(slope, 1.0))
        self.parameters["base"] = max(1.0, sy / n - self.parameters["per_row"] * sx / n)

    def estimate(
        self,
        rows_scanned: int,
        index_used: bool = False,
        join: bool = False,
        aggregate: bool = False,
    ) -> float:
        cost = self.parameters["base"] + self.parameters["per_row"] * rows_scanned
        if index_used:
            cost *= 0.2
        if join:
            cost *= 3.0
        if aggregate:
            cost *= 1.5
        return max(1.0, cost)

    def status(self) -> dict[str, Any]:
        return {"samples": len(self.samples), "parameters": self.parameters}


@dataclass
class AdaptivePlanner:
    """Plan selection driven by a learned cost model."""

    def __init__(self, cost_model: CostModel | None = None):
        self.cost_model = cost_model or CostModel()
        self.candidate_plans: dict[str, list[str]] = {}
        self.last_plan = "scan"

    def select(
        self,
        sql: str,
        params: Iterable[Any] = (),
        estimated_rows: int = 0,
        indexes: Iterable[str] = (),
        candidates: Iterable[str] = ("scan",),
    ):
        low = sql.lower()
        has_join = " join " in low
        has_aggregate = any(
            x in low for x in ("group by", "avg(", "count(", "sum(", "min(", "max(")
        )
        has_where = " where " in low and "=" in low
        plans = list(candidates) or ["scan"]
        if has_join:
            plans = ["join", "scan"]
        elif has_aggregate:
            plans = ["aggregate", "scan"]
        elif has_where:
            plans = ["point_lookup", "index_lookup", "scan"]
        self.candidate_plans[sql] = plans
        if estimated_rows <= 0:
            estimated_rows = 0
        scored = []
        for plan in plans:
            cost = 0.0
            if plan == "scan":
                cost = self.cost_model.estimate(estimated_rows)
            elif plan == "point_lookup":
                cost = self.cost_model.estimate(estimated_rows, index_used=False) * 0.5
            elif plan == "index_lookup":
                cost = self.cost_model.estimate(estimated_rows, index_used=True)
            elif plan == "join":
                cost = self.cost_model.estimate(estimated_rows, join=True)
            elif plan == "aggregate":
                cost = self.cost_model.estimate(estimated_rows, aggregate=True)
            elif plan == "write":
                cost = self.cost_model.estimate(estimated_rows)
            else:
                cost = self.cost_model.estimate(estimated_rows)
            scored.append((cost, plan))
        self.last_plan = min(scored)[1]
        return self.last_plan, tuple(plans), False

    def observe(
        self,
        plan: str,
        rows_scanned: int,
        latency_us: int,
        index_used: bool = False,
        join: bool = False,
        aggregate: bool = False,
    ):
        self.cost_model.observe(
            rows_scanned, rows_scanned, latency_us, index_used, join, aggregate
        )

    def status(self) -> dict[str, Any]:
        return {"last_plan": self.last_plan, "cost_model": self.cost_model.status()}


@dataclass
class AdaptiveExecutionEngine:
    """Adaptive execution engine with batch sizes, parallelism, and memory."""

    def __init__(
        self,
        batch_size: int = 65536,
        max_parallelism: int = 4,
        memory_budget_mb: int = 256,
    ):
        self.batch_size = max(1, batch_size)
        self.max_parallelism = max(1, max_parallelism)
        self.memory_budget_mb = max(1, memory_budget_mb)
        self.history: list[dict[str, Any]] = []
        self.current_parallelism = 1

    def choose_batch_size(
        self, rows: int, latency_us: int = 0, throughput: float = 1.0
    ) -> int:
        if latency_us > 100000:
            self.batch_size = min(1048576, self.batch_size * 2)
        elif latency_us < 10000:
            self.batch_size = max(4096, self.batch_size // 2)
        self.batch_size = min(max(self.batch_size, 4096), 1048576)
        self.history.append(
            {"batch_size": self.batch_size, "rows": rows, "latency_us": latency_us}
        )
        self.history = self.history[-1000:]
        return self.batch_size

    def choose_parallelism(self, rows: int, cores: int | None = None) -> int:
        cores = cores or max(1, self.max_parallelism)
        self.current_parallelism = min(
            cores, max(1, int(math.ceil(rows / max(self.batch_size, 1))))
        )
        return self.current_parallelism

    def choose_memory(self, rows: int, bytes_per_row: int = 64) -> int:
        needed = rows * bytes_per_row
        return min(needed, self.memory_budget_mb * 1024 * 1024)

    def status(self) -> dict[str, Any]:
        return {
            "batch_size": self.batch_size,
            "max_parallelism": self.max_parallelism,
            "current_parallelism": self.current_parallelism,
            "memory_budget_mb": self.memory_budget_mb,
            "history_len": len(self.history),
        }


@dataclass
class AutonomousPhysicalDesigner:
    """Continuous validation and auto-apply of physical design decisions."""

    def __init__(
        self,
        validator: Any = None,
        auto_apply: bool = True,
        validation_repetitions: int = 3,
    ):
        self.validator = validator
        self.auto_apply = auto_apply
        self.validation_repetitions = max(1, validation_repetitions)
        self.proposals: list[dict[str, Any]] = []
        self.validation_results: list[dict[str, Any]] = []

    def propose(
        self, typ: str, details: dict[str, Any], reason: str = ""
    ) -> dict[str, Any]:
        proposal = {
            "type": typ,
            "details": details,
            "reason": reason,
            "status": "proposed",
            "timestamp": time.time(),
        }
        self.proposals.append(proposal)
        return proposal

    def validate(
        self,
        proposal: dict[str, Any],
        baseline: Any,
        candidate: Any,
        repetitions: int | None = None,
    ) -> dict[str, Any]:
        reps = repetitions or self.validation_repetitions
        if self.validator is not None:
            result = self.validator.compare(baseline, candidate, repetitions=reps)
            winner = getattr(result, "winner", None)
        else:
            winner = "candidate" if self.auto_apply else "baseline"
        validated = winner == "candidate"
        result = {
            "proposal_type": proposal["type"],
            "validated": validated,
            "winner": winner,
            "repetitions": reps,
            "timestamp": time.time(),
        }
        self.validation_results.append(result)
        proposal["validation"] = result
        return result

    def apply(self, proposal: dict[str, Any], apply_fn: Any):
        if proposal["status"] != "proposed":
            return False
        if not self.auto_apply:
            return False
        apply_fn(proposal["details"])
        proposal["status"] = "applied"
        return True

    def auto_apply_validated(self, apply_fn: Any):
        applied = 0
        for p in list(self.proposals):
            if p["status"] == "proposed" and p.get("validation", {}).get("validated"):
                if self.apply(p, apply_fn):
                    applied += 1
        return applied

    def status(self) -> dict[str, Any]:
        return {
            "auto_apply": self.auto_apply,
            "validation_repetitions": self.validation_repetitions,
            "proposals": len(self.proposals),
            "validation_results": len(self.validation_results),
        }
