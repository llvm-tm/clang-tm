# POWER8 HTM: gem5 Patches Needed

Concrete per-file patch spec for implementing the POWER8 transactional-memory
ISA in gem5 (SE mode, `MESI_Three_Level_HTM` Ruby). Companion to
`gem5_sim/docs/power8-htm-implementation.md` (architecture plan) and
`gem5_sim/docs/x86-tsx-patch.md` (the x86 analog). Policy/model theory lives in
`docs/proofs/Power8HTM.tla`.

Verified against the working tree at
`/Users/daniel/Projects/SIM/TM-SIM/gem5` (branch `stable`,
HEAD `6eb3dca1e7`, v25.1.0.1). Instruction encodings cross-checked against
`binutils/opcodes/ppc-opc.c` (sourceware master, this session).

## 0. Scope & decisions

| Decision | Choice | Rationale |
|---|---|---|
| Memory model reuse | `MESI_Three_Level_HTM` unchanged | Already implements POWER8-style ownership/NAK (see `f_sendDataToL1` NAK + `hftm_htmFailTransactionMem`); no TSX data-forwarding exists for x86 either — one protocol, both policies (matching `TLA+ ConflictPolicy` TSX/POWER8 in `Power8HTM.tla`). |
| Checkpoint location | `src/arch/power/htm.hh` + `htm.cc` | gem5 requires ISA-specific `HTMCheckpoint` per `src/arch/generic/htm.hh`. |
| Transaction-state recording | MSR[TS] bits (34,33) + 16/`tm` bit (32) | `Msr` `BitUnion64` already declares `ts`/`tm` in `src/arch/power/regs/misc.hh:108-127`. |
| Abort-status register | TEXASR/TFIAR as int regs (like `Msr`) | POWER has no miscregs (`NUM_MISCREGS = 0`, `readMiscReg` fatals). `Msr` is already `int_reg::Msr`, so adding `TEXASR`/`TFIAR` to the int reg file is the minimal, consistent route; mfspr/mtspr for them is a follow-up (see Patch 6). |
| Nesting depth gating | Reuse generic `getHtmTransactionalDepth()` | CPU-side generic (TimingSimple + O3), no POWER code needed. |
| Conditional aborts | `tabortwc/dc/wci/dci` decoded, routed to `tabort` path (code from RA/imm) | Cheap; unconditional `tabort.` is the one benchmarks use. |
| `treclaim`/`trechkpt` | Not implemented initially | Privileged/hypervisor; irrelevant for SE user-mode benchmarks. |

## 1. Patch 1 — `src/arch/power/htm.hh` (new) + `src/arch/power/htm.cc` (new)

POWER-specific `HTMCheckpoint` mirroring `src/arch/arm/htm.{hh,cc}` and
`src/arch/x86/htm.{hh,cc}`.

**Checkpointed architectural state** (Power ISA v2.07 Book I §8.5 rollback set,
as far as gem5 POWER models it):

- All 32 GPRs: `int_reg::NumArchRegs` via `intRegClass[n]`.
- All 32 FPRs: `float_reg::NumArchRegs` via `floatRegClass[n]`.
- Special int regs: `Cr`, `Xer`, `Lr`, `Ctr`, `Tar`, `Fpscr`, `Msr`
  (`int_reg` RegId names from `src/arch/power/regs/int.hh`).
- Reservation bits `Rsv`, `RsvLen`, `RsvAddr` — the reservation is *cleared*
  on abort; save/restore them or write 0 on restore (mirror x86 `htm.cc`).
- Fallback PC: on `tbegin.` abort, Power ISA redirects to `TFHAR` (set by
  `tbegin.` to the instruction address after `tbegin.`). Store `nPc` and, on
  restore, advance to `nPc` (x86 pattern: `pc.set(nPc)` in `htm.cc`).

**Class shape** (starter):

```cpp
class HTMCheckpoint : public BaseHTMCheckpoint
{
  public:
    static constexpr int MAX_HTM_DEPTH = 255;
    void reset() override;
    void save(ThreadContext *tc) override;
    void restore(ThreadContext *tc, HtmFailureFaultCause cause) override;
    void setFallbackAddress(Addr pc) { nPc = pc; }
  private:
    std::array<RegVal, int_reg::NumArchRegs> gpr;
    std::array<RegVal, float_reg::NumArchRegs> fpr;
    RegVal cr, xer, lr, ctr, tar, fpscr, msr;
    Addr nPc;
};
```

