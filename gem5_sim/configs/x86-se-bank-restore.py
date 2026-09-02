"""
x86-64 SE-mode *restore* config for TM benchmarks (checkpoint-and-continue).

Resumes a checkpoint created by x86-se-bank-checkpoint.py.  The checkpoint
was taken at the ROI start (right after ROI_RESET_STATS / m5_work_begin,
process fully initialised), so this run skips process init and the TM
region setup entirely: it resumes at the instruction after m5_work_begin
and runs the transactional ROI to completion with freshly reset
statistics.  stats.txt therefore reflects only the measured region.

The restored board uses the *same* Ruby MESI_Three_Level_HTM hierarchy
(kept identical so HTM results are comparable) but any CPU type / clock /
cache sizing may be chosen here — the checkpoint carries only the process
state and memory, so a single save can feed many measurement variants.

Build the checkpoint binary (see x86-se-bank-checkpoint.py):
    cd tm_api_cpp/benchmarks/cpp
    make BACKEND=NOREC GEM5=1 GEM5_CKPT=1 bin/bank_gem5_ckpt

Restore phase:
    ./build/X86_TSX/gem5.opt -d m5out/ckpt-restore \
        gem5_sim/configs/x86-se-bank-restore.py \
        --binary <path>/bank_gem5_ckpt_norec --threads 1 --accounts 16 \
        --txns 50 --checkpoint <save>/cpt.bank --cpu-type timing

The restored binary re-runs its own ROI bookkeeping (it does not re-run
function-scope setup after init — the checkpoint is *after* the start-gate
handshake) and prints `PASS: Money conserved`; the ROI stats are in the
restore run's stats.txt.

KNOWN LIMITATIONS (as of 2026-09-01):
1.  Restore hangs in SE mode with the multithreaded pthread benchmark
    (pre-existing gem5 SE checkpoint caveat: process/FD state not fully
    serialized).  The save side is verified on the classic hierarchy; the
    Ruby/HTM save path panics during serialization (see
    x86-se-bank-checkpoint.py).  Restore is therefore not a stable path
    for this workload yet.
"""

import argparse
import os
import sys
from pathlib import Path

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.utils.requires import requires

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gem5.coherence_protocol import CoherenceProtocol

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM)

from components.mesi_three_level_htm_cache_hierarchy import (
    MESIThreeLevelHTMCacheHierarchy,
)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, help="static x86-64 benchmark ELF "
                   "(GEM5_CKPT=1 variant, same one used for the save phase)")
    p.add_argument("--threads", type=int, default=1, help="benchmark threads (= cores)")
    p.add_argument("--accounts", type=int, default=64)
    p.add_argument("--txns", type=int, default=2000,
                   help="total transaction quota (0 = time-based, then --duration)")
    p.add_argument("--duration", type=int, default=200, help="ms, used when --txns 0")
    p.add_argument("--read-all", type=int, default=20, help="read-all percentage")
    p.add_argument("--clk", default="1.8GHz",
                   help="core clock (1.8GHz matches the Broadwell-EP calibration)")
    p.add_argument("--cpu-type", default="timing",
                   choices=["timing", "atomic", "o3"],
                   help="timing = cycle-accurate; atomic = fast functional check")
    p.add_argument("--env", action="append", default=[],
                   help="guest env var, KEY=VALUE (repeatable)")
    p.add_argument("--checkpoint", required=True,
                   help="checkpoint directory written by the save phase "
                        "(x86-se-bank-checkpoint.py --checkpoint-dir)")
    return p.parse_args()


args = parse_args()

if args.cpu_type == "atomic":
    print("ERROR: HTM (XBEGIN/XEND) is not implemented for Atomic CPU (htm.cc:44). "
          "Use --cpu-type timing or o3.")
    raise SystemExit(2)

CPU_TYPE = {
    "timing": CPUTypes.TIMING,
    "atomic": CPUTypes.ATOMIC,
    "o3": CPUTypes.O3,
}[args.cpu_type]

cache_hierarchy = MESIThreeLevelHTMCacheHierarchy(
    l1i_size="32KiB",
    l1i_assoc=8,
    l1d_size="32KiB",
    l1d_assoc=8,
    l2_size="256KiB",
    l2_assoc=8,
    l3_size="2MiB",
    l3_assoc=16,
    num_l3_banks=1,
)

memory = SingleChannelDDR3_1600(size="2GiB")

processor = SimpleProcessor(
    cpu_type=CPU_TYPE,
    isa=ISA.X86,
    num_cores=args.threads + 1,
)

board = SimpleBoard(
    clk_freq=args.clk,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

bench_args = ["-a", str(args.accounts), "-t", str(args.threads),
              "-r", str(args.read_all)]
if args.txns > 0:
    bench_args += ["-n", str(args.txns)]
else:
    bench_args += ["-d", str(args.duration)]

import m5
from gem5.simulate.simulator import Simulator

board.set_se_binary_workload(
    BinaryResource(local_path=os.path.abspath(args.binary)),
    arguments=bench_args,
    env_list=args.env,
    checkpoint=Path(args.checkpoint),
    stdout_file=Path(m5.options.outdir) / "simout.txt",
    stderr_file=Path(m5.options.outdir) / "simerr.txt",
)

simulator = Simulator(board=board)
simulator.run()
