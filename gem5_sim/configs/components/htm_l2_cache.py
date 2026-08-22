import math

from m5.objects import (
    ClockDomain,
    MESI_Three_Level_HTM_L1Cache_Controller,
    MessageBuffer,
    RubyCache,
    RubyPrefetcher,
)

from gem5.isas import ISA
from gem5.utils.override import *
from gem5.components.processors.abstract_core import AbstractCore


class HTML2Cache(MESI_Three_Level_HTM_L1Cache_Controller):
    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1
        return cls._version - 1

    def __init__(
        self,
        l2_size,
        l2_assoc,
        network,
        core: AbstractCore,
        num_l3Caches,
        cache_line_size,
        cluster_id,
        target_isa: ISA,
        clk_domain: ClockDomain,
    ):
        super().__init__()

        self.cache = RubyCache(
            size=l2_size,
            assoc=l2_assoc,
            start_index_bit=self.getBlockSizeBits(cache_line_size),
            is_icache=False,
        )
        self.l2_select_num_bits = int(math.log(num_l3Caches, 2))
        self.cluster_id = cluster_id
        self.clk_domain = clk_domain
        self.prefetcher = RubyPrefetcher(block_size=cache_line_size)
        self.transitions_per_cycle = 32
        self.l1_request_latency = 2
        self.l1_response_latency = 2
        self.to_l2_latency = 1

        self.version = self.versionCount()
        self.connectQueues(network)

    def getBlockSizeBits(self, cache_line_size):
        bits = int(math.log(cache_line_size, 2))
        if 2**bits != int(cache_line_size):
            raise Exception("Cache line size is not a power of 2!")
        return bits

    def connectQueues(self, network):
        self.mandatoryQueue = MessageBuffer()
        self.optionalQueue = MessageBuffer()
        self.bufferToL0 = MessageBuffer(ordered=True)
        self.bufferFromL0 = MessageBuffer(ordered=True)

        self.requestFromL2 = MessageBuffer()
        self.requestFromL2.in_port = network.out_port
        self.requestToL2 = MessageBuffer()
        self.requestToL2.out_port = network.in_port

        self.responseFromL2 = MessageBuffer()
        self.responseFromL2.in_port = network.out_port
        self.responseToL2 = MessageBuffer()
        self.responseToL2.out_port = network.in_port

        self.unblockToL2 = MessageBuffer()
        self.unblockToL2.out_port = network.in_port
