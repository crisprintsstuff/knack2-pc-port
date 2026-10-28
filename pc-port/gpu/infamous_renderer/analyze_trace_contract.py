"""Audit only the static descriptor gates of the dominant native geometry batch.

The trace has descriptor metadata and ownership flags, not vertex/index bytes or a
complete record-time state snapshot. A pass here is never permission to record.
"""

import argparse
import collections
import hashlib
import json
from pathlib import Path


def analyze(path: Path) -> dict:
    digest = hashlib.sha256()
    counts = collections.Counter()
    reasons = collections.Counter()
    max_stream_bytes = 0
    max_indices = 0
    first_submit = None
    for line in path.open("rb"):
        digest.update(line)
        if b'"event":"vertex_fetch"' not in line:
            continue
        row = json.loads(line)
        if row.get("hash") != "0xbeb7156":
            continue
        counts["target"] += 1
        first_submit = row["submit"] if first_submit is None else first_submit
        attrs = row.get("attributes", [])
        max_indices = max(max_indices, row.get("index_count", 0))
        max_stream_bytes = max(max_stream_bytes, *(a.get("size", 0) for a in attrs))
        if row.get("queue") != 0 or row.get("primitive_type") != 4 or \
                row.get("index_type") != 0 or row.get("instance_count") != 1 or \
                row.get("index_offset") != 0 or len(attrs) != 3:
            reasons["draw_shape"] += 1
            continue
        if row.get("index_ownership") != 4 or any(a.get("ownership") != 4 for a in attrs):
            reasons["ownership_or_mapping"] += 1
            continue
        if tuple(a.get("stride") for a in attrs) != (8, 8, 8) or \
                tuple(a.get("sgpr_base") for a in attrs) != (2, 2, 2) or \
                tuple(a.get("dword_offset") for a in attrs) != (0, 8, 4) or \
                tuple(a.get("instance_data") for a in attrs) != (0, 0, 0):
            reasons["fetch_layout"] += 1
            continue
        if tuple(a.get("raw", [None] * 4)[1] for a in attrs) != (524290,) * 3 or \
                tuple(a.get("raw", [None] * 4)[3] for a in attrs) != \
                (418732, 299948, 193068):
            reasons["vsharp_format"] += 1
            continue
        sizes = [a.get("size", 0) for a in attrs]
        if len(set(sizes)) != 1 or sizes[0] <= 0 or sizes[0] > 32768 or sizes[0] % 8:
            reasons["range"] += 1
            continue
        counts["static_shape_pass"] += 1
    return {
        "schema": "iss.native-geometry-static-contract.v1",
        "trace": str(path.resolve()),
        "trace_sha256": digest.hexdigest(),
        "first_submit": first_submit,
        "counts": dict(counts),
        "rejects": dict(reasons),
        "max_stream_bytes": max_stream_bytes,
        "max_index_count": max_indices,
        "recordable_from_trace": False,
        "missing_for_record": [
            "copied vertex and index bytes with stable generations",
            "per-index bounds against each declared V# range",
            "complete canonical pipeline/material/resource/render state equality",
            "proof of no intervening feedback or synchronization dependency",
            "Vulkan indirect buffer upload, visibility, lifetime and feature checks",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.trace), indent=2))
