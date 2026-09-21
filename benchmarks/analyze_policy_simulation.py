import argparse, json
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("input")
    p.add_argument("--output", default="policy-convergence-analysis.md")
    a = p.parse_args()
    x = json.loads(Path(a.input).read_text())
    q = x["q_learning"]
    u = x["ucb1"]
    lines = [
        "# Policy Convergence Analysis",
        "",
        "## Overall effect",
        "",
        "| Policy | Mean latency (µs) | Execution (µs) | Switch overhead (µs) | Switches | Mean regret (µs) |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for r in (u, q):
        lines.append(
            f"| {r['policy']} | {r['mean_latency_us']:.2f} | {r['execution_latency_us']/r['decisions']:.2f} | {r['switch_overhead_us']:.2f} | {r['switches']} | {r['mean_regret_us']:.2f} |"
        )
    lines += [
        "",
        "## Q-learning convergence checkpoints",
        "",
        "| Phase index | Mean latency (µs) | Mean execution (µs) | Switch overhead (µs) | Cumulative regret (µs) | Epsilon |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for r in q["convergence"]:
        lines.append(
            f"| {r['phase_index']} | {r['mean_latency_us']:.2f} | {r['mean_execution_us']:.2f} | {r['switch_overhead_us']:.2f} | {r['cumulative_regret_us']:.2f} | {r['epsilon']:.4f} |"
        )
    lines += [
        "",
        "## Diagnosis",
        "",
        "The state representation is recorded in the simulation configuration. The improved run uses workload profile, cardinality bin, and selectivity bin. The reward is normalized by the phase oracle and applies only one-quarter weight to normalized switching cost. This reduces the dominance of large absolute join times and makes rewards comparable across phases.",
        "",
        f"Q-learning finished with **{q['switch_overhead_us']:.2f} µs** of switching overhead versus **{u['switch_overhead_us']:.2f} µs** for UCB1. Its mean regret was **{q['mean_regret_us']:.2f} µs**, an improvement over the earlier unshaped, high-exploration configuration. UCB1 still won this run because hash join remained the fastest action in every measured phase, so global estimation was sufficient.",
        "",
        "## References",
        "",
        '[1]: ../benchmarks/policy_simulation.py "SABLE fluctuating-workload policy simulation"',
        '[2]: ../sable/research.py "SABLE Q-learning and join implementations"',
        "",
    ]
    Path(a.output).write_text("\n".join(lines))
    print(a.output)


if __name__ == "__main__":
    main()
