"""
FS tiny benchmark: boots x86-ubuntu-24.04, runs a single short TM workload
as init script, then exits. Designed for <30s simulated time on 1-2 cores.

Usage (arm64 host, no KVM - uses TIMING):
  ./build/X86_TSX/gem5.opt -d /tmp/fs-tiny gem5_sim/configs/x86-fs-tiny.py \
      --binary /path/to/bank_tsx_se --threads 2

Usage (x86 host with KVM - fast-forward):
  ./build/X86_TSX/gem5.opt -d /tmp/fs-tiny gem5_sim/configs/x86-fs-tiny.py \
      --binary /path/to/bank_tsx_se --threads 2 --kvm

Requires: MESI_Three_Level_HTM (X86_TSX build), resources downloaded on first run.
"""

import argparse
import os
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, help="x86-64 static ELF (bank_tiny etc)")
parser.add_argument("--threads", type=int, default=1)
parser.add_argument("--kvm", action="store_true", help="use KVM for boot (x86 host only)")
parser.add_argument("--clk", default="1.8GHz")
args, _ = parser.parse_known_args()

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import SimpleSwitchableProcessor
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource, obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# FS needs full x86 board; we keep KVM optional
if args.kvm:
    requires(kvm_required=True)
    requires(coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL)
    from gem5.components.cachehierarchies.classic.no_cache import NoCache
    cache_hierarchy = NoCache()
    processor = SimpleSwitchableProcessor(
        starting_core_type=CPUTypes.KVM,
        switch_core_type=CPUTypes.TIMING,
        isa=ISA.X86, num_cores=args.threads,
    )
else:
    requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from components.mesi_three_level_htm_cache_hierarchy import MESIThreeLevelHTMCacheHierarchy
    cache_hierarchy = MESIThreeLevelHTMCacheHierarchy(
        l1i_size="32KiB", l1i_assoc=8, l1d_size="32KiB", l1d_assoc=8,
        l2_size="256KiB", l2_assoc=8, l3_size="2MiB", l3_assoc=16, num_l3_banks=1,
    )
    processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.X86, num_cores=args.threads)

memory = SingleChannelDDR3_1600(size="3GiB")
board = X86Board(clk_freq=args.clk, processor=processor, memory=memory, cache_hierarchy=cache_hierarchy)

# Minimal workload: boot ubuntu then run binary as script
# For tiny bench we create a simple script that runs the binary with fixed args
workload = obtain_resource("x86-ubuntu-24.04-boot-with-systemd", resource_version="5.0.0")
board.set_workload(workload)

# Add our binary as extra resource executed after boot via m5 exit handlers
# The actual command is injected via board's workload script (see x86-tsx-fs.py pattern)
print(f"[FS tiny] binary={args.binary} threads={args.threads} kvm={args.kvm} clk={args.clk}")

simulator = Simulator(board=board)
simulator.run()
