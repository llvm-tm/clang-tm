"""
ARM TME simulation with KVM fast-forward and checkpoint workflow.

This script demonstrates the recommended fast simulation workflow:
  1. Boot the OS with KVM CPU (native speed, ~20s)
  2. Switch to Timing/O3 CPU for detailed TME simulation
  3. Optionally take a checkpoint after boot for repeated runs

Usage:
  ./build/ALL/gem5.opt configs/arm-tme-kvm.py
"""

from m5.objects import ArmDefaultRelease, VExpress_GEM5_V1

from gem5.components.boards.arm_board import ArmBoard
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.exit_handler import (
    AfterBootExitHandler,
    ExitHandler,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides
from gem5.utils.requires import requires

requires(
    coherence_protocol_required="MESI_Three_Level_HTM",
    kvm_required=True,
)

from configs.components.mesi_three_level_htm_cache_hierarchy import (
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

memory = DualChannelDDR4_2400(size="3GiB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.ARM,
    num_cores=2,
)

release = ArmDefaultRelease.for_kvm()
platform = VExpress_GEM5_V1()

board = ArmBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    release=release,
    platform=platform,
)

workload = obtain_resource(
    "arm-ubuntu-24.04-boot-with-systemd", resource_version="3.0.0"
)
board.set_workload(workload)


class CustomKernelBootedExitHandler(ExitHandler, hypercall_num=1):
    @overrides(ExitHandler)
    def _process(self, simulator: "Simulator") -> None:
        print("First exit: kernel booted")

    @overrides(ExitHandler)
    def _exit_simulation(self) -> bool:
        return False


class SwitchProcessorAfterBootExitHandler(AfterBootExitHandler):
    @overrides(AfterBootExitHandler)
    def _process(self, simulator: "Simulator") -> None:
        print("Second exit: Started after_boot.sh script")
        print("Switching to Timing CPU")
        simulator.switch_processor()

    @overrides(AfterBootExitHandler)
    def _exit_simulation(self) -> bool:
        return False


class AfterBootScriptExitHandler(ExitHandler, hypercall_num=3):
    @overrides(ExitHandler)
    def _process(self, simulator: "Simulator") -> None:
        print(f"Third exit: {self.get_handler_description()}")

    @overrides(ExitHandler)
    def _exit_simulation(self) -> bool:
        return True


simulator = Simulator(board=board)

simulator.run()
