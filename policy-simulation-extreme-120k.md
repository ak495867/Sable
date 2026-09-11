# SABLE UCB1 versus Q-learning Simulation

The simulation replayed **120,000 decisions** across **240 fluctuating phases**. It added sparse-selectivity and many-to-many phases to force stronger workload variation. Candidate costs were measured from SABLE's physical join implementations, and the oracle selected the fastest measured algorithm per phase.

## Summary

| Policy | Mean latency (µs) | Execution (µs) | Switch overhead (µs) | Mean regret (µs) | Switches |
|---|---:|---:|---:|---:|---:|
| ucb1 | 19851.04 | 19851.03 | 1900.00 | 21.40 | 95 |
| q_learning | 19873.45 | 19873.33 | 15500.00 | 43.81 | 775 |

**Lower cumulative regret:** `ucb1`.

## Measured phase costs

| Profile | Selectivity | Nested loop (µs) | Hash (µs) | Merge (µs) |
|---|---:|---:|---:|---:|
| tiny_unsorted | 0.020833 | 86.93 | 25.25 | 47.48 |
| large_unsorted | 0.001190 | 24993.41 | 516.28 | 999.21 |
| large_sorted | 0.001190 | 21560.69 | 486.32 | 679.14 |
| sparse_selective | 0.000000 | 29135.38 | 193.56 | 424.43 |
| many_to_many | 0.249522 | 132532.49 | 97926.79 | 118331.50 |

## Convergence diagnosis

The Q-learning state is `(profile, size_bin, selectivity_bin)`. Its reward is normalized by the phase oracle and includes one-quarter of normalized switch cost. This prevents large absolute join times from overwhelming the learning signal. The JSON output contains periodic convergence snapshots with action counts, epsilon, execution cost, switching overhead, and cumulative regret.

UCB1 maintains one global action estimate. Q-learning receives contextual state and therefore can learn different join decisions for different selectivity regimes. The comparison should be interpreted together with the per-profile action counts and convergence trace rather than total latency alone.

## Reproducibility

Run: `python3 benchmarks/policy_simulation.py --phases 240 --episodes 500 --seed 20260911`

## References

[1]: ../sable/research.py "SABLE physical joins and learning policies"
[2]: ../sable/core.py "SABLE adaptive engine integration"
