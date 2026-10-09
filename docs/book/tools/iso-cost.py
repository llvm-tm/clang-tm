#!/usr/bin/env python3
"""Expected attempts under serializable-OCC vs snapshot-isolation validation.

Model (ch07 sec:iso-cost): hot set of k objects; each transaction reads r and
writes w of them, uniform and independent; C concurrent transactions.
Validation scope per contender: WW-only (SI) is w; value-validated serializable
OCC is 2r+w.  p_abort = 1 - (1 - w/k)^(scope*(C-1));  E[attempts] = 1/(1-p).
"""

def attempts(k: int, r: int, w: int, c: int, scope: int) -> float:
    p = 1.0 - (1.0 - w / k) ** (scope * (c - 1))
    return 1.0 / (1.0 - p) if p < 1.0 else float("inf")


def table(k: int, r: int, w: int, concurrency: range) -> None:
    print(f"k={k} r={r} w={w}")
    print("C    E[S-OCC]   E[SI]")
    for c in concurrency:
        print(f"{c:3d}  {attempts(k, r, w, c, 2 * r + w):8.2f}  {attempts(k, r, w, c, w):6.2f}")


if __name__ == "__main__":
    table(50, 4, 1, range(2, 21, 2))
