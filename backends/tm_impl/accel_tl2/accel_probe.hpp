/**
 * accel_probe.hpp — accelerator detection + zero-copy validation engine
 * for the ACCELTL2 backend.
 *
 * Detects at runtime (nothing is linked at build time):
 *   - OpenCL GPUs (via dlopen of libOpenCL.so.1) that advertise
 *     CL_DEVICE_SVM_FINE_GRAIN_BUFFER, i.e. devices whose kernels can
 *     read/write host-allocated SVM memory with hardware coherence and
 *     no explicit copies.  The TL2 guard table is placed in such memory
 *     so CPU and GPU share one copy of the conflict-detection state.
 *   - NPUs (AMD XDNA via /dev/accel/accel*, Intel/other via sysfs) —
 *     detected and REPORTED ONLY.  XDNA-class NPUs program pre-compiled
 *     tile graphs over DMA buffers; they cannot dereference arbitrary
 *     shared host pointers nor atomically update shared lock state, so
 *     they are unsuitable for transaction validation.  Detection is
 *     surfaced so users/CI can see why they are not used.
 *
 * Environment knobs:
 *   TM_ACCEL_DISABLE=1   never use the accelerator (pure CPU path)
 *   TM_ACCEL_MIN=<n>     read-set size at/above which GPU prevalidate runs
 *                        (default 4096; measured crossover on a Radeon
 *                        8060S iGPU is ~5k-10k entries — kernel round
 *                        trip costs ~17 us, CPU scans ~2-4 ns/entry)
 *   TM_ACCEL_VERBOSE=1   print the detection report at init
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace accel
{

// Detect usable accelerator. Idempotent; returns availability.
bool probe_available();

// True iff the guard table was (or is about to be) placed in
// device-shared memory.
bool guards_shared();

// Allocate an n-entry guard table in device-shared memory, or nullptr if
// no accelerator is available (caller keeps its own static storage).
std::atomic<uint64_t> *shared_guard_table(size_t n_entries);

// Read-set size at which prevalidate is worth dispatching.
uint64_t min_entries();

// Run a read-set conflict pre-scan on the accelerator.
// entries: n packed pairs (guard_idx, observed_version).
// Returns true when the accelerator definitively observed a conflict
// (locked guard or changed version).  A false return means "no conflict
// seen OR not dispatched" — callers must still validate normally.
bool prevalidate(const uint64_t *entries, size_t n, std::atomic<uint64_t> *guards);

// Release accelerator resources.
void shutdown();

} // namespace accel
