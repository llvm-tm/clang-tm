#pragma once
#include "tm_gpu_platform.hpp"
#include "gpu_gust_api.h"
#include <cstdint>

// ── GUST GPU kernel ──────────────────────────────────────────────
//
// Multi-Version Concurrency Control (Nunes, Castro, Romano — IST /
// INESC-ID).  Core structures:
//
//   VBox      — per-address Versioned Box: circular array of the most
//               recent committed versions, newest at (head-1)%DEPTH.
//               head is an atomic, monotonic append counter.
//   GTS       — global timestamp; logical clock counting FINALIZED
//               CL slots (committed or aborted).  Transactions take a
//               snapshot of GTS at begin → startTS.
//   CL        — bounded circular Commit Log.  Each update transaction
//               reserves a slot via AtomicINC on a global writePtr and
//               records its write-set there for CCT validation.
//
// Commit protocol (per warp, one transaction per lane):
//   1. READ (snapshot): each lane reads its addresses, recording the
//      newest version ≤ startTS into its read-set.
//   2. WRITE buffer: each lane builds its private write-set.
//   3. PRE-VALIDATION: intra-warp conflicts detected via ballot.
//      If a lower-numbered lane touches (reads or writes) an address
//      this lane writes, the higher lane aborts early.
//   4. CL INSERTION: warp leader AtomicINCs writePtr by WARP_SIZE,
//      broadcasts the base; CTS = base + lane.  Lane writes its
//      state + write-set into CL[CTS % CL_SIZE].
//   5. VALIDATION (hybrid CCT + MRV): scan valPtr from CTS-1 downward.
//      For valPtr ≥ GTS the preceding transaction may still be in
//      flight → CCT: compare our read-set against its CL write-set.
//      Once valPtr < GTS all earlier slots are finalized → MRV:
//      check no VBox we read has a version newer than startTS.
//   6. WRITE-BACK: append new versions to VBoxes, then the warp
//      leader waits until GTS == base and advances GTS by WARP_SIZE,
//      atomically publishing the whole batch.
//
// AtomicINC (never fails) replaces the CAS-based commit-log append of
// classic MVCC (JVSTM/CSMV), which serializes under massive GPU
// parallelism.
//
// This is a single-pass kernel (not persistent).  Caller launches one
// block per warp; blockDim.x must equal WARP_SIZE.

// ── device helpers ──────────────────────────────────────────────

GPU_GUST_DEVICE
inline uint64_t gpu_gust_vbox_version(const GUSTVBox *vb, int slot) {
    return vb->versions[slot];
}

// Snapshot read: the MAXIMUM version <= threshold across ALL slots
// (versions are stored as CTS+1; 0 = empty sentinel).  Concurrent
// write-backs append out of version order, so scanning head-first and
// stopping at the first hit can return a stale body — see FindBody in
// docs/proofs/GPU_GUST.tla (review-05).
GPU_GUST_DEVICE
inline uint64_t gpu_gust_vbox_read(const GUSTVBox *vb, uint64_t threshold) {
    uint64_t best = 0;
    for (int d = 0; d < GPU_GUST_VBOX_DEPTH; d++) {
        uint64_t v = *(volatile const uint64_t*)&vb->versions[d];
        if (v != 0 && v <= threshold && v > best) best = v;
    }
    return best;
}

// Snapshot read with payload: max version <= threshold and its value.
// A non-zero version is the publish marker (written after the value,
// release-ordered by __threadfence), so reading the value after the
// version is safe; slots are never reused (overflow-abort policy), so
// the pair cannot tear.  Returns 0 (version 0, value 0) if no such
// version exists.
GPU_GUST_DEVICE
inline uint64_t gpu_gust_vbox_read_value(const GUSTVBox *vb, uint64_t threshold,
                                         uint64_t *ver_out, uint32_t *val_out) {
    uint64_t best = 0;
    uint32_t best_slot = 0;
    for (int d = 0; d < GPU_GUST_VBOX_DEPTH; d++) {
        uint64_t v = *(volatile const uint64_t*)&vb->versions[d];
        if (v != 0 && v <= threshold && v > best) { best = v; best_slot = (uint32_t)d; }
    }
    *ver_out = best;
    *val_out = best ? vb->values[best_slot] : 0;
    return best;
}

