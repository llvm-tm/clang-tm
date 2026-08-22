"""
POWER8 HTM simulation in gem5.

This config script runs a POWER-based simulation with HTM support
using the MESI_Three_Level_HTM Ruby protocol.

NOTE: POWER full-system support in gem5 is limited. This configuration
is intended for syscall emulation (SE) mode experiments.

Usage:
  ./build/POWER/gem5.opt configs/power8-htm.py
"""

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.requires import requires

requires(
    coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM,
)

from components.mesi_three_level_htm_cache_hierarchy import (
    MESIThreeLevelHTMCacheHierarchy,
)

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
    cpu_type=CPUTypes.TIMING,
    isa=ISA.POWER,
    num_cores=2,
)

board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

binary_path = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "workloads", "power8", "array_sum"
)
board.set_se_binary_workload(
    BinaryResource(local_path=binary_path),
    arguments=[],
)

simulator = Simulator(board=board)
simulator.run()
