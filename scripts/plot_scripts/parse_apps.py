import os
import re
import sys
import numpy as np

def parse_cgups(path):
    nums = []
    with open(path, 'r', errors='replace') as f:
        for line in f:
            # split by whitespace
            tokens = line.strip().split()
            for tok in tokens:
                tok = tok.strip().rstrip(',')  # drop trailing commas
                # skip obvious hex tokens that start with 0x (case-insensitive)
                if tok.lower().startswith("0x"):
                    continue
                # If token is pure decimal digits, accept it
                if tok.isdigit():
                    nums.append(int(tok))
                # else: ignore (this will ignore hex addresses and text)
    return list(range(len(nums))), nums


def parse_gapbs(file_path):
    TRIAL_RE = re.compile(r'Trial Time:\s*([0-9]*\.?[0-9]+)')
    """Return a list of float trial times from a text file."""
    vals = []
    with open(file_path, 'r', errors='ignore') as f:
        for line in f:
            m = TRIAL_RE.search(line)
            if m:
                try:
                    vals.append(float(m.group(1)))
                except ValueError:
                    pass
    return list(range(len(vals))), vals

def parse_resnet(filepath):
    """
    Return a sorted list of (epoch, images_per_sec) found in the file.
    If the same epoch appears multiple times, the last occurrence is used.
    """
    EPOCH_RE = re.compile(
    r"Epoch\s*[:\s]*\s*([0-9]+)\s*\s*[:]\s*([0-9]*\.?[0-9]+)\s*images/sec",
    flags=re.IGNORECASE,
)
    if not os.path.isfile(filepath):
        return []

    values = []
    with open(filepath, "r", errors="replace") as f:
        for line in f:
            m = EPOCH_RE.search(line)
            if not m:
                # some logs might use "Epoch 1: 4.13 images/sec" without an extra colon or with other spacing,
                # try a more permissive pattern:
                m2 = re.search(r"Epoch\s*([0-9]+)\s*[:]\s*([0-9]*\.?[0-9]+)\s*images/sec", line, flags=re.IGNORECASE)
                m = m2
            if m:
                try:
                    epoch = int(m.group(1))
                    val = float(m.group(2))
                    values.append(val)
                except Exception:
                    # ignore bad parses
                    pass

    # Return epochs sorted ascending
    return list(range(len(values))), values

def parse_stream(file_path):
    ITER_RE = re.compile(r'Iter\s+\d+:\s*time\s*=\s*([0-9]*\.?[0-9]+)\s*seconds', re.IGNORECASE)
    """Return a list of float trial times from a text file."""
    vals: List[float] = []
    if not os.path.isfile(file_path):
        return vals
    with open(file_path, 'r', errors='ignore') as f:
        for line in f:
            m = ITER_RE.search(line)
            if m:
                try:
                    vals.append(float(m.group(1)))
                except ValueError:
                    pass
    return list(range(len(vals))), vals

def check_args(args):
    # Validate inputs exist
    for p in args.inputs:
        if not os.path.isfile(p):
            print(f"Input file '{p}' not found.", file=sys.stderr)
            return 1

    if args.labels == None:
        args.labels = [""] * len(args.inputs)

    if len(args.inputs) != len(args.labels):
        print(f"Input files not equal to the number of labels: {len(args.inputs)} != {len(args.labels)}")
        return 1


def parse_stats(file_path):
    RE_METRIC = re.compile(r'([A-Za-z0-9_/-]+):\s*\[([^\]]*)\]')
    met_dict = dict()
    if not os.path.isfile(file_path):
        print(f"Not a valid file: {file_path}")
        sys.exit(1)

    with open(file_path, 'r') as f:
        for line in f:
            for m in RE_METRIC.finditer(line):
                name = m.group(1)
                val  = float(m.group(2))
                met_dict.setdefault(name, []).append(val)
    return met_dict

import matplotlib.pyplot as plt

