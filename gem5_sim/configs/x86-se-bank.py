"""
x86-64 SE-mode config for TM benchmarks (e.g. tm_api_cpp's `bank`) built
against *software* TM backends (TinySTM/NOrec/TL2/...).  No TM ISA support
(TSX/TME/HTM) is required in the simulated CPU: transactional atomicity is
provided entirely in software, so this runs TM benchmarks on machines
(and simulated ISAs) without any hardware TM instruction set.

The MESI_Three_Level_HTM Ruby hierarchy is reused (the X86_TSX build ships
it and it keeps results directly comparable with the HTM/TSX runs); no HTM
instructions are exercised by software-backend binaries.

Build the benchmark (on any host, no TSX needed):
    cd tm_api_cpp/benchmarks/cpp
    make BACKEND=NOREC GEM5=1 bin/bank_gem5      # static x86-64, ROI markers

Run:
    ./build/X86_TSX/gem5.opt -d m5out/bank-norec-t1 \
        gem5_sim/configs/x86-se-bank.py \
        --binary <path>/bank_gem5 --threads 1 --accounts 64 --txns 2000

Transaction behaviour capture: pass --env TM_TRACE_PATH=/tmp/tm_trace.txt to
record every TM op from inside the guest (SE syscalls write to the host FS).
"""

import argparse
import os
import sys

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM)

from components.mesi_three_level_htm_cache_hierarchy import (
    MESIThreeLevelHTMCacheHierarchy,
)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, help="static x86-64 benchmark ELF")
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
                   help="guest env var, KEY=VALUE (repeatable), e.g. TM_TRACE_PATH=...")
    p.add_argument("--max-ticks", type=int, default=0,
                   help="abort simulation after this many ticks (0 = unlimited); "
                        "1 tick = 1 ps, so 1e12 ticks = 1 s of simulated time "
                        "at any clock. Use to bound hung/slow runs.")
    return p.parse_args()


args = parse_args()

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

# SE mode needs one hardware thread context per software thread (gem5 does
# not timeshare contexts): the benchmark spawns --threads workers plus the
# main thread, so provision --threads + 1 cores.
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
from pathlib import Path

board.set_se_binary_workload(
    BinaryResource(local_path=os.path.abspath(args.binary)),
    arguments=bench_args,
    env_list=args.env,
    # Route guest stdout/stderr into the gem5 outdir so run scripts and
    # result checkers can find them (simout.txt / simerr.txt).
    stdout_file=Path(m5.options.outdir) / "simout.txt",
    stderr_file=Path(m5.options.outdir) / "simerr.txt",
)

simulator = Simulator(board=board)
if args.max_ticks > 0:
    simulator.set_max_ticks(args.max_ticks)
    print(f"=== max-ticks guard: {args.max_ticks} ticks ===")
simulator.run()
