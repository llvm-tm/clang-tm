CXXFLAGS ?= -g -std=c++20
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

SRC := STMbench7.cpp

.PHONY: all clean

all: out bin stmbench_uninstrumented stmbench_singlelock stmbench_tl2 stmbench_tinystm

stmbench_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/stmbench_uninstrumented

$(eval $(call tm_target, stmbench_singlelock, $(SRC), singlelock))
$(eval $(call tm_target, stmbench_tl2, $(SRC), tl2))
$(eval $(call tm_target, stmbench_tinystm, $(SRC), tinystm))

clean:
	rm -rf out bin
