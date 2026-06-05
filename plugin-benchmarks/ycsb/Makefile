TM_PLUGIN := $(abspath ../../llvm_tm_plugin/bin/libTMInstrument.so)
TM_INSTRUMENT_FLAGS = -tm-allow-opaque
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

SRC := YCSB.cpp

.PHONY: all clean

all: out bin ycsb_uninstrumented ycsb_singlelock ycsb_tl2 ycsb_tinystm \
	ycsb_tsxsgl ycsb_dudetm ycsb_spht \
	ycsb_tinystm_wbctl ycsb_tinystm_wt ycsb_norec

ycsb_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/ycsb_uninstrumented

$(eval $(call tm_target, ycsb_singlelock, $(SRC), singlelock))
$(eval $(call tm_target, ycsb_tl2, $(SRC), tl2))
$(eval $(call tm_target, ycsb_tinystm, $(SRC), tinystm))
$(eval $(call tm_target, ycsb_tsxsgl, $(SRC), tsxsgl))
$(eval $(call tm_target, ycsb_dudetm, $(SRC), dudetm))
$(eval $(call tm_target, ycsb_spht, $(SRC), spht))
$(eval $(call tm_target, ycsb_tinystm_wbctl, $(SRC), tinystm_wbctl))
$(eval $(call tm_target, ycsb_tinystm_wt, $(SRC), tinystm_wt))
$(eval $(call tm_target, ycsb_norec, $(SRC), norec))

clean:
	rm -rf out bin
