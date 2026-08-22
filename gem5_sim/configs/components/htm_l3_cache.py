import math

from m5.objects import (
    MESI_Three_Level_HTM_L2Cache_Controller,
    MessageBuffer,
    RubyCache,
)


class HTML3Cache(MESI_Three_Level_HTM_L2Cache_Controller):
    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1
        return cls._version - 1

    def __init__(
        self,
        l3_size,
        l3_assoc,
        network,
        num_l3Caches,
        cache_line_size,
        cluster_id,
    ):
        super().__init__()

        self.L2cache = RubyCache(
            size=l3_size,
            assoc=l3_assoc,
            start_index_bit=self.getIndexBit(num_l3Caches, cache_line_size),
        )

        self.transitions_per_cycle = 4
        self.cluster_id = cluster_id
        self.l2_request_latency = 2
        self.l2_response_latency = 2
        self.to_l1_latency = 1

        self.version = self.versionCount()
        self.connectQueues(network)

    def getIndexBit(self, num_l3Caches, cache_line_size):
        l3_bits = int(math.log(num_l3Caches, 2))
        bits = int(math.log(cache_line_size, 2)) + l3_bits
        return bits

    def connectQueues(self, network):
        self.DirRequestFromL2Cache = MessageBuffer()
        self.DirRequestFromL2Cache.out_port = network.in_port
        self.L1RequestFromL2Cache = MessageBuffer()
        self.L1RequestFromL2Cache.out_port = network.in_port
        self.responseFromL2Cache = MessageBuffer()
        self.responseFromL2Cache.out_port = network.in_port
        self.unblockToL2Cache = MessageBuffer()
        self.unblockToL2Cache.in_port = network.out_port
        self.L1RequestToL2Cache = MessageBuffer()
        self.L1RequestToL2Cache.in_port = network.out_port
        self.responseToL2Cache = MessageBuffer()
        self.responseToL2Cache.in_port = network.out_port