**`restore()` MUST ALSO** (this is where the TLA+ model pins behaviour):
- Set `MSR[ts]` to Non-transactional (clear bits 34,33) — rollback.
- Populate `TEXASR` + `TFIAR`: TEXASR failure cause from `cause`
  (mem→conflict, size→capacity, explicit→abort), TFIAR = failing address;
  set TEXASR[64:N]=1 etc. per `Power8HTM.tla` `FMAbort`-style recording.
  Keep it minimal for now (see Patch 5 — per-ISA abort status hook).
- Set CR0 per ISA rules (verified vs Linux kernel `tbegin; beq abort_handler`
  and LLVM's comment "for tbegin., the EQ bit ... successfully started (0) or
  failed (1)"): on abort set CR0[EQ]=1 so the software fallback `beq` branches
  to the handler; on success CR0[EQ]=0. Note: an *early* abort (transaction
  never started) also takes the fault path in this model → CR0[EQ]=1 is set by
  the same restore path.
- `tc->getIsaPtr()->globalClearExclusive()` (mirror ARM).

## 2. Patch 2 — `src/arch/power/insts/tm.hh` (new) + `tm.cc` (new)

Instruction classes modeled on `src/arch/arm/insts/tme64.{hh,cc}` and
`src/arch/x86/insts/htmruby.cc` (single-pass + Ruby split if needed; here
single class set is enough since POWER has no non-Ruby path in gem5).

Base class `TmOp : public PowerStaticInst` with generateDisassembly.

| Class | Mnemonic | Mem-cmd / flags | Behaviour |
|---|---|---|---|
| `TBegin` | `tbegin.` | `HTM_START`, `IsHtmStart`, `IsLoad`, `IsNonSpeculative` | `initiateAcc`→`xc->initiateMemMgmtCmd(STRICT_ORDER\|PHYSICAL\|HTM_START)` (depth>1: add `NO_ACCESS`). `completeAcc`: on depth==1 save checkpoint + `setFallbackAddress(PC+4)` + set MSR[ts]=`01` + **CR0[EQ]=0** (started); failure path (NEST/cache) → `GenericHtmFailureFault` → restore sets CR0[EQ]=1, TEXASR, PC→nPc. |
| `TEnd` | `tend.` | `HTM_COMMIT`, `IsLoad`, `IsNonSpeculative` | `initiateAcc`→`HTM_COMMIT` (depth>1: `NO_ACCESS`). `completeAcc`: depth==1 → `cpt->reset()`, clear MSR[ts], `globalClearExclusive()`. **CR0[EQ]=0** on commit success. |
| `TAbort` | `tabort.` | `HTM_CANCEL` then explicit fault | `initiateAcc`→`HTM_CANCEL`; `completeAcc`→`GenericHtmFailureFault(uid, EXPLICIT, ra_low8)` (code from `GPR[RA]&0xff`). CR0[EQ]=1 via restore. Conditional variants (tabortwc/dc/wci/dci) produce the same fault with a condition check. |
| `TCheck` | `tcheck` | none (`execute`) | Read MSR[ts]/TEXASR, write CR field `BF` (0 current / 1 transactional / 2 suspended). Memory-free. |
| `TSr` | `tsuspend`/`tresume` | none (`execute`) | Update `MSR[ts]` to `11` (suspended) / `01` (resumed); no memory semantics. Suspended-state store restrictions can be skipped in the model (documented). |

Flags required for O3/TimingSimple bookkeeping: `IsHtmStart`, `IsNonSpeculative`
for begin/cancel; `IsMicroop`/`IsLoad` to route through the memory path.

## 3. Patch 3 — decoder: `src/arch/power/isa/formats/tm.isa` (new), `main.isa`, `decoder.isa`, `includes.isa`, `bitfields.isa`

All POWER8 TM instructions are X-form, primary opcode 31, decoded in the
existing `31: decode X_XO {` block (`decoder.isa:325`). `X_XO <10:1>` is the
same field value binutils uses as `xop` (verified: existing entries at
`decoder.isa:151` isync=150, `:598` sync=598, `:774` eieio=854 all equal
binutils `X(31,N)` values).

**XO keys to add** (binutils `ppc-opc.c` authoritative):

| Mnemonic | binutils | XO key |
|---|---|---|
| `tbegin.` | `XRC(31,654,1)` | **654** (R bit = XO MSB; decode both R forms) |
| `tend.` / `tendall.` | `XRC(31,686,1)` (+`\|(1<<25)` for tendall) | **686** |
| `tcheck` | `X(31,718)` | **718** (BF operand = bits 25:23 → gem5 `BF <25:23>` already at `bitfields.isa:85`) |
| `tsuspend.` / `tresume.` | `XRCL(31,750,0/1,1)` | **750** (L bit 21 = gem5 bit 10) |
| `tabortwc.` | `XRC(31,782,1)` | **782** |
| `tabortdc.` | `XRC(31,814,1)` | **814** |
| `tabortwci.` | `XRC(31,846,1)` | **846** |
| `tabortdci.` | `XRC(31,878,1)` | **878** |
| `tabort.` | `XRC(31,910,1)` | **910** (abort code = GPR[RA] bits 0:6, persistent bit 7) |
| `treclaim.` (priv) | `XRC(31,942,1)` | **942** — skip |
| `trechkpt.` (priv) | `XRC(31,1006,1)` | **1006** — skip |

Templates:

```isa
// formats/tm.isa
def template TmExecute {{
    Fault %(class_name)s::execute(ExecContext *xc,
        trace::InstRecord *traceData) const
    {
        Fault fault = NoFault;
        %(op_decl)s;
        %(op_rd)s;
        %(code)s;
        if (fault == NoFault) { %(op_wb)s; }
        return fault;
    }
}};
def format TmOp(code, inst_flags = []) {{
    iop = InstObjParams(name, Name, 'TmOp', {"code": code}, inst_flags)
    header_output = BasicDeclare.subst(iop)
    decoder_output = BasicConstructor.subst(iop)
    decode_block = BasicDecode.subst(iop)
    exec_output = TmExecute.subst(iop)
}};
```

Wiring:
- `isa/main.isa`: add `##include "formats/tm.isa"` (after formats/formats.isa).
- `isa/includes.isa`: add `#include "arch/power/insts/tm.hh"` to the `output
  header` block (next to `misc.hh`).
- `isa/decoder.isa`, inside `31: decode X_XO {` (before the `default:`):
  ```
  654: TmOp::tbegin({{ ... }});
  686: TmOp::tend({{ ... }});
  718: TmOp::tcheck({{ ... }});
  750: TmOp::tsr({{ ... }});
  782: TmOp::tabortwc({{ ... }}); 814: TmOp::tabortdc({{ ... }});
  846: TmOp::tabortwci({{ ... }}); 878: TmOp::tabortdci({{ ... }});
  910: TmOp::tabort({{ ... }});
  ```
  (assign the non-trivial bodies to the C++ methods in Patch 2; pass via
  constructor flags, do the mem-cmd work in initiateAcc/completeAcc.)

## 4. Patch 4 — `src/arch/power/isa.cc`: allocate the checkpoint

In `ISA::startup()` (ARM precedent `src/arch/arm/isa.cc:155-166`, x86
`src/arch/x86/isa.cc:530-533`):

```cpp
void ISA::startup() {
    BaseISA::startup();
    if (tc) {
        std::unique_ptr<BaseHTMCheckpoint> cpt(new HTMCheckpoint());
        tc->setHtmCheckpointPtr(std::move(cpt));
    }
}
```

POWER8 always has TM (no extension-equality gate; the chicken/egg ordering is
fine since `startup()` runs after the TC is attached).

## 5. Patch 5 — `src/sim/faults.cc` + `src/sim/faults.hh`: make abort-status ISA-aware

Current `GenericHtmFailureFault::invoke` (`faults.cc:124-180`) hardcodes the
x86 RTM EAX status into `intRegClass[0]` after restore. POWER8 must instead
record TEXASR/TFIAR + CR0 and restore a *different* register set. This is a
local-dirty-block that is correct only for x86.

Refactor (keeps x86 behaviour, adds POWER):
- Introduce a virtual abort-status hook on `BaseHTMCheckpoint`:
  `virtual void setAbortStatus(ThreadContext *tc, HtmFailureFaultCause cause, uint8_t code) {}`
- `X86ISA::HTMCheckpoint::restore` → move the current EAX block into
  `setAbortStatus` (x86 semantics unchanged: bit0 explicit, bit1 retry,
  bit2 conflict, bit3 capacity, bit5 nested, code<<24).
- `PowerISA::HTMCheckpoint::restore` → `setAbortStatus` writes TEXASR
  (cause→Failure/Conflict/Capacity bits, abort code), TFIAR (failing
  address — set to faulting `nPc` for simplicity), then sets MSR[ts]=00 and
  CR0 failure; **do not touch int 0**.
- `GenericHtmFailureFault::invoke`: call `checkpoint->setAbortStatus(...)`
  after `restore()` and drop the hardcoded EAX block + its try/catch.

## 6. Patch 6 — `src/arch/power/regs/int.hh` + `misc.hh`: TEXASR/TFIAR registers (minimal)

To hold failure state per Patch 5:

- `int.hh`: extend `int_reg` enum after `_RsvAddrIdx` with
  `_TexasrIdx, _TfiarIdx` (bump `NumRegs`), add RegIds `Texasr`, `Tfiar`
  alongside `Msr`/`RsvAddr` (mirror the existing pattern at lines 82-147).
- `misc.hh`: no enum change needed (no POWER miscreg namespace); optionally add
  `BitUnion64(Texasr)` naming bits used by the model to keep TLA+ ↔ C++ field
  names aligned (`Failure`, `Abort`, `Conflict`, `Capacity`, `Nested`,
  `FailureCode`, `Persistent`).

Follow-up (not required to run bank/fuzz): `mtspr`/`mfspr` XFX decodes
(`XFX_XO <10:1>`, SPR `<20:11>`) for SPR 128 = TFHAR, 129 = TFIAR,
130 = TEXASR so libitm/glibc-style `mfspr` reads work (verified via
binutils `ppc-opc.c` XSPR row: `mt{it}fhar` 128, `mt{it}fiar` 129,
`mttexasr` 130). gem5 POWER currently has **no**
mtspr/mfspr at all — grep confirms none exist in the decoder or insts.
Seeding these regs is optional for SE: the checkpoint reads them
internally, which is enough for model verification.

## 7. Patch 7 — `src/arch/power/SConscript`: build the new sources

Add to the `Source(...)` list (next to `insts/misc.cc` line 49 region):
```python
Source('htm.cc', tags=['power isa'])
Source('insts/tm.cc', tags=['power isa'])
```

## 8. Patch 8 — `gem5_sim` configs + workload (invariant harness)

- New config `gem5_sim/configs/power-se-bank.py` mirroring
  `x86-se-bank.py` but with a `powerpc64le` ELF (kernel not needed; SE mode):
  `X86TimingSimpleCPU` equivalent → `PowerTimingSimpleCPU` (all-TimingSimple)
  + `MESI_Three_Level_HTM` Ruby (same hierarchy params as the x86/TSX run to
  keep `comparison-tsx-vs-gem5.md` apples-to-apples).
- Build a POWER8 bank/fuzz workload. The existing C++ banchmarks can't emit
  POWER8 asm on this host; use a tiny hand-written `{.word}` / `.insn`
  POWER8 assembly bank (tbegin./bne fallback/tend. pattern from the paper) OR
  `powerpc64le-linux-gnu-gcc -mhtm` via Docker (`ppc64le/alpine` similar to
  the existing x86 Docker recipe in `gem5_sim/README.md`). Invariant: money
  conserved = the same check the TLA+ `MoneyConservation` (TSXSGL-conserved)
  formalizes. Also run the existing `gem5_sim/tests` harness with the POWER
  binary substituted.

## 9. Ordering & verification

1. Patch 1-4 (checkpoint + instructions + decode + alloc): build `gem5.opt`
   `PowerISA` target, run a single-transaction smoke test.
2. Patch 5 (abort-status hook): verify a manual `tabort` restores CR0 and
   exits via TFHAR path; started-abort sets TEXASR bits.
3. Patch 6-7: TEXASR/TFIAR int regs; SCons build.
4. Patch 8: bank/fuzz on `MESI_Three_Level_HTM`; 1/2/4 threads; money
   conserved; abort counts non-zero under contention.
5. Cross-check vs x86 TSX run (`comparison-tsx-vs-gem5.md`) at identical
   clock — POWER8-style NAK should abort *more* than TSX last-writer-wins
   (matches the `Power8HTM.tla` TSX vs POWER8 policy divergence).
6. Re-run `docs/proofs/Power8HTM*` TLC checks after confirming semantic
   mapping (WriteImpliesOwn/ReadersNoNAK correspond to Ruby NAK behaviour).

## 10. Non-goals / documented gaps

- True L2TMCAM / L2 write-buffering: gem5 model uses L0/L1 Ruby roles (same
  protocol as ARM TME/x86 TSX). Document in README that POWER8 L2 buffering is
  approximated by the shared `MESI_Three_Level_HTM`.
- Suspended-state store-failure semantics, ROT read-untracking, treclaim/
  trechkpt, vector (VSX/VMX) checkpointing: explicitly not modelled.
- AtomicSimpleCPU `execute()` panic for tbegin/tend/tabort (mem-cmd only) —
  same limitation as ARM TME (`tme64.cc:139`).