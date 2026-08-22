import math

from m5.objects import (
    LRURP,
    ClockDomain,
    MESI_Three_Level_HTM_L0Cache_Controller,
    MessageBuffer,
    RubyCache,
    RubyHTMSequencer,
    RubyPrefetcher,
)

from gem5.isas import ISA
from gem5.utils.override import *
from gem5.components.processors.abstract_core import AbstractCore


class HTML1Cache(MESI_Three_Level_HTM_L0Cache_Controller):
    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1
        return cls._version - 1

    def __init__(
        self,
        l1i_size,
        l1i_assoc,
        l1d_size,
        l1d_assoc,
        network,
        core: AbstractCore,
        cache_line_size,
        target_isa: ISA,
        clk_domain: ClockDomain,
    ):
        super().__init__()

        self.Icache = RubyCache(
            size=l1i_size,
            assoc=l1i_assoc,
            start_index_bit=self.getBlockSizeBits(cache_line_size),
            is_icache=True,
            replacement_policy=LRURP(),
        )
        self.Dcache = RubyCache(
            size=l1d_size,
            assoc=l1d_assoc,
            start_index_bit=self.getBlockSizeBits(cache_line_size),
            is_icache=False,
            replacement_policy=LRURP(),
        )
        self.clk_domain = clk_domain
        self.prefetcher = RubyPrefetcher(block_size=cache_line_size)
        self.send_evictions = core.requires_send_evicts()
        self.transitions_per_cycle = 32
        self.enable_prefetch = False
        self.request_latency = 2
        self.response_latency = 2

        self.version = self.versionCount()
        self.connectQueues(network)

    def getBlockSizeBits(self, cache_line_size):
        bits = int(math.log(cache_line_size, 2))
        if 2**bits != int(cache_line_size):
            raise Exception("Cache line size is not a power of 2!")
        return bits

    def connectQueues(self, network):
        self.prefetchQueue = MessageBuffer()
        self.mandatoryQueue = MessageBuffer()
        self.optionalQueue = MessageBuffer()

        self.bufferToL1 = MessageBuffer(ordered=True)
        self.bufferFromL1 = MessageBuffer(ordered=True)