// True if any slot holds a committed version newer than `threshold`
// (used by MRV).  Scans the whole window: per the spec's Valid() the
// abort condition is "ANY body newer than the snapshot", not "the
// first-ordered body is newer" (review-05 / GPU_GUST.tla counterexample).
GPU_GUST_DEVICE
inline int gpu_gust_vbox_has_newer(const GUSTVBox *vb, uint64_t threshold) {
    for (int s = 0; s < GPU_GUST_VBOX_DEPTH; s++) {
        uint64_t v = *(volatile const uint64_t*)&vb->versions[s];
        if (v != 0 && v > threshold) return 1;
    }
    return 0;
}

GPU_GUST_DEVICE
inline int gpu_gust_read_contains(uint32_t a, const uint32_t *ra, int n) {
    for (int i = 0; i < n; i++) if (ra[i] == a) return 1;
    return 0;
}

GPU_GUST_DEVICE
inline int gpu_gust_write_contains(uint32_t a, const uint32_t *wa, int n) {
    for (int i = 0; i < n; i++) if (wa[i] == a) return 1;
    return 0;
}

template <int MAX_READS, int MAX_WRITES>
__global__ void gpu_gust_kernel(
    GUSTVBox     *vboxes,         // [num_addrs] versioned boxes
    uint64_t     *gts,            // global timestamp (finalized CL slots)
    uint64_t     *write_ptr,      // commit log write pointer (AtomicINC target)
    GUSTCLEntry  *cl,             // [CL_SIZE] commit log
    int           num_addrs,
    uint64_t     *committed_count,
    uint64_t     *abort_count,
    uint64_t     *overflow_count,   // VBox-window overflows (may be null)
    int           reads_per_thread,
    int           writes_per_thread
) {
    const int lane = threadIdx.x;
    const int warp = blockIdx.x;

    extern __shared__ char s_buf[];
    uint32_t  *s_read_addr = (uint32_t*)s_buf;
    uint64_t  *s_read_ver  = (uint64_t*)(s_buf + 32 * MAX_READS * 4);
    uint32_t  *s_write_addr = (uint32_t*)(s_buf + 32 * MAX_READS * 12);
    uint32_t  *s_write_val  = (uint32_t*)(s_buf + 32 * MAX_READS * 12 + 32 * MAX_WRITES * 4);

    uint32_t *my_ra = &s_read_addr[lane * MAX_READS];
    uint64_t *my_rv = &s_read_ver[lane * MAX_READS];
    uint32_t *my_wa = &s_write_addr[lane * MAX_WRITES];
    uint32_t *my_wv = &s_write_val[lane * MAX_WRITES];

    __syncwarp();

    // ── Phase 1: snapshot + READ ──────────────────────────────
    // GTS counts FINALIZED slots (committed or aborted).  A writer
    // with CTS=c publishes version c+1; it is finalized (visible to
    // snapshots) iff c < startTS.  Hence the snapshot threshold is
    // startTS — NOT startTS+1, which would let an in-flight writer
    // (CTS == startTS, not yet finalized) slip into the snapshot.
    const uint64_t startTS = *gts;                 // versions stored as CTS+1
    const uint64_t snapshot = startTS;             // threshold for snapshot read
    int my_reads = 0;
    for (int r = 0; r < reads_per_thread; r++) {
        int idx = (lane * reads_per_thread + r) % num_addrs;
        my_ra[my_reads] = (uint32_t)idx;
        my_rv[my_reads] = gpu_gust_vbox_read(&vboxes[idx], snapshot);
        my_reads++;
    }
    __syncwarp();

    // ── Phase 2: WRITE buffer ────────────────────────────────
    int my_writes = 0;
    for (int w = 0; w < writes_per_thread; w++) {
        int idx = (lane * writes_per_thread + w + 1) % num_addrs;
        my_wa[my_writes] = (uint32_t)idx;
        my_wv[my_writes] = (uint32_t)(lane + warp * 32 + w);
        my_writes++;
    }
    __syncwarp();

    // ── Phase 3: PRE-VALIDATION (intra-warp conflicts) ────────
    // If a lower-numbered lane reads or writes an address this lane
    // writes, this (higher) lane aborts early so the warp proceeds
    // collectively without intra-warp contention.
    //
    // NOTE: all lanes iterate every write so that every lane calls
    // __ballot_sync in lockstep.  Short-circuiting on this lane's own
    // `conflict` flag would desynchronize ballot participation.
    int conflict = 0;
    for (int w = 0; w < my_writes; w++) {
        uint32_t a = my_wa[w];
        uint64_t rmask = __ballot_sync(~0ULL,
                          gpu_gust_read_contains(a, my_ra, my_reads));
        uint64_t wmask = __ballot_sync(~0ULL,
                          gpu_gust_write_contains(a, my_wa, my_writes));
        uint64_t lower = (1ull << lane) - 1;
        if ((rmask | wmask) & lower) conflict = 1;
    }
    __syncwarp();

    // ── Phase 4: CL INSERTION (AtomicINC, warp-cooperative) ───
    uint64_t base = 0;
    if (lane == 0) {
        base = atomicAdd((unsigned long long*)write_ptr,
                         (unsigned long long)GPU_GUST_WARP_SIZE);
    }
    // Broadcast the leader's result to every lane (outside the guard).
    base = __shfl_sync(~0ULL, base, 0);
    const uint64_t CTS = base + (uint64_t)lane;

    uint32_t cl_slot = (uint32_t)(CTS & GPU_GUST_CL_MASK);
    GUSTCLEntry *my_entry = &cl[cl_slot];

    int is_aborted = conflict;
    // Epoch-based ring-slot reclamation (review-05): a foreign entry at
    // this slot is exactly one revolution stale (cts == CTS - CL_SIZE).
    // It may be taken over only once GTS has passed it; under the launch
    // bound num_warps < CL_SIZE / WARP_SIZE (writePtr - gts < CL_SIZE) a
    // live foreign entry is impossible.  The old code overwrote whatever
    // was there (trampling live entries) and, worse, entries never
    // returned to FREE, so after one revolution every transaction
    // aborted.
    int owns_slot = 1;
    uint64_t old_cts = *(volatile uint64_t*)&my_entry->cts;
    if (old_cts != 0 && old_cts != CTS) {
        if ((uint64_t)*(volatile uint64_t*)gts <= old_cts) {
            // Defensive: CL too small for this grid.  Abort locally
            // WITHOUT touching the foreign entry.
            is_aborted = 1;
            owns_slot = 0;
        }
    }

    if (owns_slot) {
        // Publish payload FIRST, then cts, then state as the marker
        // (review-05: the old code wrote state=PENDING before
        // write_addrs/vals, so a concurrent CCT scan could observe
        // PENDING with an empty or stale write-set and miss the
        // conflict → lost update).
        my_entry->num_writes = (uint32_t)(is_aborted ? 0 : my_writes);
        if (!is_aborted) {
            for (int w = 0; w < my_writes; w++) {
                my_entry->write_addrs[w] = my_wa[w];
                my_entry->write_vals[w]  = my_wv[w];
            }
        }
        my_entry->cts = CTS;                 // epoch tag, payload group
        __threadfence();                     // release: payload before marker
        my_entry->state = is_aborted ? GPU_GUST_CL_ABORTED
                                     : GPU_GUST_CL_PENDING;
    }

    // ── Phase 5: VALIDATION (hybrid CCT + MRV) ────────────────
    if (!is_aborted) {
        int64_t valPtr = (int64_t)CTS - 1;
        const int64_t start = (int64_t)startTS;
        while (valPtr > start) {
            if ((uint64_t)valPtr < *(volatile uint64_t*)gts) {
                // Every slot below valPtr is finalized; the unconditional
                // MRV below covers them (spec: Valid = CCT ∧ MRV).
                break;
            }
            // CCT: valPtr ≥ GTS → that transaction may still be in
            // flight.  The slot reservation (AtomicINC) outpaces entry
            // publication, so spin until this epoch is published (the
            // owner publishes independently of GTS; bounded).
            GUSTCLEntry *e = &cl[(uint32_t)((uint64_t)valPtr & GPU_GUST_CL_MASK)];
            while (*(volatile uint64_t*)&e->cts != (uint64_t)valPtr &&
                   *(volatile uint64_t*)gts <= (uint64_t)valPtr) { }
            if (*(volatile uint64_t*)&e->cts != (uint64_t)valPtr) {
                valPtr--; continue;          // stale generation / silent abort
            }
            __threadfence();                 // acquire: payload after marker
            uint32_t st = *(volatile uint32_t*)&e->state;
            if (st != GPU_GUST_CL_ABORTED) {
                int nw = (int)e->num_writes;
                for (int i = 0; i < my_reads && !is_aborted; i++) {
                    for (int j = 0; j < nw; j++) {
                        if (e->write_addrs[j] == my_ra[i]) { is_aborted = 1; break; }
                    }
                }
            }
            valPtr--;
        }
        // MRV — unconditional (spec Valid = CCT ∧ MRV; review-05: the old
        // code skipped it whenever the CCT loop never crossed below GTS,
        // missing committed concurrent writers).  Abort if ANY read VBox
        // holds ANY version newer than the snapshot.
        if (!is_aborted) {
            for (int i = 0; i < my_reads; i++) {
                if (gpu_gust_vbox_has_newer(&vboxes[my_ra[i]], snapshot)) {
                    is_aborted = 1;
                    break;
                }
            }
        }
    }

    if (is_aborted) {
        if (owns_slot) {
            // Finalize after validation failed on a published entry.
            __threadfence();
            my_entry->state = GPU_GUST_CL_ABORTED;
        }
    } else {
        // ── Phase 6: WRITE-BACK ─────────────────────────────────
        // Reserve every slot first (AtomicINC on head); if any VBox
        // window is full the transaction overflows and aborts instead
        // of clobbering a live committed version (keeps every published
        // (version, value) pair immutable).  Then publish value FIRST
        // and version LAST — the version is the publish marker
        // (review-05: old order was head → version → value, so readers
        // could observe an advanced window with a half-written pair).
        uint32_t slot_of[GPU_GUST_MAX_WRITES];
        int overflow = 0;
        for (int w = 0; w < my_writes; w++) {
            GUSTVBox *vb = &vboxes[my_wa[w]];
            uint32_t s = (uint32_t)atomicAdd(&vb->head, 1u);
            slot_of[w] = s & (GPU_GUST_VBOX_DEPTH - 1);
            if (s >= GPU_GUST_VBOX_DEPTH) overflow = 1;
        }
        if (overflow) {
            is_aborted = 1;
            if (overflow_count)
                atomicAdd((unsigned long long*)overflow_count, 1ull);
            __threadfence();
            my_entry->state = GPU_GUST_CL_ABORTED;
        } else {
            for (int w = 0; w < my_writes; w++) {
                GUSTVBox *vb = &vboxes[my_wa[w]];
                vb->values[slot_of[w]] = my_wv[w];    // payload first
                __threadfence();
                vb->versions[slot_of[w]] = CTS + 1;   // version = marker
            }
            __threadfence();
            my_entry->state = GPU_GUST_CL_COMMITTED;
        }
    }
    __syncwarp();

    // Batch publication: leader waits until GTS reaches this warp's
    // base (all earlier batches finalized), then advances GTS by the
    // full batch, making every lane's version visible atomically.
    // NOTE: requires all warps co-resident (small grid); the spin
    // terminates because each prior batch finalizes exactly once.
    //
    // Every lane reaches this ballot (aborted lanes included) — a
    // lane that exited early would desynchronize __ballot_sync.
    uint64_t cmask = __ballot_sync(~0ULL, is_aborted ? 0u : 1u);
    if (lane == 0) {
        while (*(volatile uint64_t*)gts < base) { }
        atomicAdd((unsigned long long*)gts,
                  (unsigned long long)GPU_GUST_WARP_SIZE);
        atomicAdd((unsigned long long*)committed_count,
                  (unsigned long long)__popc((unsigned int)cmask));
        atomicAdd((unsigned long long*)abort_count,
                  (unsigned long long)(GPU_GUST_WARP_SIZE
                                       - __popc((unsigned int)cmask)));
    }
}
