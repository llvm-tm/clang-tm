from m5.objects import (
    DMASequencer,
    RubyPortProxy,
    RubyHTMSequencer,
    RubySystem,
)

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.override import overrides
from gem5.utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM)

from gem5.isas import ISA
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.abstract_three_level_cache_hierarchy import (
    AbstractThreeLevelCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import SimplePt2Pt

from .htm_l1_cache import HTML1Cache
from .htm_l2_cache import HTML2Cache
from .htm_l3_cache import HTML3Cache
from .htm_directory import HTMLDirectory
from .htm_dma_controller import HTMLDMAController


class MESIThreeLevelHTMCacheHierarchy(
    AbstractRubyCacheHierarchy, AbstractThreeLevelCacheHierarchy
):

    def __init__(
        self,
        l1i_size: str,
        l1i_assoc: int,
        l1d_size: str,
        l1d_assoc: int,
        l2_size: str,
        l2_assoc: int,
        l3_size: str,
        l3_assoc: int,
        num_l3_banks: int,
    ):
        AbstractRubyCacheHierarchy.__init__(self=self)
        AbstractThreeLevelCacheHierarchy.__init__(
            self,
            l1i_size=l1i_size,
            l1i_assoc=l1i_assoc,
            l1d_size=l1d_size,
            l1d_assoc=l1d_assoc,
            l2_size=l2_size,
            l2_assoc=l2_assoc,
            l3_size=l3_size,
            l3_assoc=l3_assoc,
        )

        self._num_l3_banks = num_l3_banks

    @overrides(AbstractCacheHierarchy)
    def get_coherence_protocol(self):
        return CoherenceProtocol.MESI_THREE_LEVEL_HTM

    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        cache_line_size = board.get_cache_line_size()

        self.ruby_system = RubySystem()

        self.ruby_system.number_of_virtual_networks = 3

        self.ruby_system.network = SimplePt2Pt(self.ruby_system)
        self.ruby_system.network.number_of_virtual_networks = 3

        self._l1_controllers = []
        self._l2_controllers = []
        self._l3_controllers = []
        cores = board.get_processor().get_cores()
        for core_idx, core in enumerate(cores):
            l1_cache = HTML1Cache(
                l1i_size=self._l1i_size,
                l1i_assoc=self._l1i_assoc,
                l1d_size=self._l1d_size,
                l1d_assoc=self._l1d_assoc,
                network=self.ruby_system.network,
                core=core,
                cache_line_size=cache_line_size,
                target_isa=board.processor.get_isa(),
                clk_domain=board.get_clock_domain(),
            )

            l1_cache.sequencer = RubyHTMSequencer(
                version=core_idx,
                dcache=l1_cache.Dcache,
                clk_domain=l1_cache.clk_domain,
                ruby_system=self.ruby_system,
            )

            if board.has_io_bus():
                l1_cache.sequencer.connectIOPorts(board.get_io_bus())

            l1_cache.ruby_system = self.ruby_system

            core.connect_icache(l1_cache.sequencer.in_ports)
            core.connect_dcache(l1_cache.sequencer.in_ports)

            core.connect_walker_ports(
                l1_cache.sequencer.in_ports, l1_cache.sequencer.in_ports
            )

            if board.get_processor().get_isa() == ISA.X86:
                int_req_port = l1_cache.sequencer.interrupt_out_port
                int_resp_port = l1_cache.sequencer.in_ports
                core.connect_interrupt(int_req_port, int_resp_port)
            else:
                core.connect_interrupt()

            self._l1_controllers.append(l1_cache)

            l2_cache = HTML2Cache(
                l2_size=self._l2_size,
                l2_assoc=self._l2_assoc,
                network=self.ruby_system.network,
                core=core,
                num_l3Caches=self._num_l3_banks,
                cache_line_size=cache_line_size,
                cluster_id=0,
                target_isa=board.processor.get_isa(),
                clk_domain=board.get_clock_domain(),
            )

            l2_cache.ruby_system = self.ruby_system
            l2_cache.bufferFromL0 = l1_cache.bufferToL1
            l2_cache.bufferToL0 = l1_cache.bufferFromL1

            self._l2_controllers.append(l2_cache)

        for _ in range(self._num_l3_banks):
            l3_cache = HTML3Cache(
                l3_size=self._l3_size,
                l3_assoc=self._l3_assoc,
                network=self.ruby_system.network,
                num_l3Caches=self._num_l3_banks,
                cache_line_size=cache_line_size,
                cluster_id=0,
            )
            l3_cache.ruby_system = self.ruby_system
            self._l3_controllers.append(l3_cache)

        for cache in self._l3_controllers:
            cache.ruby_system = self.ruby_system

        self._directory_controllers = [
            HTMLDirectory(self.ruby_system.network, cache_line_size, range, port)
            for range, port in board.get_mem_ports()
        ]
        for dir in self._directory_controllers:
            dir.ruby_system = self.ruby_system

        self._dma_controllers = []
        if board.has_dma_ports():
            dma_ports = board.get_dma_ports()
            for i, port in enumerate(dma_ports):
                ctrl = HTMLDMAController(
                    DMASequencer(
                        version=i,
                        in_ports=port,
                        ruby_system=self.ruby_system,
                    ),
                    self.ruby_system,
                )
                self._dma_controllers.append(ctrl)

        self.ruby_system.num_of_sequencers = len(self._l1_controllers) + len(
            self._dma_controllers
        )
        self.ruby_system.l1_controllers = self._l1_controllers
        self.ruby_system.l2_controllers = self._l2_controllers
        self.ruby_system.l3_controllers = self._l3_controllers
        self.ruby_system.directory_controllers = self._directory_controllers

        if len(self._dma_controllers) != 0:
            self.ruby_system.dma_controllers = self._dma_controllers

        self.ruby_system.network.connectControllers(
            self._l1_controllers
            + self._l2_controllers
            + self._l3_controllers
            + self._directory_controllers
            + self._dma_controllers
        )
        self.ruby_system.network.setup_buffers()

        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    @overrides(AbstractRubyCacheHierarchy)
    def _reset_version_numbers(self):
        HTMLDirectory._version = 0
        HTML1Cache._version = 0
        HTML2Cache._version = 0
        HTML3Cache._version = 0
        HTMLDMAController._version = 0
