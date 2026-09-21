"""Long measured UCB1 versus Q-learning simulation for SABLE join policies."""

from __future__ import annotations
import argparse
import json
import random
import statistics
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))
from sable import JoinExecutor, TabularQLearner, UCB1Bandit

ARMS = ("nested_loop", "hash", "merge")
PROFILES = (
    "tiny_unsorted",
    "large_unsorted",
    "large_sorted",
    "sparse_selective",
    "many_to_many",
)


def build_workload(profile: str, seed: int):
    rng = random.Random(seed)
    if profile == "tiny_unsorted":
        left_n, right_n = 24, 48
        left = [{"id": i, "x": i * 2} for i in range(left_n)]
        right = [{"id": i, "y": i * 3} for i in range(right_n)]
    elif profile == "large_unsorted":
        left_n, right_n = 420, 840
        left = [{"id": i, "x": i * 2} for i in range(left_n)]
        right = [{"id": i, "y": i * 3} for i in range(right_n)]
    elif profile == "large_sorted":
        left_n, right_n = 420, 840
        left = [{"id": i, "x": i * 2} for i in range(left_n)]
        right = [{"id": i, "y": i * 3} for i in range(right_n)]
    elif profile == "sparse_selective":
        left_n, right_n = 700, 700
        left = [{"id": i * 10, "x": i} for i in range(left_n)]
        right = [{"id": i * 10 + 1, "y": i} for i in range(right_n)]
    else:  # many-to-many: extreme duplicate-key selectivity
        left_n, right_n = 700, 700
        left = [{"id": rng.randrange(4), "x": i} for i in range(left_n)]
        right = [{"id": rng.randrange(4), "y": i} for i in range(right_n)]
    sorted_inputs = profile == "large_sorted"
    if profile in ("tiny_unsorted", "large_unsorted"):
        rng.shuffle(left)
        rng.shuffle(right)
    if sorted_inputs:
        left = sorted(left, key=lambda r: r["id"])
        right = sorted(right, key=lambda r: r["id"])
    matches = sum(1 for a in left for b in right if a["id"] == b["id"])
    selectivity = matches / max(1, len(left) * len(right))
    return (
        left,
        right,
        sorted_inputs,
        {
            "left_rows": len(left),
            "right_rows": len(right),
            "matches": matches,
            "selectivity": selectivity,
        },
    )


def measure_costs(left, right, sorted_inputs):
    costs = {}
    outputs = {}
    for arm in ARMS:
        samples = []
        for _ in range(3):
            t = time.perf_counter_ns()
            result = JoinExecutor.execute(
                left, right, "id", arm, presorted=sorted_inputs
            )
            samples.append((time.perf_counter_ns() - t) / 1000)
        costs[arm] = statistics.median(samples)
        outputs[arm] = len(result)
    if len(set(outputs.values())) != 1:
        raise AssertionError(f"join result mismatch: {outputs}")
    return costs


def state_for(profile, meta):
    # Context combines workload identity, logarithmic cardinality, and selectivity class.
    sel = meta["selectivity"]
    sel_bin = (
        "zero"
        if sel == 0
        else "low" if sel < 0.01 else "medium" if sel < 0.2 else "high"
    )
    size_bin = "small" if meta["left_rows"] < 100 else "large"
    return (profile, size_bin, sel_bin)


