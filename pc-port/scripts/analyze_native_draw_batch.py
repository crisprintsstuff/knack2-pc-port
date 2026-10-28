"""Measure one shader pair's batching boundary in a completed command trace.

This is an observation of translated commands and declared resources. It never
changes a game packet, shader, save, pipeline or cache.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import statistics


def parse_hash(value):
    return int(value, 16)


def analyze(path, fragment_hash, vertex_hash):
    target = (parse_hash(fragment_hash), parse_hash(vertex_hash))
    sha = hashlib.sha256()
    event_counts = Counter()
    between = []
    last_index_packet = None
    fragment = None
    previous_target = False
    previous_images = None
    previous_userdata_address = None
    runs = []
    userdata_addresses = []
    index_addresses = []
    index_counts = []
    gap_opcodes = Counter()
    gap_opcode_sets = Counter()
    adjacent_userdata_plus_48 = 0
    same_image_adjacencies = 0
    invalid_target_draws = 0
    complete = False
    vertex_fetch_events = 0
    vertex_fetch_index_mismatches = 0

    with path.open("rb") as stream:
        for raw_line in stream:
            sha.update(raw_line)
            try:
                row = json.loads(raw_line)
            except json.JSONDecodeError:
                continue
            event = row.get("event")
            event_counts[event] += 1
            if event == "end":
                complete = row.get("reason") == "complete"
            if event == "vertex_fetch":
                if previous_target and runs and row.get("hash") == f"0x{target[1]:x}":
                    runs[-1].setdefault("fetches", []).append(row)
                    vertex_fetch_events += 1
                    if index_addresses and int(row.get("index_address", "0"), 16) != index_addresses[-1]:
                        vertex_fetch_index_mismatches += 1
                continue
            if event == "packet":
                between.append(row.get("opcode"))
                if row.get("opcode") == 0x27:
                    last_index_packet = row
                continue
            if event != "draw":
                continue
            if row.get("stage") == 0:
                fragment = row
                continue
            if row.get("stage") != 1 or fragment is None:
                continue

            same_command = (fragment.get("submit") == row.get("submit") and
                            fragment.get("queue") == row.get("queue") and
                            fragment.get("tick") == row.get("tick"))
            matched = same_command and (parse_hash(fragment["hash"]),
                                        parse_hash(row["hash"])) == target
            if matched:
                images = fragment.get("images", [])
                userdata = row.get("user_data", [])
                packet = last_index_packet
                words = packet.get("words", []) if packet else []
                valid_packet = (packet and packet.get("submit") == row.get("submit")
                                and packet.get("queue") == row.get("queue")
                                and len(words) >= 5)
                if len(images) < 5 or len(userdata) < 4 or not valid_packet:
                    invalid_target_draws += 1
                    previous_target = False
                else:
                    image_key = (images[0].get("base"), images[4].get("base"))
                    userdata_address = (int(userdata[3]) << 32) | int(userdata[2])
                    index_address = (int(words[3]) << 32) | int(words[2])
                    userdata_addresses.append(userdata_address)
                    index_addresses.append(index_address)
                    index_counts.append(int(words[1]))
                    if previous_target and image_key == previous_images:
                        run = runs[-1]
                        run["draws"] += 1
                        for side, current in (("fragment", fragment), ("vertex", row)):
                            first = run[f"{side}_first"]
                            for field in ("user_data", "buffers", "images"):
                                for index, (before, now) in enumerate(zip(first[field], current[field])):
                                    if before != now:
                                        run["variations"][f"{side}_{field}"].add(index)
                        same_image_adjacencies += 1
                        gap_opcodes.update(between)
                        gap_opcode_sets[tuple(sorted(set(between)))] += 1
                        adjacent_userdata_plus_48 += (
                            userdata_address == previous_userdata_address + 48)
                    else:
                        runs.append({
                            "images": image_key,
                            "draws": 1,
                            "fragment_first": fragment,
                            "vertex_first": row,
                            "variations": {
                                f"{side}_{field}": set()
                                for side in ("fragment", "vertex")
                                for field in ("user_data", "buffers", "images")
                            },
                        })
                    previous_target = True
                    previous_images = image_key
                    previous_userdata_address = userdata_address
            else:
                previous_target = False
            fragment = None
            between = []

    sizes = [run["draws"] for run in runs]
    varying_groups = {}
    for side in ("fragment", "vertex"):
        for field in ("user_data", "buffers", "images"):
            key = f"{side}_{field}"
            counts = Counter(index for run in runs for index in run["variations"][key])
            varying_groups[key] = {str(index): count for index, count in sorted(counts.items())}
    fetch_attribute_variation = Counter()
    fetch_state_variation = Counter()
    pipeline_identities = set()
    for run in runs:
        fetches = run.get("fetches", [])
        if not fetches:
            continue
        first = fetches[0]
        pipeline_identities.update(row.get("pipeline_identity") for row in fetches)
        for field in ("context_changes", "primitive_type", "instance_count", "index_type"):
            if len({row.get(field) for row in fetches}) > 1:
                fetch_state_variation[field] += 1
        for index in range(len(first.get("attributes", []))):
            for field in ("raw", "base", "size", "stride"):
                if len({json.dumps(row["attributes"][index].get(field), sort_keys=True)
                        for row in fetches}) > 1:
                    fetch_attribute_variation[f"{index}:{field}"] += 1
    return {
        "schema": "iss.native-draw-batch-feasibility.v1",
        "trace": str(path.resolve()),
        "trace_sha256": sha.hexdigest(),
        "complete": complete,
        "fragment_hash": f"0x{target[0]:08x}",
        "vertex_hash": f"0x{target[1]:08x}",
        "event_counts": dict(event_counts),
        "target_draws": len(userdata_addresses),
        "invalid_target_draws": invalid_target_draws,
        "vertex_fetch_events": vertex_fetch_events,
        "vertex_fetch_index_mismatches": vertex_fetch_index_mismatches,
        "pipeline_identities_in_fetch_events": len(pipeline_identities),
        "varying_fetch_state_groups": dict(sorted(fetch_state_variation.items())),
        "varying_fetch_attribute_groups": dict(sorted(fetch_attribute_variation.items())),
        "image_groups": len(runs),
        "unique_image_pairs": len({tuple(run["images"]) for run in runs}),
        "group_median_draws": statistics.median(sizes) if sizes else 0,
        "group_max_draws": max(sizes, default=0),
        "varying_slots_within_image_groups": varying_groups,
        "same_image_adjacencies": same_image_adjacencies,
        "userdata_address_unique": len(set(userdata_addresses)),
        "userdata_2mib_regions": len({addr >> 21 for addr in userdata_addresses}),
        "adjacent_userdata_plus_48": adjacent_userdata_plus_48,
        "index_address_unique": len(set(index_addresses)),
        "index_2mib_regions": len({addr >> 21 for addr in index_addresses}),
        "total_index_count_declared": sum(index_counts),
        "intervening_packet_opcodes": {f"0x{op:02x}": count
                                       for op, count in sorted(gap_opcodes.items())
                                       if isinstance(op, int)},
        "intervening_opcode_sets": [
            {"opcodes": [f"0x{op:02x}" for op in opcodes], "draw_gaps": count}
            for opcodes, count in gap_opcode_sets.most_common()
        ],
        "limits": [
            "Only recorded packets are counted; the trace omits register setters by design.",
            "Shader hashes and declared descriptors do not prove GPU access or visual equivalence.",
            "A common shader pair and image group is not authorization to merge draws.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--fragment", default="0x6353e48b")
    parser.add_argument("--vertex", default="0x0beb7156")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.trace, args.fragment, args.vertex)
    result = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding="utf-8")
    print(result, end="")


if __name__ == "__main__":
    main()
