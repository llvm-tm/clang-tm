/**
 * accel_probe.cpp — runtime accelerator discovery + zero-copy TL2
 * read-set pre-validation for the ACCELTL2 backend.
 *
 * Build-time dependencies: none (OpenCL is dlopen'd at runtime; if
 * libOpenCL.so.1 is absent, or no GPU advertises
 * CL_DEVICE_SVM_FINE_GRAIN_BUFFER, everything degrades to the plain
 * CPU path with identical semantics).
 *
 * Why OpenCL here even though the repo's gpu/ kernels are CUDA/HIP:
 * this component must (a) build on any Linux box in CI, (b) discover the
 * device at RUN time, and (c) JIT one tiny validation kernel — exactly
 * where OpenCL ICDs excel and where pre-compiled CUDA/HIP kernels do not.
 *
 * Zero-copy story: on fine-grain-SVM devices (e.g. the AMD Radeon 8060S
 * iGPU on Ryzen AI MAX / gfx1151), clSVMAlloc memory is ordinary host
 * memory that both the CPU atomics and GPU kernels address through the
 * same virtual pointer with hardware coherence.  The TL2 guard table is
 * allocated there, so the device scans the LIVE conflict-detection
 * state — no staging copies.
 *
 * NPU policy: detected and reported, never used.  AMD XDNA / similar
 * NPUs execute pre-compiled tile graphs over DMA buffers; they cannot
 * dereference arbitrary shared host pointers nor atomically observe the
 * guard table, which is the whole requirement here.
 */
#include "accel_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <unistd.h>

#include <dlfcn.h>

namespace accel
{
namespace
{

// ── minimal OpenCL ABI (dlopen'd; no headers/libs at build time) ─
using cl_int = int32_t;
using cl_uint = uint32_t;
using cl_ulong = uint64_t;
using cl_device_type = uint64_t;
using cl_mem_flags = uint64_t;
using cl_svm_mem_flags = uint64_t;
using cl_context_properties = intptr_t;
using cl_command_queue_properties = uint64_t;
using cl_platform_id = void *;
using cl_device_id = void *;
using cl_context = void *;
using cl_command_queue = void *;
using cl_program = void *;
using cl_kernel = void *;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_device_type CL_DEVICE_TYPE_GPU = 1ULL << 2;
constexpr uint64_t CL_DEVICE_NAME = 0x102B;
constexpr uint64_t CL_DEVICE_SVM_CAPABILITIES = 0x1003;
constexpr uint64_t CL_DEVICE_SVM_FINE_GRAIN_BUFFER = 1ULL << 1;
constexpr uint64_t CL_DEVICE_SVM_ATOMICS = 1ULL << 3;
constexpr uint64_t CL_PROGRAM_BUILD_LOG = 0x1006;
constexpr cl_mem_flags CL_MEM_READ_WRITE = 1ULL << 1;
// cl_svm_mem_flags for clSVMAlloc (distinct namespace from cl_mem_flags):
constexpr cl_svm_mem_flags CL_MEM_SVM_FINE_GRAIN_BUFFER = 1ULL << 10;
constexpr cl_svm_mem_flags CL_MEM_SVM_ATOMICS = 1ULL << 11;

struct OpenCLEntryPoints {
	void *lib = nullptr;
	// Core entry points actually used:
	cl_int (*GetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *) = nullptr;
	cl_int (*GetDeviceIDs)(cl_platform_id,
	                       cl_device_type,
	                       cl_uint,
	                       cl_device_id *,
	                       cl_uint *) = nullptr;
	cl_int (*GetDeviceInfo)(cl_device_id, uint64_t, size_t, void *, size_t *) = nullptr;
	cl_context (*CreateContext)(const cl_context_properties *,
	                            cl_uint,
	                            const cl_device_id *,
	                            void (*)(const char *, const void *, size_t, void *),
	                            void *,
	                            cl_int *) = nullptr;
	cl_command_queue (*CreateCommandQueue)(cl_context,
	                                       cl_device_id,
	                                       cl_command_queue_properties,
	                                       cl_int *) = nullptr;
	cl_program (*CreateProgramWithSource)(cl_context,
	                                      cl_uint,
	                                      const char **,
	                                      const size_t *,
	                                      cl_int *) = nullptr;
	cl_int (*BuildProgram)(cl_program,
	                       cl_uint,
	                       const cl_device_id *,
	                       const char *,
	                       void (*)(cl_program, void *),
	                       void *) = nullptr;
	cl_int (*GetProgramBuildInfo)(cl_program,
	                              cl_device_id,
	                              uint64_t,
	                              size_t,
	                              void *,
	                              size_t *) = nullptr;
	cl_kernel (*CreateKernel)(cl_program, const char *, cl_int *) = nullptr;
	void *(*SVMAlloc)(cl_context, cl_svm_mem_flags, size_t, cl_uint) = nullptr;
	void (*SVMFree)(cl_context, void *) = nullptr;
	cl_int (*SetKernelArgSVMPointer)(cl_kernel, cl_uint, const void *) = nullptr;
	cl_int (*SetKernelArg)(cl_kernel, cl_uint, size_t, const void *) = nullptr;
	cl_int (*EnqueueNDRangeKernel)(cl_command_queue,
	                               cl_kernel,
	                               cl_uint,
	                               const size_t *,
	                               const size_t *,
	                               const size_t *,
	                               cl_uint,
	                               const void *const *,
	                               void **) = nullptr;
	cl_int (*Finish)(cl_command_queue) = nullptr;

