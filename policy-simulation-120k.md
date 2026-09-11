# SABLE UCB1 versus Q-learning Simulation

The simulation replayed **120,000 decisions** across **240 fluctuating phases**. Candidate costs were measured from SABLE's actual join implementations before policy replay. The oracle selected the fastest measured algorithm for each phase.

## Summary

| Policy | Mean latency (µs) | Mean regret (µs) | Cumulative regret (µs) | Switches |
|---|---:|---:|---:|---:|
| ucb1 | 6139.93 | 0.24 | 28225.68 | 9 |
| q_learning | 6851.94 | 712.25 | 85470128.42 | 18064 |

**Lower cumulative regret:** `ucb1`.

## Measured phase costs

| Profile | Nested loop (µs) | Hash (µs) | Merge (µs) |
|---|---:|---:|---:|
| tiny_unsorted | 87.75 | 24.50 | 46.69 |
| large_unsorted | 24861.68 | 496.32 | 951.13 |
| large_sorted | 21828.27 | 444.71 | 794.87 |
| skewed | 45625.01 | 23593.22 | 26557.45 |

## Interpretation

UCB1 is non-contextual in this experiment. It maintains one global estimate per join algorithm, so it can be disadvantaged when the best algorithm changes with workload phase. Q-learning receives the phase profile as state and can retain different action values for different phases. Regret includes the configured policy-switch cost.

## Reproducibility

Run: `python3 benchmarks/policy_simulation.py --phases 240 --episodes 500 --seed 20260911`

## References

[1]: ../sable/research.py "SABLE physical joins and learning policies"
[2]: ../sable/core.py "SABLE adaptive engine integration"
