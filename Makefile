

LLVM-CONFIG:=$(shell ./llvm-config-args.sh)

run: prog
	./prog 2> debug.txt

out.ll: out.bc
	llvm-dis out.bc

prog: out.ll tm_runtime.cpp 
	clang out.ll tm_runtime.cpp -o prog

test.bc: test.cpp
	clang -O0 -emit-llvm -c test.cpp -o test.bc -fno-stack-protector

out.bc: test.bc libTMInstrument.so
	opt -load-pass-plugin=./libTMInstrument.so -passes="tm-instrument" -debug-pass-manager test.bc -o out.bc

libTMInstrument.so: TMInstrumentPass.cpp
	clang++ -fPIC -shared TMInstrumentPass.cpp -o libTMInstrument.so $(LLVM-CONFIG)

