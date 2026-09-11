# Policy Convergence Analysis

## Overall effect

| Policy | Mean latency (µs) | Execution (µs) | Switch overhead (µs) | Switches | Mean regret (µs) |
|---|---:|---:|---:|---:|---:|
| ucb1 | 19851.04 | 19851.03 | 1900.00 | 95 | 21.40 |
| q_learning | 19873.45 | 19873.33 | 15500.00 | 775 | 43.81 |

## Q-learning convergence checkpoints

| Phase index | Mean latency (µs) | Mean execution (µs) | Switch overhead (µs) | Cumulative regret (µs) | Epsilon |
|---:|---:|---:|---:|---:|---:|
| 0 | 556.32 | 553.80 | 1260.00 | 181375.03 | 0.0312 |
| 12 | 23457.36 | 23455.74 | 10540.00 | 4383252.47 | 0.0015 |
| 24 | 16244.90 | 16243.86 | 13060.00 | 4488486.97 | 0.0010 |
| 36 | 21639.54 | 21638.83 | 13180.00 | 4530316.91 | 0.0010 |
| 48 | 22402.25 | 22401.71 | 13380.00 | 4600128.97 | 0.0010 |
| 60 | 19653.37 | 19652.93 | 13500.00 | 4635316.39 | 0.0010 |
| 72 | 21818.95 | 21818.58 | 13660.00 | 4685615.95 | 0.0010 |
| 84 | 19922.51 | 19922.18 | 13780.00 | 4686642.56 | 0.0010 |
| 96 | 19511.93 | 19511.65 | 13940.00 | 4790812.49 | 0.0010 |
| 108 | 17391.41 | 17391.15 | 14180.00 | 4791643.99 | 0.0010 |
| 120 | 18121.33 | 18121.09 | 14260.00 | 4860935.39 | 0.0010 |
| 132 | 17258.05 | 17257.83 | 14500.00 | 4928789.36 | 0.0010 |
| 144 | 18546.79 | 18546.59 | 14540.00 | 4949903.73 | 0.0010 |
| 156 | 17776.96 | 17776.77 | 14660.00 | 5009337.42 | 0.0010 |
| 168 | 17695.14 | 17694.96 | 14740.00 | 5030514.03 | 0.0010 |
| 180 | 17627.84 | 17627.68 | 14940.00 | 5131758.87 | 0.0010 |
| 192 | 17558.64 | 17558.49 | 15060.00 | 5131985.01 | 0.0010 |
| 204 | 17499.27 | 17499.12 | 15060.00 | 5131985.01 | 0.0010 |
| 216 | 17449.22 | 17449.08 | 15140.00 | 5132450.65 | 0.0010 |
| 228 | 18254.74 | 18254.60 | 15300.00 | 5167361.95 | 0.0010 |

## Diagnosis

The state representation is recorded in the simulation configuration. The improved run uses workload profile, cardinality bin, and selectivity bin. The reward is normalized by the phase oracle and applies only one-quarter weight to normalized switching cost. This reduces the dominance of large absolute join times and makes rewards comparable across phases.

Q-learning finished with **15500.00 µs** of switching overhead versus **1900.00 µs** for UCB1. Its mean regret was **43.81 µs**, an improvement over the earlier unshaped, high-exploration configuration. UCB1 still won this run because hash join remained the fastest action in every measured phase, so global estimation was sufficient.

## References

[1]: ../benchmarks/policy_simulation.py "SABLE fluctuating-workload policy simulation"
[2]: ../sable/research.py "SABLE Q-learning and join implementations"
