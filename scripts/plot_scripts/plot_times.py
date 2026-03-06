import sys
import os
import argparse
import matplotlib.pyplot as plt
from pathlib import Path
from parse_apps import parse_times
from plot_apps import plot_multiple
import numpy as np

# Get the directory of the current script
script_dir = Path(__file__).resolve().parent
plt.style.use(os.path.join(script_dir, 'ieee.mplstyle'))

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("inputs", nargs="+", help="One or more input text files.")
    p.add_argument("--output", help="Output image filename (e.g. out.png, out.pdf).")
    p.add_argument("--metrics", nargs="+", help="metrics to plot")
    p.add_argument("--labels", nargs='+', default=None, help="labels for graph lines")
    p.add_argument("--xlabel", default="Time", help="Label for the x-axis.")
    p.add_argument("--ylabel", default="Time Diff", help="Label for the y-axis.")
    p.add_argument("--yrange", nargs=2, default=None, help="y range")
    p.add_argument("--title", help="title")

    return p.parse_args()


def main():

    args = parse_args()

    datasets = []
    args.labels = []
    output = args.output
    for path in args.inputs:
        times = parse_times(path, output)
        continue
        for group in times:
            if len(times[group]) == 0:
                continue
            args.output = f"{output}/{group}.png"
            args.labels.append(group)
            datasets.append((range(len(times[group])), times[group]))
            # plot_multiple([(range(len(times[group])), times[group])], args)
    plot_multiple(datasets, args)

        # print(times)
    return 0
    

    datasets = []
    for path in args.inputs:
        stats = parse_stats(path)
        for met in args.metrics:
            for idx, val in enumerate(stats[met]):
                stats[met] = np.asarray(stats[met])
                stats[met][np.isnan(stats[met])] = 0
            datasets.append((range(len(stats[met])), stats[met]))

    return plot_multiple(datasets, args)


if __name__ == "__main__":
    sys.exit(main())