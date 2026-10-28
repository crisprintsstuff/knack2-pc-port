"""Inspect the two PS 1318DDD4 draw families in a completed command trace."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


TARGETS = {0xC01162F7, 0x47B0694B}


def shape(stage):
    return {
        "user_data_words": len(stage.get("user_data", [])),
        "buffers": len(stage.get("buffers", [])),
        "images": len(stage.get("images", [])),
        "hash": stage.get("hash"),
    }


def analyze(path):
    digest = hashlib.sha256()
    fragment = None
    last_packet = None
    instances = None
    index_type = None
    previous_pair = None
    gap = []
    result = defaultdict(lambda: {
        "draws": 0, "samples": [], "stage_shapes": Counter(), "userdata_changes": Counter(),
        "buffer_changes": Counter(), "image_changes": Counter(), "gap_sets": Counter(),
        "indexed_packets": Counter(), "index_type_packets": Counter(), "run_lengths": [],
        "instances": Counter(), "index_types": Counter(), "ssbo_bytes": Counter(),
        "ssbo_matches_instances_72": Counter(),
        "last": None,
    })
    for raw in path.open("rb"):
        digest.update(raw)
        row = json.loads(raw)
        if row.get("event") == "packet":
            gap.append(row.get("opcode"))
            if row.get("opcode") == 0x27:
                last_packet = row
            if row.get("opcode") == 0x2f and len(row.get("words", [])) >= 2:
                instances = row["words"][1]
            if row.get("opcode") == 0x2a and len(row.get("words", [])) >= 2:
                index_type = row["words"][1]
            continue
        if row.get("event") != "draw":
            continue
        if row.get("stage") == 0:
            fragment = row
            continue
        if row.get("stage") != 1 or fragment is None:
            continue
        vertex_hash = int(row.get("hash", "0"), 16)
        target = (vertex_hash in TARGETS and
                  int(fragment.get("hash", "0"), 16) == 0x1318DDD4 and
                  all(fragment.get(k) == row.get(k) for k in ("submit", "queue", "tick")))
        if target:
            item = result[vertex_hash]
            item["draws"] += 1
            item["instances"][instances] += 1
            item["index_types"][index_type] += 1
            buffers = row.get("buffers", [])
            if buffers:
                ssbo_bytes = buffers[0].get("size")
                item["ssbo_bytes"][ssbo_bytes] += 1
                item["ssbo_matches_instances_72"][
                    isinstance(instances, int) and ssbo_bytes == instances * 72] += 1
            item["stage_shapes"][(tuple(shape(fragment).values()),
                                  tuple(shape(row).values()))] += 1
            if len(item["samples"]) < 2:
                item["samples"].append({"fragment": fragment, "vertex": row,
                                        "last_index_packet": last_packet})
            if previous_pair == vertex_hash and item["last"]:
                before_ps, before_vs = item["last"]
                for side, before, current in (("ps", before_ps, fragment),
                                              ("vs", before_vs, row)):
                    for field, counter in (("user_data", item["userdata_changes"]),
                                           ("buffers", item["buffer_changes"]),
                                           ("images", item["image_changes"])):
                        a, b = before.get(field, []), current.get(field, [])
                        for i in range(max(len(a), len(b))):
                            if i >= len(a) or i >= len(b) or a[i] != b[i]:
                                counter[(side, i)] += 1
                item["gap_sets"][tuple(sorted(set(gap)))] += 1
                item["run_lengths"][-1] += 1
            else:
                item["run_lengths"].append(1)
            item["last"] = fragment, row
            if last_packet:
                item["indexed_packets"][last_packet.get("opcode")] += 1
            previous_pair = vertex_hash
        else:
            previous_pair = None
        fragment = None
        gap.clear()
    output = {"schema": "iss.secondary-batch-families.v1", "trace": str(path.resolve()),
              "trace_sha256": digest.hexdigest(), "families": {}}
    for h, item in result.items():
        output["families"][f"0x{h:08x}"] = {
            "draws": item["draws"],
            "stage_shapes": [{"shape": str(k), "count": v}
                             for k, v in item["stage_shapes"].most_common()],
            "userdata_changes": {str(k): v for k, v in item["userdata_changes"].items()},
            "buffer_changes": {str(k): v for k, v in item["buffer_changes"].items()},
            "image_changes": {str(k): v for k, v in item["image_changes"].items()},
            "gap_sets": [{"opcodes": [f"0x{x:02x}" for x in k], "count": v}
                         for k, v in item["gap_sets"].most_common(8)],
            "runs": len(item["run_lengths"]),
            "max_run": max(item["run_lengths"], default=0),
            "instances": dict(item["instances"]),
            "index_types": dict(item["index_types"]),
            "ssbo_bytes": dict(item["ssbo_bytes"]),
            "ssbo_matches_instances_72": dict(item["ssbo_matches_instances_72"]),
            "samples": item["samples"],
        }
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.trace)
    content = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(content, encoding="utf-8")
    print(content)
