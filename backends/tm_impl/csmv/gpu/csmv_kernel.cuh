#pragma once

#include <cstdint>
#include "csmv_api.h"

#ifdef __CUDACC__

// ── Warp-cooperative version list traversal ─────────────────────
// CSMV on GPU uses warp-level primitives to traverse version lists
// in parallel.  Each warp owns the read/write sets of its lanes.
//
// Version list layout (device memory):
//   Each CSMVObjectEntry lives in global memory.
//   head pointer → newest VersionNode → ... → oldest VersionNode.
//
// Read path (warp-cooperative):
//   1. Lane 0 loads the head pointer.
//   2. All lanes traverse shared memory: for each node in the list,
//      lane i checks node[i % warpSize] timestamp ≤ startTime.
//   3. __ballot_sync to find the first lane whose node matches.
//   4. __shfl_sync to broadcast the matching value.
//
// Write path:
//   1. Buffer in per-lane write-set (shared memory).
//   2. At commit, warp leader creates a new VersionNode with
//      the commit timestamp and prepends it to the list.

// Launch parameters
#define CSMV_GPU_WARP_SIZE  32

// Kernel launch
int csmv_gpu_launch(int num_warps,
                     void (*tx_body)(int lane_id, int warp_id,
                                     void *data, void *shared_scratch),
                     void *tx_data);

#endif // __CUDACC__
