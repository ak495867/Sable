# SABLE research-grade features

SABLE now includes executable research subsystems for the remaining v1 research directions. They are exposed as explicit Python APIs and are reported in `db.adaptive.status()["research"]`.

| Area | Implementation | Status |
|---|---|---|
| Storage | `SegmentStore`, `RowStore`, `ColumnStore`, `HybridStore` | Durable appendable row/column segments with migration |
| SIMD/vectorization | `VectorizedExecutor` | NumPy vector execution with typed comparisons and Python fallback |
| Physical joins | Nested-loop, hash, and merge join | Functional and test-covered |
| Compression | None, zlib, delta, and run-length encoding | Functional round-trip codecs |
| Shadow execution | Baseline/candidate timing and result comparison | Functional controlled comparison primitive |
| Bandits | UCB1 multi-armed bandit | Functional online selection/update baseline |
| Reinforcement learning | Tabular Q-learning | Functional bounded-state learning baseline |

## Usage

```python
from sable import ColumnStore, JoinExecutor, UCB1Bandit, CompressionCodec

store = ColumnStore([{"id": 1, "value": 10}])
joined = JoinExecutor.execute(left_rows, right_rows, "id", "hash")
codec = CompressionCodec.choose([1, 1, 2, 2], access_frequency=0.1)
arm = UCB1Bandit(["hash", "merge"])
```

The adaptive engine owns a join bandit and a Q-learning policy object. `enable_policy("heuristic")`, `enable_policy("bandit")`, and `enable_policy("q_learning")` select the policy explicitly. `choose_join_algorithm()` selects a physical join, and `observe_join()` feeds measured latency back to both learners. The default planner remains deterministic. This separation permits controlled experiments without silently changing production behavior.

The vector executor uses NumPy on this environment and reports its backend. It retains a chunked Python fallback for portability. Hardware-level AVX2 or AVX-512 usage is delegated to the installed NumPy build rather than falsely hand-labeling a Python loop as SIMD.

Run the feature benchmark with:

```bash
python3 benchmarks/research_features.py
```

Run the long fluctuating-workload comparison with:

```bash
python3 benchmarks/policy_simulation.py --phases 240 --episodes 500 --seed 20260911
```

The simulation measures nested-loop, hash, and merge join costs on each workload profile, replays the same phase sequence to UCB1 and Q-learning, includes a policy-switch cost, and reports cumulative regret against a per-phase oracle.
