import argparse
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("telemetry")
    p.add_argument("--output", default="telemetry-analysis.md")
    a = p.parse_args()
    events = [
        json.loads(line)
        for line in Path(a.telemetry).read_text().splitlines()
        if line.strip()
    ]
    by_plan = defaultdict(list)
    by_phase = defaultdict(list)
    by_epoch = Counter()
    indexes = Counter()
    cache = Counter()
    for e in events:
        by_plan[e["plan"]].append(e["latency_us"])
        by_phase[e["phase"]].append(e["latency_us"])
        by_epoch[e["adaptation_epoch"]] += 1
        indexes[e["index_used"]] += 1
        cache[e["cache_hit"]] += 1

    def stats(vals):
        return {
            "n": len(vals),
            "p50_us": statistics.median(vals),
            "p95_us": (
                statistics.quantiles(vals, n=20)[18] if len(vals) >= 20 else max(vals)
            ),
            "mean_us": round(statistics.mean(vals), 2),
        }

    lines = [
        "# SABLE Telemetry Analysis",
        "",
        f"**Events:** {len(events)}",
        "",
        "## By plan",
        "",
        "| Plan | Events | p50 (µs) | p95 (µs) | Mean (µs) |",
        "|---|---:|---:|---:|---:|",
    ]
    for k, v in sorted(by_plan.items()):
        s = stats(v)
        lines.append(
            f"| {k} | {s['n']} | {s['p50_us']:.1f} | {s['p95_us']:.1f} | {s['mean_us']:.2f} |"
        )
    lines += [
        "",
        "## By detected workload phase",
        "",
        "| Phase | Events | p50 (µs) | p95 (µs) |",
        "|---|---:|---:|---:|",
    ]
    for k, v in sorted(by_phase.items()):
        s = stats(v)
        lines.append(f"| {k} | {s['n']} | {s['p50_us']:.1f} | {s['p95_us']:.1f} |")
    lines += [
        "",
        "## Adaptation and instrumentation",
        "",
        f"- Adaptation epochs observed: **{len(by_epoch)}**",
        f"- Index-used events: **{indexes[True]}**; not indexed: **{indexes[False]}**",
        f"- Plan-cache hits: **{cache[True]}**; misses: **{cache[False]}**",
        "",
        "## Interpretation",
        "",
        "The report is descriptive rather than a manufactured performance claim. Compare the phase and plan distributions against a fixed-policy run before drawing causal conclusions. A healthy adaptive run should show phase changes in the recent workload model, proposals at epoch boundaries, and index-used events only after the key-index proposal is explicitly committed.",
        "",
        "## References",
        "",
        '[1]: ../benchmarks/phase_change.py "SABLE phase-change benchmark harness"',
        '[2]: ../sable/core.py "SABLE adaptive engine and telemetry schema"',
        "",
    ]
    Path(a.output).write_text("\n".join(lines))
    print("wrote", a.output)


if __name__ == "__main__":
    main()
