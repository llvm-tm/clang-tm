include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

SRC := TPCC.cpp

.PHONY: all clean

all: out bin tpcc_uninstrumented tpcc_singlelock tpcc_tl2 tpcc_tinystm tpcc_persistentsgl

tpcc_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/tpcc_uninstrumented

$(eval $(call tm_target, tpcc_singlelock, $(SRC), singlelock))
$(eval $(call tm_target, tpcc_tl2, $(SRC), tl2))
$(eval $(call tm_target, tpcc_tinystm, $(SRC), tinystm))
$(eval $(call tm_target, tpcc_persistentsgl, $(SRC), persistentsgl))

clean:
	rm -rf out bin
