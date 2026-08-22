from m5.objects import (
    MESI_Three_Level_HTM_DMA_Controller,
    MessageBuffer,
)


class HTMLDMAController(MESI_Three_Level_HTM_DMA_Controller):
    _version = 0

    @classmethod
    def _get_version(cls):
        cls._version += 1
        return cls._version - 1

    def __init__(self, dma_sequencer, ruby_system):
        super().__init__(
            version=self._get_version(),
            dma_sequencer=dma_sequencer,
            ruby_system=ruby_system,
        )
        self.connectQueues(self.ruby_system.network)

    def connectQueues(self, network):
        self.mandatoryQueue = MessageBuffer()
        self.responseFromDir = MessageBuffer(ordered=True)
        self.responseFromDir.in_port = network.out_port
        self.requestToDir = MessageBuffer()
        self.requestToDir.out_port = network.in_port
