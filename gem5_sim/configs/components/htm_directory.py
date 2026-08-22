from m5.objects import (
    MESI_Three_Level_HTM_Directory_Controller,
    MessageBuffer,
    RubyDirectoryMemory,
)

from gem5.utils.override import overrides


class HTMLDirectory(MESI_Three_Level_HTM_Directory_Controller):
    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1
        return cls._version - 1

    def __init__(self, network, cache_line_size, mem_range, port):
        super().__init__()
        self.version = self.versionCount()
        self.addr_ranges = [mem_range]
        self.directory = RubyDirectoryMemory(block_size=cache_line_size)
        self.memory_out_port = port
        self.connectQueues(network=network)

    def connectQueues(self, network):
        self.requestToDir = MessageBuffer()
        self.requestToDir.in_port = network.out_port
        self.responseToDir = MessageBuffer()
        self.responseToDir.in_port = network.out_port
        self.responseFromDir = MessageBuffer()
        self.responseFromDir.out_port = network.in_port
        self.requestToMemory = MessageBuffer()
        self.responseFromMemory = MessageBuffer()
