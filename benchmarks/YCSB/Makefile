include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

SRC := YCSB.cpp

.PHONY: all clean

all: out bin ycsb_uninstrumented ycsb_singlelock ycsb_tl2 ycsb_tinystm

ycsb_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/ycsb_uninstrumented

$(eval $(call tm_target, ycsb_singlelock, $(SRC), singlelock))
$(eval $(call tm_target, ycsb_tl2, $(SRC), tl2))
$(eval $(call tm_target, ycsb_tinystm, $(SRC), tinystm))

clean:
	rm -rf out bin
