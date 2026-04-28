#!/usr/bin/env python3
"""
Bank Benchmark Plotting Script

Generates comparison plots from benchmark results.
"""

import os
import sys
import re
import argparse
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

COLORS = {
    'uninstrumented': '#2ecc71',
    'singlelock': '#3498db',
    'tinystm': '#e74c3c',
    'tl2': '#9b59b6',
    'swisstm': '#f39c12',
}

BACKEND_LABELS = {
    'uninstrumented': 'Uninstrumented (Baseline)',
    'singlelock': 'SingleGlobalLock',
    'tinystm': 'TinySTM',
    'tl2': 'TL2',
    'swisstm': 'SwissTM',
}


def parse_results_file(results_dir):
    """Parse the results.txt file and extract data."""
    results_file = os.path.join(results_dir, 'results.txt')
    if not os.path.exists(results_file):
        print(f"Error: Results file not found: {results_file}")
        sys.exit(1)

    data = {}
    with open(results_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            parts = line.split()
            if len(parts) < 6:
                continue

            backend = parts[0]
            threads = int(parts[1])
            txns_avg = int(parts[2]) if parts[2].isdigit() else 0
            txns_std = int(parts[3]) if parts[3].isdigit() else 0

            if backend not in data:
                data[backend] = {'threads': [], 'txns_avg': [], 'txns_std': []}

            data[backend]['threads'].append(threads)
            data[backend]['txns_avg'].append(txns_avg)
            data[backend]['txns_std'].append(txns_std)

    return data


def plot_throughput(data, output_file):
    """Plot throughput comparison."""
    fig, ax = plt.subplots(figsize=(10, 6))

    for backend, values in data.items():
        if not values['threads']:
            continue

        color = COLORS.get(backend, '#95a5a6')
        label = BACKEND_LABELS.get(backend, backend)

        ax.errorbar(
            values['threads'],
            [x / 1e6 for x in values['txns_avg']],
            yerr=[x / 1e6 for x in values['txns_std']],
            marker='o',
            linewidth=2,
            markersize=8,
            capsize=4,
            label=label,
            color=color,
        )

    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Transactions per Second (Millions)', fontsize=12)
    ax.set_title('Bank Benchmark: Throughput vs Thread Count', fontsize=14, fontweight='bold')
    ax.legend(loc='best', fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.set_xticks(sorted(set(t for v in data.values() for t in v['threads'])))

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    plt.close()
    print(f"Saved throughput plot to: {output_file}")


def plot_speedup(data, output_file):
    """Plot speedup relative to uninstrumented 1-thread baseline."""
    fig, ax = plt.subplots(figsize=(10, 6))

    baseline_data = data.get('uninstrumented')
    if not baseline_data or not baseline_data['txns_avg']:
        print("Warning: No baseline data found, skipping speedup plot")
        return

    baseline_1t = baseline_data['txns_avg'][0]

    for backend, values in data.items():
        if not values['threads']:
            continue

        color = COLORS.get(backend, '#95a5a6')
        label = BACKEND_LABELS.get(backend, backend)

        speedup = [txns / baseline_1t for txns in values['txns_avg']]

        ax.plot(
            values['threads'],
            speedup,
            marker='o',
            linewidth=2,
            markersize=8,
            label=label,
            color=color,
        )

    ax.axhline(y=1, color='gray', linestyle='--', linewidth=1, alpha=0.5)
    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Speedup (vs 1-thread uninstrumented)', fontsize=12)
    ax.set_title('Bank Benchmark: Speedup vs Thread Count', fontsize=14, fontweight='bold')
    ax.legend(loc='best', fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.set_xticks(sorted(set(t for v in data.values() for t in v['threads'])))

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    plt.close()
    print(f"Saved speedup plot to: {output_file}")


def plot_comparison(data, output_file):
    """Plot comparison of all backends at different thread counts."""
    fig, ax = plt.subplots(figsize=(12, 7))

    threads = sorted(set(t for v in data.values() for t in v['threads']))
    x = np.arange(len(threads))
    width = 0.8 / max(len(data), 1)

    for i, (backend, values) in enumerate(data.items()):
        if not values['threads']:
            continue

        color = COLORS.get(backend, '#95a5a6')
        label = BACKEND_LABELS.get(backend, backend)

        txns = []
        for t in threads:
            idx = values['threads'].index(t) if t in values['threads'] else -1
            if idx >= 0:
                txns.append(values['txns_avg'][idx] / 1e6)
            else:
                txns.append(0)

        bars = ax.bar(x + i * width - (len(data) - 1) * width / 2, txns, width,
                      label=label, color=color, alpha=0.8)

    ax.set_xlabel('Number of Threads', fontsize=12)
    ax.set_ylabel('Transactions per Second (Millions)', fontsize=12)
    ax.set_title('Bank Benchmark: Backend Comparison', fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(threads)
    ax.legend(loc='best', fontsize=10)
    ax.grid(True, alpha=0.3, axis='y')

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    plt.close()
    print(f"Saved comparison plot to: {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description='Generate plots from bank benchmark results'
    )
    parser.add_argument(
        'results_dir',
        nargs='?',
        default=None,
        help='Directory containing results.txt (default: latest results_* directory)',
    )
    parser.add_argument(
        '--output', '-o',
        default='plots',
        help='Output directory for plots (default: plots)',
    )

    args = parser.parse_args()

    if args.results_dir is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        dirs = [
            d for d in os.listdir(script_dir)
            if os.path.isdir(os.path.join(script_dir, d)) and d.startswith('results_')
        ]
        if not dirs:
            print("Error: No results directories found")
            sys.exit(1)
        dirs.sort()
        args.results_dir = os.path.join(script_dir, dirs[-1])
        print(f"Using latest results directory: {args.results_dir}")

    if not os.path.exists(args.results_dir):
        print(f"Error: Results directory not found: {args.results_dir}")
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)

    data = parse_results_file(args.results_dir)

    if not data:
        print("Error: No data found in results file")
        sys.exit(1)

    print(f"Found data for backends: {', '.join(data.keys())}")

    plot_throughput(data, os.path.join(args.output, 'throughput.png'))
    plot_speedup(data, os.path.join(args.output, 'speedup.png'))
    plot_comparison(data, os.path.join(args.output, 'comparison.png'))

    print(f"\nAll plots saved to: {args.output}/")


if __name__ == '__main__':
    main()