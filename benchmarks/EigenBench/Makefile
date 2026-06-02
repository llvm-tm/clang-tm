TM_PLUGIN := $(abspath ../../llvm_tm_plugin/bin/libTMInstrument.so)
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

SRC := EigenBench.cpp

.PHONY: all clean

all: out bin eigen_uninstrumented eigen_singlelock eigen_tl2 eigen_tinystm \
	eigen_norec eigen_swisstm

eigen_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/eigen_uninstrumented

$(eval $(call tm_target, eigen_singlelock, $(SRC), singlelock))
$(eval $(call tm_target, eigen_tl2, $(SRC), tl2))
$(eval $(call tm_target, eigen_tinystm, $(SRC), tinystm))
$(eval $(call tm_target, eigen_norec, $(SRC), norec))
$(eval $(call tm_target, eigen_swisstm, $(SRC), swisstm))

clean:
	rm -rf out bin
