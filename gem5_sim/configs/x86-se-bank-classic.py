"""
Classic-memory variant of x86-se-bank.py — bypasses Ruby/MESI_Three_Level_HTM
entirely. Diagnostic tool: if a pthread program deadlocks under the Ruby HTM
hierarchy but runs here, the fork's HTM protocol is at fault; if it also
deadlocks here, the bug is deeper (decoder/exec/SE emulation).
"""

import argparse
import os
import sys

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True)
    p.add_argument("--threads", type=int, default=1)
    p.add_argument("--accounts", type=int, default=64)
    p.add_argument("--txns", type=int, default=2000)
    p.add_argument("--duration", type=int, default=200)
    p.add_argument("--read-all", type=int, default=20)
    p.add_argument("--clk", default="1.8GHz")
    p.add_argument("--cpu-type", default="atomic", choices=["timing", "atomic", "o3"])
    p.add_argument("--env", action="append", default=[])
    p.add_argument("--max-ticks", type=int, default=0)
    return p.parse_args()


args = parse_args()

CPU_TYPE = {
    "timing": CPUTypes.TIMING,
    "atomic": CPUTypes.ATOMIC,
    "o3": CPUTypes.O3,
}[args.cpu_type]

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32KiB", l1i_size="32KiB", l2_size="256KiB"
)
memory = SingleChannelDDR3_1600(size="2GiB")
processor = SimpleProcessor(cpu_type=CPU_TYPE, isa=ISA.X86,
                            num_cores=args.threads + 1)
board = SimpleBoard(clk_freq=args.clk, processor=processor, memory=memory,
                    cache_hierarchy=cache_hierarchy)

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
    stdout_file=Path(m5.options.outdir).resolve() / "simout.txt",
    stderr_file=Path(m5.options.outdir).resolve() / "simerr.txt",
)

simulator = Simulator(board=board)
if args.max_ticks > 0:
    simulator.set_max_ticks(args.max_ticks)
simulator.run()