	bool load()
	{
		lib = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			lib = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			return false;
#define BIND(name)                                                                       \
	name = reinterpret_cast<decltype(name)>(dlsym(lib, "cl" #name));                     \
	if (!name)                                                                           \
		return false;
		BIND(GetPlatformIDs)
		BIND(GetDeviceIDs)
		BIND(GetDeviceInfo)
		BIND(CreateContext)
		BIND(CreateCommandQueue)
		BIND(CreateProgramWithSource)
		BIND(BuildProgram)
		BIND(GetProgramBuildInfo)
		BIND(CreateKernel)
		BIND(SVMAlloc)
		BIND(SVMFree)
		BIND(SetKernelArgSVMPointer)
		BIND(SetKernelArg)
		BIND(EnqueueNDRangeKernel)
		BIND(Finish)
#undef BIND
		return true;
	}
};

// One work-item per read-set entry: a locked guard or a version change
// is a conflict.  Guard encoding (tl2.hpp): bit0 = LOCK, bits[63:1] =
// version stored as version<<1 — hence (g >> 1) is the version.
constexpr const char
    *KSRC = "typedef struct { ulong idx; ulong ver; } RDE;\n"
            "__kernel void tl2_prevalidate(__global RDE *rdlog, __global ulong *guards,\n"
            "                              __global int *conflict, ulong n)\n"
            "{\n"
            "\tulong i = get_global_id(0);\n"
            "\tif (i >= n) return;\n"
            "\tulong g = guards[rdlog[i].idx];\n"
            "\tif ((g & (ulong)1) || ((g >> 1) != rdlog[i].ver))\n"
            "\t\tatomic_max(conflict, 1);\n"
            "}\n";

struct State {
	bool probed = false;
	bool available = false;
	bool verbose = false;
	uint64_t min = 4096;
	uint64_t svm_caps = 0;
	size_t cap = 1u << 20; // max read-set entries per dispatch

	OpenCLEntryPoints cl;
	cl_platform_id platform = nullptr;
	cl_device_id device = nullptr;
	cl_context ctx = nullptr;
	cl_command_queue queue = nullptr;
	cl_program prog = nullptr;
	cl_kernel kernel = nullptr;

	void *guards_svm = nullptr;
	void *log_svm = nullptr;
	int *flag_svm = nullptr;

	// stats (verbose reporting at shutdown): dispatches, conflicts found
	uint64_t n_dispatch = 0;
	uint64_t n_conflict = 0;

	std::mutex dispatch_mu; // one queue: serialize dispatches
};

State g;

bool env_flag(const char *k)
{
	const char *v = getenv(k);
	return v && *v && strcmp(v, "0") != 0;
}

uint64_t env_num(const char *k, uint64_t dflt)
{
	const char *v = getenv(k);
	if (!v || !*v)
		return dflt;
	char *end = nullptr;
	unsigned long long x = strtoull(v, &end, 10);
	return (end && end != v) ? (uint64_t)x : dflt;
}

void report_npus()
{
	if (!g.verbose)
		return;
	DIR *d = opendir("/sys/class/accel");
	if (!d) {
		printf("[accel] NPU: no /sys/class/accel — no NPU class device present\n");
		return;
	}
	struct dirent *de;
	int found = 0;
	while ((de = readdir(d)) != nullptr) {
		if (strncmp(de->d_name, "accel", 5) != 0)
			continue;
		++found;
		char path[256];
		snprintf(path, sizeof(path), "/sys/class/accel/%s/device/driver", de->d_name);
		char link[256];
		ssize_t n = readlink(path, link, sizeof(link) - 1);
		if (n > 0) {
			link[n] = '\0';
			const char *base = strrchr(link, '/');
			printf("[accel] NPU: %s driver '%s' @ /dev/accel/%s — detected but NOT used: "
			       "NPU-class devices (tile graphs over DMA buffers) cannot dereference "
			       "shared host pointers or atomically observe the guard table.\n",
			       de->d_name,
			       base ? base + 1 : link,
			       de->d_name);
		} else {
			printf("[accel] NPU: /sys/class/accel/%s present (driver unknown) — not "
			       "used.\n",
			       de->d_name);
		}
	}
	if (!found)
		printf("[accel] NPU: no accel class devices found\n");
	closedir(d);
}

void do_probe()
{
	g.probed = true;
	g.verbose = env_flag("TM_ACCEL_VERBOSE");
	g.min = env_num("TM_ACCEL_MIN", 4096);

	if (env_flag("TM_ACCEL_DISABLE")) {
		if (g.verbose)
			printf("[accel] disabled via TM_ACCEL_DISABLE\n");
		return;
	}
	if (!g.cl.load()) {
		if (g.verbose)
			printf("[accel] no libOpenCL.so.1 present — CPU path only\n");
		report_npus();
		return;
	}

	cl_uint npf = 0;
	if (g.cl.GetPlatformIDs(0, nullptr, &npf) != CL_SUCCESS || npf == 0) {
		if (g.verbose)
			printf("[accel] no OpenCL platforms — CPU path only\n");
		report_npus();
		return;
	}
	for (cl_uint p = 0; p < npf && !g.available; p++) {
		cl_platform_id plats[8];
		if (g.cl.GetPlatformIDs(8, plats, nullptr) != CL_SUCCESS)
			break;
		g.platform = plats[p];
		cl_uint nd = 0;
		if (g.cl.GetDeviceIDs(g.platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &nd) !=
		    CL_SUCCESS)
			continue;
		for (cl_uint d = 0; d < nd && !g.available; d++) {
			cl_device_id dev;
			if (g.cl.GetDeviceIDs(g.platform, CL_DEVICE_TYPE_GPU, 1, &dev, nullptr) !=
			    CL_SUCCESS)
				break;
			g.device = dev;
			uint64_t svm = 0;
			if (g.cl.GetDeviceInfo(dev,
			                       CL_DEVICE_SVM_CAPABILITIES,
			                       sizeof(svm),
			                       &svm,
			                       nullptr) != CL_SUCCESS)
				continue;
			if (!(svm & CL_DEVICE_SVM_FINE_GRAIN_BUFFER)) {
				if (g.verbose)
					printf("[accel] OpenCL GPU %u lacks fine-grain SVM — not eligible\n",
					       d);
				continue;
			}
			char dname[128] = "?";
			g.cl.GetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dname), dname, nullptr);
			cl_int e = 0;
			g.ctx = g.cl.CreateContext(nullptr, 1, &dev, nullptr, nullptr, &e);
			if (e != CL_SUCCESS)
				continue;
			g.queue = g.cl.CreateCommandQueue(g.ctx, dev, 0, &e);
			if (e != CL_SUCCESS)
				continue;
			const char *src = KSRC;
			size_t len = strlen(src);
			g.prog = g.cl.CreateProgramWithSource(g.ctx, 1, &src, &len, &e);
			if (e != CL_SUCCESS)
				continue;
			if (g.cl.BuildProgram(g.prog, 1, &dev, nullptr, nullptr, nullptr) !=
			    CL_SUCCESS) {
				if (g.verbose) {
					char log[4096];
					if (g.cl.GetProgramBuildInfo(g.prog,
					                             dev,
					                             CL_PROGRAM_BUILD_LOG,
					                             sizeof(log),
					                             log,
					                             nullptr) == CL_SUCCESS)
						printf("[accel] kernel build failed:\n%s\n", log);
				}
				continue;
			}
			g.kernel = g.cl.CreateKernel(g.prog, "tl2_prevalidate", &e);
			if (e != CL_SUCCESS)
				continue;
			g.available = true;
			if (g.verbose)
				printf("[accel] GPU '%s': fine-grain SVM OK — guard table will be shared "
				       "zero-copy; prevalidate threshold n>=%llu\n",
				       dname,
				       (unsigned long long)g.min);
		}
	}
	if (!g.available && g.verbose)
		printf("[accel] no eligible GPU accelerator — CPU path only\n");
	report_npus();
}

} // namespace

bool probe_available()
{
	static std::once_flag once;
	std::call_once(once, do_probe);
	return g.available;
}

bool guards_shared() { return probe_available() && g.guards_svm != nullptr; }

std::atomic<uint64_t> *shared_guard_table(size_t n_entries)
{
	if (!probe_available() || g.guards_svm)
		return reinterpret_cast<std::atomic<uint64_t> *>(g.guards_svm);
	// CL_MEM_SVM_FINE_GRAIN_BUFFER as an ALLOCATION flag is required to
	// actually get fine-grain memory: without it the runtime may fall
	// back to coarse-grain placement, whose CPU-side mapping is uncached
	// on at least the AMD ICD (measured: guard-table loads ~160 ns vs
	// ~2 ns cached — i.e. a 30-80x backend-wide slowdown).
	// Alignment 0 = let the runtime choose (explicit large alignments are
	// rejected by some ICDs, e.g. AMD's for gfx1151).
	g.guards_svm = g.cl.SVMAlloc(g.ctx,
	                             CL_MEM_READ_WRITE | CL_MEM_SVM_FINE_GRAIN_BUFFER,
	                             n_entries * sizeof(uint64_t),
	                             0);
	if (!g.guards_svm) {
		g.available = false;
		if (g.verbose)
			printf("[accel] clSVMAlloc(guards) failed — falling back to CPU path\n");
		return nullptr;
	}
	// Read-set staging + conflict flag, also device-visible (fine-grain
	// so the CPU-packed log is live without migration; the kernel's
	// atomic_max is a device-plain atomic on its own buffer, legal
	// without CL_DEVICE_SVM_ATOMICS).
	g.log_svm = g.cl.SVMAlloc(g.ctx,
	                          CL_MEM_READ_WRITE | CL_MEM_SVM_FINE_GRAIN_BUFFER,
	                          g.cap * 2 * sizeof(uint64_t),
	                          0);
	g.flag_svm = (int *)g.cl.SVMAlloc(g.ctx, CL_MEM_READ_WRITE, sizeof(int), 0);
	if (!g.log_svm || !g.flag_svm) {
		g.available = false;
		return nullptr;
	}
	return reinterpret_cast<std::atomic<uint64_t> *>(g.guards_svm);
}

uint64_t min_entries() { return g.min; }

bool prevalidate(const uint64_t *entries, size_t n, std::atomic<uint64_t> *guards)
{
	if (!g.available || !g.guards_svm || n < g.min || n > g.cap)
		return false;
	if (reinterpret_cast<void *>(guards) != g.guards_svm)
		return false; // guard table never went shared

	std::lock_guard<std::mutex> lk(g.dispatch_mu);
	memcpy(g.log_svm, entries, n * 2 * sizeof(uint64_t)); // CPU store, no DMA copy
	*g.flag_svm = 0;

	cl_int e = 0;
	e |= g.cl.SetKernelArgSVMPointer(g.kernel, 0, g.log_svm);
	e |= g.cl.SetKernelArgSVMPointer(g.kernel, 1, g.guards_svm);
	e |= g.cl.SetKernelArgSVMPointer(g.kernel, 2, g.flag_svm);
	uint64_t nn = n;
	e |= g.cl.SetKernelArg(g.kernel, 3, sizeof(nn), &nn);
	if (e != CL_SUCCESS)
		return false;
	size_t gsz = ((n + 63) / 64) * 64;
	e = g.cl.EnqueueNDRangeKernel(g.queue,
	                              g.kernel,
	                              1,
	                              nullptr,
	                              &gsz,
	                              nullptr,
	                              0,
	                              nullptr,
	                              nullptr);
	if (e != CL_SUCCESS)
		return false;
	if (g.cl.Finish(g.queue) != CL_SUCCESS)
		return false;
	++g.n_dispatch;
	bool conflict = *g.flag_svm != 0;
	if (conflict)
		++g.n_conflict;
	return conflict;
}

void shutdown()
{
	if (!g.probed)
		return;
	if (g.verbose)
		printf("[accel] stats: %llu GPU prevalidations dispatched, %llu conflicts "
		       "found\n",
		       (unsigned long long)g.n_dispatch,
		       (unsigned long long)g.n_conflict);
	if (g.guards_svm)
		g.cl.SVMFree(g.ctx, g.guards_svm);
	if (g.log_svm)
		g.cl.SVMFree(g.ctx, g.log_svm);
	if (g.flag_svm)
		g.cl.SVMFree(g.ctx, g.flag_svm);
	g.guards_svm = g.log_svm = nullptr;
	g.flag_svm = nullptr;
	if (g.kernel)
		g.cl.Finish(g.queue);
	g.available = false;
}

} // namespace accel
