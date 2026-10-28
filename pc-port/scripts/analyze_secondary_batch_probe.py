"""Validate bounded secondary-batch JSONL/blob snapshots without changing them."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path


def analyze(log: Path, blob: Path) -> dict:
    data = blob.read_bytes()
    counts = Counter()
    by_vertex = defaultdict(lambda: {"draws": 0, "keys": Counter(), "formats": Counter(),
                                     "strides": Counter(), "vertex_ownership": Counter(),
                                     "ssbo_ownership": Counter(), "index_ownership": Counter()})
    issues = Counter()
    for line in log.open(encoding="utf-8"):
        row = json.loads(line)
        if row.get("event") != "secondary_draw":
            continue
        counts["draws"] += 1
        family = by_vertex[row["vs"]]
        family["draws"] += 1
        family["keys"][row["key"]] += 1
        raw = row["vsharp_raw"]
        stride = (raw[1] >> 16) & 0x3fff
        family["strides"][stride] += 1
        family["formats"][raw[3]] += 1
        if not row["stable"]:
            issues["generation_changed_during_capture"] += 1
        if row["first_instance"] != 0:
            issues["original_first_instance_nonzero"] += 1
        if row["index_type"] != 0:
            issues["index_not_uint16"] += 1
        if row["ssbo"]["size"] != row["instances"] * 72:
            issues["ssbo_size_not_72_per_instance"] += 1
        for name, counter in (("vertex", family["vertex_ownership"]),
                              ("ssbo", family["ssbo_ownership"]),
                              ("indices", family["index_ownership"])):
            snapshot = row[name]
            counter[snapshot["ownership"]] += 1
            if not snapshot["bytes_copied"]:
                issues[f"{name}_bytes_missing"] += 1
                continue
            offset, size = snapshot["blob_offset"], snapshot["blob_size"]
            if size != snapshot["size"] or offset + size > len(data):
                issues[f"{name}_blob_invalid"] += 1
        vertex, indices = row["vertex"], row["indices"]
        if not vertex["bytes_copied"] or not indices["bytes_copied"] or not stride or \
                vertex["size"] % stride:
            continue
        start, size = indices["blob_offset"], indices["blob_size"]
        if size != row["index_count"] * 2 or start + size > len(data):
            issues["index_blob_or_count_mismatch"] += 1
            continue
        vertex_count = vertex["size"] // stride
        vertex_offset = row["vertex_offset"]
        for i in range(row["index_count"]):
            index = int.from_bytes(data[start + i * 2:start + i * 2 + 2], "little")
            if not 0 <= index + vertex_offset < vertex_count:
                issues["out_of_bounds_vertex_index"] += 1
                break
        else:
            counts["index_ranges_valid"] += 1
    return {
        "schema": "iss.secondary-batch-probe-audit.v1",
        "log": str(log.resolve()), "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
        "blob": str(blob.resolve()), "blob_sha256": hashlib.sha256(data).hexdigest(),
        "blob_bytes": len(data), "counts": dict(counts), "issues": dict(issues),
        "families": {key: {field: dict(value) if isinstance(value, Counter) else value
                           for field, value in family.items()}
                     for key, family in by_vertex.items()},
        "limitations": [
            "Only bounded captured draws are counted; no runtime batch was recorded.",
            "This audit does not prove material/render state equality or last GPU writer identity.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("blob", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    content = json.dumps(analyze(args.log, args.blob), indent=2) + "\n"
    if args.output:
        args.output.write_text(content, encoding="utf-8")
    print(content)
