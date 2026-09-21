import json
import time
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parents[1]))
from sable import (
    CompressionCodec,
    JoinExecutor,
    SegmentStore,
    ShadowExecutor,
    TabularQLearner,
    UCB1Bandit,
    VectorizedExecutor,
)


def main():
    left = [{"id": i, "x": i * 2} for i in range(5000)]
    right = [{"id": i, "y": i * 3} for i in range(2500)]
    timings = {}
    for algo in ("nested_loop", "hash", "merge"):
        t = time.perf_counter()
        out = JoinExecutor.execute(
            left if algo != "nested_loop" else left[:500], right, "id", algo
        )
        timings[algo] = {
            "rows": len(out),
            "elapsed_ms": round((time.perf_counter() - t) * 1000, 3),
        }
    vec = VectorizedExecutor()
    t = time.perf_counter()
    vec_sum = vec.sum(vec.add(list(range(100000)), list(range(100000))))
    vector_ms = round((time.perf_counter() - t) * 1000, 3)
    vector_backend = vec.benchmark(1000000)
    compression = {}
    values = list(range(10000))
    for codec in CompressionCodec.codecs:
        t = time.perf_counter()
        blob = CompressionCodec.encode(values, codec)
        restored = CompressionCodec.decode(blob, codec)
        compression[codec] = {
            "bytes": len(blob),
            "roundtrip": restored == values,
            "elapsed_ms": round((time.perf_counter() - t) * 1000, 3),
        }
    shadow = ShadowExecutor.compare(
        lambda: JoinExecutor.hash_join(left[:1000], right, "id"),
        lambda: JoinExecutor.merge_join(left[:1000], right, "id"),
    )
    bandit = UCB1Bandit(["hash", "merge"])
    rl = TabularQLearner(["hash", "merge"], epsilon=0)
    for _ in range(10):
        arm = bandit.select()
        bandit.update(arm, 1.0 if arm == "hash" else 0.5)
        rl.update(("join",), arm, 1.0 if arm == "hash" else 0.5, ("join",))
    segment_path = "research-segments"
    store = SegmentStore(segment_path, "row", 1024)
    store.append(left)
    row_stats = store.stats()
    store.migrate("column")
    column_stats = store.stats()
    result = {
        "join_timings": timings,
        "vector_backend": vec.backend,
        "vector_sum": vec_sum,
        "vector_elapsed_ms": vector_ms,
        "vector_million_element_run": vector_backend,
        "compression": compression,
        "shadow": shadow.__dict__,
        "durable_segments": {"row": row_stats, "after_column_migration": column_stats},
        "bandit": bandit.status(),
        "q_learning": rl.status(),
    }
    Path("research-features-summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
