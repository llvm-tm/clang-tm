#!/usr/bin/env python3
"""Generate yada mesh input files matching the original STAMP spec.

The original yada benchmark (https://github.com/ccaominh/stamp) reads
Triangle-format files:
  <prefix>.node — vertex coordinates
  <prefix>.ele  — triangle element indices
  <prefix>.poly — boundary segments

Spec: https://github.com/ccaominh/stamp/blob/master/yada/README

Without this tool, yada generates a 10x10 grid with ±0.5 jitter and
spacing 4.0, producing 162 triangles all with min_angle > 20° — so
the work_heap is empty and the benchmark hangs.

This script generates meshes with tunable irregularity so that many
triangles fail the angle constraint and enter the work heap.
"""

import argparse
import os
import random


def generate_mesh(grid_size, spacing, jitter, seed=42):
    """Generate a 2D triangulated mesh with controlled irregularity.

    Returns (vertices, triangles) where:
      vertices is a list of (x, y) tuples
      triangles is a list of (v0, v1, v2) 0-based index tuples
    """
    rng = random.Random(seed)
    points = []
    for i in range(grid_size):
        for j in range(grid_size):
            px = i * spacing + rng.uniform(-jitter, jitter)
            py = j * spacing + rng.uniform(-jitter, jitter)
            points.append((px, py))

    triangles = []
    for i in range(grid_size - 1):
        for j in range(grid_size - 1):
            idx0 = i * grid_size + j
            idx1 = i * grid_size + j + 1
            idx2 = (i + 1) * grid_size + j
            idx3 = (i + 1) * grid_size + j + 1
            triangles.append((idx0, idx1, idx3))
            triangles.append((idx0, idx3, idx2))

    return points, triangles


def write_node(vertices, path):
    """Write a .node file in Triangle format."""
    with open(path, "w") as f:
        f.write(f"{len(vertices)} 2 0 0\n")
        for i, (x, y) in enumerate(vertices, start=1):
            f.write(f"{i} {x:.15g} {y:.15g}\n")


def write_ele(triangles, path):
    """Write an .ele file in Triangle format."""
    with open(path, "w") as f:
        f.write(f"{len(triangles)} 3 0\n")
        for i, t in enumerate(triangles, start=1):
            # .ele format uses 1-based vertex indices
            f.write(f"{i} {t[0]+1} {t[1]+1} {t[2]+1}\n")


def write_poly(vertices, path):
    """Write a minimal .poly file (no boundary segments, no holes).

    The yada benchmark computes boundary edges algorithmically from
    neighbor analysis, so a minimal .poly suffices.
    """
    with open(path, "w") as f:
        f.write(f"{len(vertices)} 2 0 0\n")
        f.write("0 1\n")
        f.write("0\n")


def main():
    parser = argparse.ArgumentParser(
        description="Generate yada .node/.ele/.poly mesh files "
        "with tunable irregularity"
    )
    parser.add_argument(
        "-p",
        "--prefix",
        default=None,
        help="Output file prefix (e.g. 'inputs/mesh' → "
        "inputs/mesh.node, inputs/mesh.ele, inputs/mesh.poly). "
        "If omitted, writes a single .mesh file (legacy format).",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="yada.mesh",
        help="Output .mesh file path (default: yada.mesh). "
        "Only used when --prefix is not set.",
    )
    parser.add_argument(
        "-g",
        "--grid",
        type=int,
        default=10,
        help="Grid size (default: 10 → 100 points, 162 triangles)",
    )
    parser.add_argument(
        "-s",
        "--spacing",
        type=float,
        default=4.0,
        help="Point spacing (default: 4.0)",
    )
    parser.add_argument(
        "-j",
        "--jitter",
        type=float,
        default=2.5,
        help="Jitter magnitude. Default 2.5 produces many bad "
        "triangles with spacing=4.0. Use 0.5 for the original "
        "near-regular grid (all good).",
    )
    parser.add_argument(
        "--seed", type=int, default=42, help="Random seed (default: 42)"
    )
    args = parser.parse_args()

    vertices, triangles = generate_mesh(
        grid_size=args.grid,
        spacing=args.spacing,
        jitter=args.jitter,
        seed=args.seed,
    )

    if args.prefix:
        out_dir = os.path.dirname(args.prefix) if os.path.dirname(args.prefix) else "."
        if out_dir and not os.path.exists(out_dir):
            os.makedirs(out_dir, exist_ok=True)
        node_path = args.prefix + ".node"
        ele_path = args.prefix + ".ele"
        poly_path = args.prefix + ".poly"
        write_node(vertices, node_path)
        write_ele(triangles, ele_path)
        write_poly(vertices, poly_path)
        print(f"Wrote {len(vertices)} vertices, {len(triangles)} triangles →")
        print(f"  {node_path}")
        print(f"  {ele_path}")
        print(f"  {poly_path}")
    else:
        out_dir = os.path.dirname(args.output)
        if out_dir and not os.path.exists(out_dir):
            os.makedirs(out_dir, exist_ok=True)
        with open(args.output, "w") as f:
            f.write(f"{len(vertices)} {len(triangles)}\n")
            for x, y in vertices:
                f.write(f"{x:.15g} {y:.15g}\n")
            for t in triangles:
                f.write(f"{t[0]} {t[1]} {t[2]}\n")
        print(f"Wrote {len(vertices)} vertices, {len(triangles)} "
              f"triangles → {args.output}")


if __name__ == "__main__":
    main()
