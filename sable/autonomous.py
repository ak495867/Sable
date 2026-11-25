"""Autonomous subsystems for SABLE v3.0.

Provides the four advanced autonomous features requested for v3.0:

* Adaptive Storage Layer - hot/warm/cold tier migration with compression
* Query Planner - learned cost models for plan selection
* Execution Engine - adaptive batch sizes, parallelism, and memory limits
* Autonomous Physical Design - continuous validation and auto-apply of
  physical design decisions
"""

from __future__ import annotations
import math
import time
from dataclasses import dataclass
from typing import Any, Iterable


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
        ta = self.table_access.setdefault(table, [])
        ta.append(entry)
        if len(ta) > self.window + 200:
            self.table_access[table] = ta[-self.window :]
        for col in columns:
            ca = self.column_access.setdefault(col, [])
            ca.append({"table": table, "column": col, "operation": operation, "access_freq": access_freq, "timestamp": ts})
            if len(ca) > self.window + 200:
                self.column_access[col] = ca[-self.window :]
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
    def __init__(self, window: int = 1000):
        self.window = max(1, window)
        self.samples: list[dict[str, Any]] = []
        self._dirty = 0
        self.version = 0
        self.parameters: dict[str, float] = {
            "base": 50.0,
            "per_row": 0.01,
            "index_penalty": 0.2,
            "join_multiplier": 3.0,
            "aggregate_multiplier": 1.5,
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
        if len(self.samples) > self.window + 200:
            self.samples = self.samples[-self.window :]
        self._dirty += 1
        if self._dirty >= 100 or len(self.samples) <= 20:
            self._fit()
            self._dirty = 0

    def _fit(self):
        if len(self.samples) < 3:
            return
        n = len(self.samples)
        sx = 0.0
        sy = 0.0
        sxx = 0.0
        sxy = 0.0
        idx_sum = 0.0
        idx_cnt = 0
        non_idx_sum = 0.0
        non_idx_cnt = 0
        join_sum = 0.0
        join_cnt = 0
        for s in self.samples:
            rx = s["rows_scanned"]
            ly = s["latency_us"]
            sx += rx
            sy += ly
            sxx += rx * rx
            sxy += rx * ly
            if s.get("index_used"):
                idx_sum += ly
                idx_cnt += 1
            elif not s.get("join"):
                non_idx_sum += ly
                non_idx_cnt += 1
            if s.get("join"):
                join_sum += ly
                join_cnt += 1
        denom = n * sxx - sx * sx
        if denom != 0:
            slope = (n * sxy - sx * sy) / denom
            self.parameters["per_row"] = max(0.001, min(slope, 1.0))
        self.parameters["base"] = max(1.0, sy / n - self.parameters["per_row"] * sx / n)
        if idx_cnt > 0 and non_idx_cnt > 0:
            non_idx_mean = non_idx_sum / non_idx_cnt
            if non_idx_mean > 0:
                self.parameters["index_penalty"] = max(0.01, min(0.9, round((idx_sum / idx_cnt) / non_idx_mean, 4)))
        if join_cnt > 0 and non_idx_cnt > 0:
            non_idx_mean = non_idx_sum / non_idx_cnt
            if non_idx_mean > 0:
                self.parameters["join_multiplier"] = max(1.1, min(10.0, round((join_sum / join_cnt) / non_idx_mean, 4)))
        self.version += 1

    def estimate(
        self,
        rows_scanned: int,
        index_used: bool = False,
        join: bool = False,
        aggregate: bool = False,
    ) -> float:
        cost = self.parameters["base"] + self.parameters["per_row"] * rows_scanned
        if index_used:
            cost *= self.parameters.get("index_penalty", 0.2)
        if join:
            cost *= self.parameters.get("join_multiplier", 3.0)
        if aggregate:
            cost *= self.parameters.get("aggregate_multiplier", 1.5)
        return max(1.0, cost)

    def status(self) -> dict[str, Any]:
        if self._dirty > 0:
            self._fit()
            self._dirty = 0
        return {"samples": len(self.samples), "parameters": self.parameters}


@dataclass
class AdaptivePlanner:
    """Plan selection driven by a learned cost model."""

    def __init__(self, cost_model: CostModel | None = None):
        self.cost_model = cost_model or CostModel()
        self.candidate_plans: dict[str, list[str]] = {}
        self.last_plan = "scan"
        self._plan_cache: dict[tuple[str, frozenset[str]], tuple[str, tuple[str, ...], bool]] = {}
        self._fit_version = -1

    def select(
        self,
        sql: str,
        params: Iterable[Any] = (),
        estimated_rows: int = 0,
        indexes: Iterable[str] = (),
        candidates: Iterable[str] = ("scan",),
    ):
        cm_ver = getattr(self.cost_model, "version", 0)
        if cm_ver != self._fit_version:
            self._plan_cache.clear()
            self._fit_version = cm_ver
        cache_key = (sql, frozenset(indexes))
        cached = self._plan_cache.get(cache_key)
        if cached is not None:
            self.last_plan = cached[0]
            return cached
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
            available = set(indexes)
            plans = ["point_lookup", "scan"]
            if "idx_sable_key" in available:
                plans.insert(0, "index_lookup")
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
        res = (self.last_plan, tuple(plans), True)
        if len(self._plan_cache) > 1024:
            self._plan_cache.clear()
        self._plan_cache[cache_key] = res
        return (self.last_plan, tuple(plans), False)

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
        if len(self.history) > 1200:
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
