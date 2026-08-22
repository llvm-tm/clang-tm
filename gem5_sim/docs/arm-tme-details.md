# ARM TME in gem5

ARM Transactional Memory Extension (TME) is a hardware transactional
memory feature in ARMv8.1-A and later architectures.

## gem5 Implementation

- Generic HTM framework: `src/arch/generic/htm.hh`
- ARM-specific checkpoint: `src/arch/arm/htm.hh`
- TME instructions: `src/arch/arm/insts/tme64*`
- Cache protocol: `MESI_Three_Level_HTM` (Ruby)
- HTM sequencer: `src/mem/ruby/system/HTMSequencer`

## How It Works

1. **TSTART** begins a transaction, saves a register checkpoint
2. Memory updates are buffered in L1 data cache (speculative state)
3. L2 cache holds pre-transactional values (inclusive, versioning)
4. On conflict: `HtmFailureFault` restores checkpoint + aborts transaction
5. **TCOMMIT** makes speculative writes visible atomically
6. **TCANCEL** discards speculative state

## Constraints

- Transaction working set must fit in L1 data cache
- Eviction from L1 during a transaction causes abort
- Only works with Ruby MESI_Three_Level_HTM protocol

## Building

```bash
cd ../gem5
scons build/ARM/gem5.opt -j$(nproc)
# or build ALL to include both x86 and ARM:
scons build/ALL/gem5.opt -j$(nproc)
```