def parse_times(file_path, output_dir):
    dtype = np.dtype([
        ("ts", "<f8"),
        ("group", "u1"),
    ])

    BASE_GROUPS = np.array([
        "Read", "Lookup", "Cool", "Pred", #"Add page", "update neighbor", "pagr pred start", "pagr hot start", "sample_lru_start",
        "Finish", "Reset Start", "Reset Finish"
    ])

    data = np.fromfile(file_path, dtype=dtype)

    ts = data["ts"]
    groups = data["group"]

    finish_id = np.where(BASE_GROUPS == "Finish")[0][0]
    finish_indices = np.where(groups == finish_id)[0]

    num_stages = finish_id
    finish_indices = finish_indices[finish_indices >= num_stages]

    window_offsets = np.arange(-num_stages, 1)
    windows = finish_indices[:, None] + window_offsets
    window_ts = ts[windows]

    durations = np.diff(window_ts, axis=1)
    stage_names = BASE_GROUPS[:finish_id]

    os.makedirs(output_dir, exist_ok=True)

    # ---- Create distribution plot per stage ----
    for i, stage in enumerate(stage_names):
        stage_durations = durations[:, i]

        # Remove zeros or negatives (required for log scale)
        stage_durations = stage_durations[stage_durations > 0]

        plt.figure()

        # Log-spaced bins
        bins = np.logspace(
            np.log10(stage_durations.min()),
            np.log10(stage_durations.max()),
            100
        )

        plt.hist(stage_durations, bins=bins)
        plt.xscale("log")
        plt.yscale("log")

        plt.xlabel("Time (seconds) [log scale]")
        plt.ylabel("Frequency")
        plt.title(f"{stage} Time Distribution")

        output_path = os.path.join(output_dir, f"{stage}_distribution.png")
        plt.savefig(output_path, bbox_inches="tight")
        plt.close()

        print(f"Saved: {output_path}")

    # Optional: still print medians
    median_time_taken = {
        stage_names[i]: np.median(durations[:, i])
        for i in range(len(stage_names))
    }

    tot_time = sum(median_time_taken.values())

    print("\nMedian Time Per Stage:")
    for k, v in median_time_taken.items():
        print(f"{k}:\t{v:.9f}s ({100 * v / tot_time:.2f}%)")
    print(f"Total:\t{tot_time:.9f}s")

    # ts = data["ts"]
    # group = data["group"]

    # base_ids = group        # 0=PRED, 1=RESET, ...
    # is_start = (group % 2) == 0  # True if _S

    # results = {}

    # for base_id, name in enumerate(BASE_GROUPS):
    #     mask = base_ids == base_id

    #     group_ts = ts[mask]
    #     group_is_start = is_start[mask]

    #     starts = group_ts[group_is_start]
    #     finishes = group_ts[~group_is_start]

    #     # Pair start and finish
    #     count = min(len(starts), len(finishes))
    #     durations = finishes[:count] - starts[:count]
    #     timestamps = starts[:count]  # associate each duration with its start time

    #     if len(durations) == 0:
    #         results[name] = np.array([])
    #         continue

    #     # Compute bins
    #     start_time = timestamps[0]
    #     end_time = timestamps[-1]
    #     n_bins = int(np.ceil((end_time - start_time) / interval))
    #     bin_edges = start_time + np.arange(n_bins + 1) * interval

    #     # Digitize timestamps into bins
    #     bin_indices = np.digitize(timestamps, bin_edges) - 1  # bins start at 0

    #     # Compute average per bin
    #     avg_per_bin = np.zeros(n_bins)
    #     for i in range(n_bins):
    #         in_bin = durations[bin_indices == i]
    #         if len(in_bin) > 0:
    #             avg_per_bin[i] = np.median(in_bin)
    #         else:
    #             avg_per_bin[i] = np.nan  # or 0 if you prefer

    #     results[name] = avg_per_bin

    # return results

