#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <csetjmp>

void longjmp( std::jmp_buf env, int status );

#define TM_BUFFER_SIZE 1024

thread_local std::jmp_buf tm_jmpbuf = {0};
// If this counter is 1 then it is a normal transaction, >1 is a nested transaction
thread_local int32_t tm_nested_call_counter = 0;

thread_local uint8_t tm_buffer[TM_BUFFER_SIZE] = {0};


extern "C" void tm_begin() {
	printf("tm_begin\n");
}

extern "C" void tm_end() {
	printf("tm_end\n");
}

extern "C" int8_t tm_read_i1(void* addr) {
	int8_t val = *(int8_t*)addr;
	printf("tm_read_i1(%p) = %d\n", addr, val);
	return val;
}

extern "C" int16_t tm_read_i2(void* addr) {
	int16_t val = *(int16_t*)addr;
	printf("tm_read_i2(%p) = %d\n", addr, val);
	return val;
}

extern "C" int32_t tm_read_i4(void* addr) {
	int32_t val = *(int32_t*)addr;
	printf("tm_read_i4(%p) = %d\n", addr, val);
	return val;
}

extern "C" int64_t tm_read_i8(void* addr) {
	int64_t val = *(int64_t*)addr;
	printf("tm_read_i8(%p) = %lld\n", addr, val);
	return val;
}

extern "C" float tm_read_f4(void* addr) {
	float val = *(float*)addr;
	printf("tm_read_f4(%p) = %f\n", addr, val);
	return val;
}

extern "C" double tm_read_f8(void* addr) {
	double val = *(double*)addr;
	printf("tm_read_f8(%p) = %lf\n", addr, val);
	return val;
}

extern "C" void* tm_read_ptr(void* addr) {
	void* val = *(void**)addr;
	printf("tm_read_ptr(%p) = %p\n", addr, val);
	return val;
}

extern "C" void* tm_read_z(void* src, size_t sz) { // returns a buffer with the read value
	assert(sz < TM_BUFFER_SIZE);
	memcpy((void*)tm_buffer, src, sz);
	printf("tm_read_z(%p, %zu) = %p\n", src, sz, (void*)tm_buffer);
	return (void*)tm_buffer;
}

extern "C" void tm_write_i1(void* addr, uint8_t val) {
	printf("tm_write_i1(%p, %c)\n", addr, val);
	*(uint8_t*)addr = val;
}

extern "C" void tm_write_i2(void* addr, int16_t val) {
	printf("tm_write_i2(%p, %hd)\n", addr, val);
	*(uint16_t*)addr = val;
}

extern "C" void tm_write_i4(void* addr, int32_t val) {
	printf("tm_write_i4(%p, %d)\n", addr, val);
	*(uint32_t*)addr = val;
}

extern "C" void tm_write_i8(void* addr, int64_t val) {
	printf("tm_write_i8(%p, %lld)\n", addr, val);
	*(uint64_t*)addr = val;
}

extern "C" void tm_write_f4(void* addr, float val) {
	printf("tm_write_f4(%p, %f)\n", addr, val);
	*(float*)addr = val;
}

extern "C" void tm_write_f8(void* addr, double val) {
	printf("tm_write_f8(%p, %lf)\n", addr, val);
	*(double*)addr = val;
}

extern "C" void tm_write_ptr(void* addr, void* val) {
	printf("tm_write_ptr(%p, %p)\n", addr, val);
	*(void**)addr = val;
}

extern "C" void tm_write_z(/*tm_memory*/void* dst, void* src, size_t sz) {
	assert(sz < TM_BUFFER_SIZE);
	memcpy(dst, src, sz);
	printf("tm_write_z(%p, %p, %zu)\n", dst, src, sz);
}