def run_policy(
    name, policy, phase_states, phase_costs, phase_meta, episodes, switch_cost_us
):
    total = oracle_total = execution_total = switch_total = regret = 0.0
    switches = 0
    previous = None
    records = []
    convergence = []
    counts = Counter()
    by_phase = defaultdict(
        lambda: {
            "n": 0,
            "latency_us": 0.0,
            "execution_us": 0.0,
            "switch_us": 0.0,
            "regret_us": 0.0,
            "oracle_us": 0.0,
            "actions": Counter(),
        }
    )
    for phase_index, state_key in enumerate(phase_states):
        profile = state_key[0]
        costs = phase_costs[profile]
        oracle = min(costs.values())
        for episode in range(episodes):
            if name == "q_learning":
                arm = policy.choose(state_key, explore=True)
                policy.epsilon = max(0.001, policy.epsilon * 0.9995)
            else:
                arm = policy.select()
            execution = costs[arm]
            switched = previous is not None and arm != previous
            switch_penalty = switch_cost_us if switched else 0.0
            latency = execution + switch_penalty
            # Shaped reward removes absolute scale differences and isolates switching overhead.
            reward = -(execution / max(oracle, 1.0)) - (
                (switch_penalty / max(oracle, 1.0)) * 0.25
            )
            if name == "q_learning":
                policy.update(state_key, arm, reward, state_key)
            else:
                policy.update(arm, reward)
            execution_total += execution
            switch_total += switch_penalty
            total += latency
            oracle_total += oracle
            regret += latency - oracle
            switches += int(switched)
            previous = arm
            counts[arm] += 1
            bucket = by_phase[profile]
            bucket["n"] += 1
            bucket["latency_us"] += latency
            bucket["execution_us"] += execution
            bucket["switch_us"] += switch_penalty
            bucket["regret_us"] += latency - oracle
            bucket["oracle_us"] += oracle
            bucket["actions"][arm] += 1
            if (
                len(records) < 1000
                or (phase_index * episodes + episode) % max(1, episodes // 10) == 0
            ):
                records.append(
                    {
                        "phase_index": phase_index,
                        "profile": profile,
                        "state": state_key,
                        "episode": episode,
                        "arm": arm,
                        "execution_us": execution,
                        "switch_us": switch_penalty,
                        "latency_us": latency,
                        "oracle_us": oracle,
                        "regret_us": latency - oracle,
                        "epsilon": getattr(policy, "epsilon", None),
                    }
                )
        if phase_index % max(1, len(phase_states) // 20) == 0:
            convergence.append(
                {
                    "phase_index": phase_index,
                    "profile": profile,
                    "decisions": (phase_index + 1) * episodes,
                    "mean_latency_us": total / ((phase_index + 1) * episodes),
                    "mean_execution_us": execution_total
                    / ((phase_index + 1) * episodes),
                    "switch_overhead_us": switch_total,
                    "cumulative_regret_us": regret,
                    "switches": switches,
                    "epsilon": getattr(policy, "epsilon", None),
                    "action_counts": dict(counts),
                }
            )
    return {
        "policy": name,
        "decisions": len(phase_states) * episodes,
        "total_latency_us": total,
        "execution_latency_us": execution_total,
        "switch_overhead_us": switch_total,
        "oracle_latency_us": oracle_total,
        "cumulative_regret_us": regret,
        "mean_latency_us": total / (len(phase_states) * episodes),
        "mean_regret_us": regret / (len(phase_states) * episodes),
        "switches": switches,
        "arm_counts": dict(counts),
        "by_profile": {
            k: {
                **v,
                "actions": dict(v["actions"]),
                "mean_latency_us": v["latency_us"] / v["n"],
                "mean_execution_us": v["execution_us"] / v["n"],
                "mean_switch_us": v["switch_us"] / v["n"],
                "mean_regret_us": v["regret_us"] / v["n"],
            }
            for k, v in by_phase.items()
        },
        "convergence": convergence,
        "trace_sample": records,
        "policy_state": policy.status(),
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--phases", type=int, default=240)
    p.add_argument("--episodes", type=int, default=500)
    p.add_argument("--seed", type=int, default=20260911)
    p.add_argument("--switch-cost-us", type=float, default=20.0)
    p.add_argument("--output", default="policy-simulation.json")
    p.add_argument("--report", default="policy-simulation.md")
    a = p.parse_args()
    rng = random.Random(a.seed)
    phase_states = [PROFILES[i % len(PROFILES)] for i in range(a.phases)]
    rng.shuffle(phase_states)
    phase_costs = {}
    phase_meta = {}
    measurement = {}
    for i, profile in enumerate(PROFILES):
        left, right, sorted_inputs, meta = build_workload(profile, a.seed + i)
        phase_costs[profile] = measure_costs(left, right, sorted_inputs)
        phase_meta[profile] = meta
        measurement[profile] = {
            "costs_us": phase_costs[profile],
            "metadata": meta,
            "sorted_inputs": sorted_inputs,
        }
    state_sequence = [
        state_for(profile, phase_meta[profile]) for profile in phase_states
    ]
    bandit = UCB1Bandit(ARMS, seed=a.seed)
    q = TabularQLearner(ARMS, alpha=0.15, gamma=0.85, epsilon=0.04, seed=a.seed)
    bandit_result = run_policy(
        "ucb1",
        bandit,
        state_sequence,
        phase_costs,
        phase_meta,
        a.episodes,
        a.switch_cost_us,
    )
    q_result = run_policy(
        "q_learning",
        q,
        state_sequence,
        phase_costs,
        phase_meta,
        a.episodes,
        a.switch_cost_us,
    )
    result = {
        "configuration": {
            "phases": a.phases,
            "episodes_per_phase": a.episodes,
            "decisions": a.phases * a.episodes,
            "seed": a.seed,
            "switch_cost_us": a.switch_cost_us,
            "phase_sequence": phase_states,
            "state_representation": "(profile, size_bin, selectivity_bin)",
            "reward": "-(execution/oracle)-0.25*(switch_penalty/oracle)",
        },
        "measured_phase_costs": measurement,
        "ucb1": bandit_result,
        "q_learning": q_result,
    }
    Path(a.output).write_text(json.dumps(result, indent=2))
    better = (
        "q_learning"
        if q_result["cumulative_regret_us"] < bandit_result["cumulative_regret_us"]
        else "ucb1"
    )
    report = [
        "# SABLE UCB1 versus Q-learning Simulation",
        "",
        f"The simulation replayed **{a.phases*a.episodes:,} decisions** across **{a.phases} fluctuating phases**. It added sparse-selectivity and many-to-many phases to force stronger workload variation. Candidate costs were measured from SABLE's physical join implementations, and the oracle selected the fastest measured algorithm per phase.",
        "",
        "## Summary",
        "",
        "| Policy | Mean latency (µs) | Execution (µs) | Switch overhead (µs) | Mean regret (µs) | Switches |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for r in (bandit_result, q_result):
        report.append(
            f"| {r['policy']} | {r['mean_latency_us']:.2f} | {r['execution_latency_us']/r['decisions']:.2f} | {r['switch_overhead_us']:.2f} | {r['mean_regret_us']:.2f} | {r['switches']} |"
        )
    report += [
        "",
        f"**Lower cumulative regret:** `{better}`.",
        "",
        "## Measured phase costs",
        "",
        "| Profile | Selectivity | Nested loop (µs) | Hash (µs) | Merge (µs) |",
        "|---|---:|---:|---:|---:|",
    ]
    for profile, c in phase_costs.items():
        report.append(
            f"| {profile} | {phase_meta[profile]['selectivity']:.6f} | {c['nested_loop']:.2f} | {c['hash']:.2f} | {c['merge']:.2f} |"
        )
    report += [
        "",
        "## Convergence diagnosis",
        "",
        "The Q-learning state is `(profile, size_bin, selectivity_bin)`. Its reward is normalized by the phase oracle and includes one-quarter of normalized switch cost. This prevents large absolute join times from overwhelming the learning signal. The JSON output contains periodic convergence snapshots with action counts, epsilon, execution cost, switching overhead, and cumulative regret.",
        "",
        "UCB1 maintains one global action estimate. Q-learning receives contextual state and therefore can learn different join decisions for different selectivity regimes. The comparison should be interpreted together with the per-profile action counts and convergence trace rather than total latency alone.",
        "",
        "## Reproducibility",
        "",
        f"Run: `python3 benchmarks/policy_simulation.py --phases {a.phases} --episodes {a.episodes} --seed {a.seed}`",
        "",
        "## References",
        "",
        '[1]: ../sable/research.py "SABLE physical joins and learning policies"',
        '[2]: ../sable/core.py "SABLE adaptive engine integration"',
        "",
    ]
    Path(a.report).write_text("\n".join(report))
    print(
        json.dumps(
            {
                "output": a.output,
                "report": a.report,
                "decisions": a.phases * a.episodes,
                "lower_regret": better,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
