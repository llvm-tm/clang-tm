"""
x86-64 SE-mode *checkpoint* config for TM benchmarks (checkpoint-and-continue).

Same board as x86-se-bank.py, but the benchmark binary must be the
checkpoint variant (built with `GEM5_CKPT=1`, so its ROI is bracketed by
m5_work_begin / m5_work_end).  Setting the System param
`work_begin_ckpt_count = 1` makes gem5 exit with reason "checkpoint" the
first time m5_work_begin fires (the point right after ROI_RESET_STATS where
the process is fully initialised and stats are freshly reset).  The
ExitEvent.CHECKPOINT handler then calls `save_checkpoint(dir)` and stops,
so a later restore-phase run can resume from that directory without
re-running process init, the TM region allocator, or the bank's setup.

Build the checkpoint binary (on any host, no TSX needed):
    cd tm_api_cpp/benchmarks/cpp
    make BACKEND=NOREC GEM5=1 GEM5_CKPT=1 bin/bank_gem5_ckpt
    # -> bin/bank_gem5_ckpt_norec  (static x86-64, m5_work_begin/end set)

Save phase:
    ./build/X86_TSX/gem5.opt -d m5out/ckpt-save \
        gem5_sim/configs/x86-se-bank-checkpoint.py \
        --binary <path>/bank_gem5_ckpt_norec --threads 1 --accounts 16 \
        --txns 50 --checkpoint-dir m5out/ckpt-save/cpt.bank \
        --cpu-type timing
    # -> m5out/ckpt-save/cpt.bank/cpt.<tick>/ contains the checkpoint

Restore phase:
    ./build/X86_TSX/gem5.opt -d m5out/ckpt-restore \
        gem5_sim/configs/x86-se-bank-restore.py \
        --binary <path>/bank_gem5_ckpt_norec --threads 1 --accounts 16 \
        --txns 50 --checkpoint <path>/cpt.bank --cpu-type timing

The restore run skips process init: it resumes at the instruction after
m5_work_begin and runs the transactional ROI to completion with freshly
reset statistics, so stats.txt reflects only the measured region.

KNOWN LIMITATIONS (as of 2026-09-01):
1.  Save phase (this file) works end-to-end on the classic (Ruby-free)
    hierarchy: x86-se-bank-checkpoint-classic.py writes a real checkpoint
    (board.physmem.store0.pmem + m5.cpt).  The Ruby/HTM path
    (MESI_Three_Level_HTM) PANICS during checkpoint serialization with
    "Invalid RubyRequestType" (L0Cache_Controller.cc:2106) — the Ruby
    caches cannot be serialized yet.  Use the *-classic.py variants if you
    need a working save/restore.
2.  Restore phase hangs in SE mode: after Simulator._instantiate + restore,
    the multithreaded pthread benchmark never completes (pre-existing gem5
    SE checkpoint caveat — process/FD state is not fully serialized).  The
    save side is verified; restore is not a stable path for this workload.
3.  This checkpoint/continue feature is separate from the TSX/TME/POWER8
    HTM comparison work.  The Ruby fitness of _Three_Level_HTM is what
    serializes real transactional state, so HTM checkpointing remains open.
"""

import argparse
import os
import sys
from pathlib import Path

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.exit_event import ExitEvent
from gem5.utils.requires import requires

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gem5.coherence_protocol import CoherenceProtocol

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL_HTM)

from components.mesi_three_level_htm_cache_hierarchy import (
    MESIThreeLevelHTMCacheHierarchy,
)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, help="static x86-64 benchmark ELF "
                   "(GEM5_CKPT=1 variant with m5_work_begin/end around the ROI)")
    p.add_argument("--threads", type=int, default=1, help="benchmark threads (= cores)")
    p.add_argument("--accounts", type=int, default=64)
    p.add_argument("--txns", type=int, default=2000,
                   help="total transaction quota (0 = time-based, then --duration)")
    p.add_argument("--duration", type=int, default=200, help="ms, used when --txns 0")
    p.add_argument("--read-all", type=int, default=20, help="read-all percentage")
    p.add_argument("--clk", default="1.8GHz",
                   help="core clock (1.8GHz matches the Broadwell-EP calibration)")
    p.add_argument("--cpu-type", default="timing",
                   choices=["timing", "atomic", "o3"],
                   help="timing = cycle-accurate; atomic = fast functional check")
    p.add_argument("--env", action="append", default=[],
                   help="guest env var, KEY=VALUE (repeatable)")
    p.add_argument("--checkpoint-dir", required=True,
                   help="directory to write the checkpoint into (created if needed)")
    return p.parse_args()


args = parse_args()

if args.cpu_type == "atomic":
    print("ERROR: HTM (XBEGIN/XEND) is not implemented for Atomic CPU (htm.cc:44). "
          "Use --cpu-type timing or o3.")
    raise SystemExit(2)

CPU_TYPE = {
    "timing": CPUTypes.TIMING,
    "atomic": CPUTypes.ATOMIC,
    "o3": CPUTypes.O3,
}[args.cpu_type]

cache_hierarchy = MESIThreeLevelHTMCacheHierarchy(
    l1i_size="32KiB",
    l1i_assoc=8,
    l1d_size="32KiB",
    l1d_assoc=8,
    l2_size="256KiB",
    l2_assoc=8,
    l3_size="2MiB",
    l3_assoc=16,
    num_l3_banks=1,
)

memory = SingleChannelDDR3_1600(size="2GiB")

processor = SimpleProcessor(
    cpu_type=CPU_TYPE,
    isa=ISA.X86,
    num_cores=args.threads + 1,
)

board = SimpleBoard(
    clk_freq=args.clk,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

bench_args = ["-a", str(args.accounts), "-t", str(args.threads),
              "-r", str(args.read_all)]
if args.txns > 0:
    bench_args += ["-n", str(args.txns)]
else:
    bench_args += ["-d", str(args.duration)]

import m5
from gem5.simulate.simulator import Simulator

board.set_se_binary_workload(
    BinaryResource(local_path=os.path.abspath(args.binary)),
    arguments=bench_args,
    env_list=args.env,
    # exit_on_work_items=False so work_begin falls through to the
    # work_begin_ckpt_count logic below (instead of exiting "workbegin"
    # and returning early).
    exit_on_work_items=False,
    stdout_file=Path(m5.options.outdir) / "simout.txt",
    stderr_file=Path(m5.options.outdir) / "simerr.txt",
)

# On the first m5_work_begin (ROI start, right after stats reset), gem5
# exits with reason "checkpoint", because System.work_begin_ckpt_count == 1
# (the stdlib board *is* the System, so the param lives on the board).
board.work_begin_ckpt_count = 1

ckpt_dir = Path(args.checkpoint_dir)


class SaveAndExit:
    """Exit-event handler: save the checkpoint at ROI start, then stop."""

    def __call__(self):
        print(f"=== CHECKPOINT: saving to {ckpt_dir} ===")
        simulator.save_checkpoint(ckpt_dir)
        print("=== CHECKPOINT: saved — stopping save phase ===")
        return True


simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.CHECKPOINT: SaveAndExit()},
)
simulator.run()
