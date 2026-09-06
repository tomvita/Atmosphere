# Bug: GDB hardware watchpoints stop working ~1 second after being armed

Found 2026-09-05 while reverse-engineering Aggelos 2. **Symptom is confirmed and
reproducible; the cause is not yet known.**

## Symptom

Set a hardware watchpoint through the GDB stub (`Z2` write, or `Z4` access).
dmnt replies `OK`.

- If something writes that address **within about a second**, the watchpoint
  fires normally.
- If the address is next written **several seconds later**, the watchpoint
  **never fires at all**.

It fails silently. There is no error; the watchpoint simply stops trapping.

This is easy to miss, because the natural way to test a watchpoint is to point
it at something that changes constantly -- and that always works. It only shows
up when watching something that changes on a player action (taking damage,
starting a hover), which is the main reason you would set a watchpoint at all.

## Reproduction

Two heap addresses in the same process:

- `A` = written every frame.
- `B` = written only while the player performs some action.

```
Z2,<A>,4  ; vCont;c   -> fires within milliseconds, every time
Z2,<B>,4  ; vCont;c   -> never fires, across 90+ seconds and many player actions
```

Both addresses are 8-byte aligned with `size = 4`, so both encode identically
(`bas = 0x0F`). Tried with `Z2` alone, and with `Z2` and `Z4` both armed.

## Workaround

**Re-arm roughly once a second.** In a loop: set the watchpoint, `vCont;c`, wait
~1.2 s, and if no stop arrives, interrupt and set it again.

With this, watchpoints that had never fired in 90 seconds fired after **2
re-arms**, every time, on three different addresses.

## Ruled out (measured, not assumed)

- **Address encoding.** `SetDataBreakPoint` computes
  `bas = ((1 << size) - 1) << (address & 7)` and aligns down. Both the working
  and non-working addresses are 8-byte aligned with size 4, giving identical
  `bas = 0x0F` and identical `dbgbcr`.
- **Per-thread binding.** `WatchPoint::Set` calls
  `HardwareBreakPointManager::SetContextBreakPoint(m_ctx, debug_process)`, which
  binds the context to the *process*, not a thread.
- **Register exhaustion.** Four watchpoint registers (`D0`-`D3`); only one or
  two were in use.
- **The process not actually running.** Verified through a completely
  independent path (a sysmodule reading via `dmnt:cht`): while resumed, a
  per-frame value took 14-17 distinct values per second; while halted, exactly
  1. So `vCont;c` genuinely resumes.
- **`clearw()`.** Its call sites are all driven by gen2 IPC commands, not by a
  periodic loop.

## Not yet investigated (superseded -- see the investigation below)

Something clears the ARM debug registers between arming and the write.
Candidates worth checking, in the order they seem likely:

1. The gen2 polling thread (`gen2_loop`) touching the same four hardware
   watchpoint registers via `setw()` / `clearw()` while a GDB session also owns
   them -- the two share `HardwareWatchPointManager` with no arbitration.
2. Debug-event handling re-applying or resetting breakpoint state on thread
   create/exit, which a game does constantly.
3. Stale gen2 watch state left behind by a previous client (Breeze,
   bookmark.ovl) that the loop keeps re-asserting.

Tomvita's own reading is that this dates from the merge of old `dmnt` into
`dmnt.gen2`, and that the GDB breakpoint path was probably never exercised.

## Practical guidance while the root cause is unproven

- Re-arm watchpoints in a loop; do not trust a single `Z2`.
- For an address with **several** writers, prefer the gen2 watch instead. It is
  no-pause and captures many hits with offset/x30 filtering, whereas a `Z2`
  returns one hit at a time and will keep landing on the most frequent writer.
  In the case that prompted this note, a `Z2` repeatedly caught a per-frame
  reset while a gen2 watch revealed all four writers -- including the one that
  actually mattered -- in a single capture.

---

## Investigation, 2026-09-06 (found by code read; all three now fixed on device)

Read of `dmnt2_hardware_breakpoint.cpp`, `dmnt2_hardware_watchpoint.cpp`,
`dmnt2_breakpoint_manager_base.cpp`, `dmnt2_gdb_server_impl.cpp`,
`dmnt2_debug_process.cpp`, and the mesosphere side (`kern_k_debug.cpp`,
`kern_k_scheduler.cpp`, `kern_k_sleep_manager.cpp`).

### Established facts

- The kernel does **not** save/restore `DBGWCRn/DBGWVRn/DBGBCRn/DBGBVRn`
  across context switches, thread creation, `ContinueDebugEvent`, or core
  migration. `KDebug::SetHardwareBreakPoint` writes the system registers of
  whichever core the calling thread is on and nothing else
  (`kern_k_debug.cpp`, `MESOSPHERE_SET_HW_WATCH_POINT`). Only `KSleepManager`
  (full console sleep) loses them, and it does not save them.
  So the registers can only be cleared by *somebody calling
  `svc::SetHardwareBreakPoint` again*, or by the write never landing on the
  core in question in the first place.
- `MDSCR_EL1` is safe. `kern_k_scheduler.cpp:260` read-modify-writes it
  (the accessor's default ctor reads the live register), so `MDE` survives.
- `CONTEXTIDR_EL1` is the process id for every user process at all times
  (`KProcess::Switch` -> `ActivateProcess`), so context matching does not
  depend on anything the debugger does.
- Inside dmnt.gen2, the only callers of `svc::SetHardwareBreakPoint` are
  `HardwareBreakPointManager::{SetHardwareBreakPoint, SetContextBreakPoint,
  SetExecutionBreakPoint}`, `WatchPoint::{Set,Clear}`, and the one-shot probe
  `CountBreakPointRegisters()` (guarded by `g_last_bp_ctx_register == -1`, so
  it runs once per dmnt boot).

### Additionally ruled out

- **Candidate 2 (debug-event handling resetting breakpoint state on thread
  create/exit).** `DebugEvent_CreateThread` and `DebugEvent_ExitThread` in
  `ProcessDebugEvents` only call `m_debug_process.Continue()`. No breakpoint
  state is touched. `ClearStep()` at the top of the loop drives
  `m_step_breakpoints`, which is a reference bound to `m_software_breakpoints`
  (`dmnt2_debug_process.hpp:80`) - software breakpoints, not debug registers.
- **Candidate 1 as stated (the gen2 polling thread on its own).**
  `gen2_loop()` returns before the command switch whenever
  `m_watch_data.execute == false`, and nothing sets `execute` except an IPC
  client (`SetGen2WatchData`). An idle Breeze / bookmark.ovl does not drive
  `setw()` / `clearw()`. It *does* become live the moment a client executes a
  gen2 command while a GDB session is up - see Finding A.

### Finding A - gen2 silently claims and clears GDB watchpoint hits

`dmnt2_gdb_server_impl.cpp:1929`

    const u64 band = std::max<u64>(32, m_watch_data.size);
    const u64 our_lo = util::AlignDown(m_watch_data.address, band);
    const u64 our_hi = our_lo + band;
    const bool in_band = (m_watch_data.size > 0) && (address >= our_lo) && (address < our_hi);
    if (address == m_watch_data.address || in_band || m_watch_data.gen2loop_on == 2) {
        m_watch_data.intercepted = true;
        if (R_SUCCEEDED(m_debug_process.ClearWatchPoint(m_watch_data.address, m_watch_data.size))) {

`m_watch_data` is a file-scope global (`dmnt2_gdb_server_impl.cpp:26`), not a
`GdbServerImpl` member. It survives session teardown, so a gen2 watch armed
earlier by Breeze or bookmark.ovl is still sitting in `address` / `size` when a
GDB client later arms a `Z2`.

If the `Z2` address falls anywhere in the 32-byte band around that stale
`m_watch_data.address`, gen2 claims the hit: it **clears the watchpoint**, sets
a next-PC hardware breakpoint, `Continue()`s, and sets `intercepted = true` so
**no stop reply is sent to GDB**. From the client's side the watchpoint simply
never fires, and is now disarmed.

The exact-match guard in the `Z`/`z` handler (`dmnt2_gdb_server_impl.cpp:2614`,
`if (address == m_watch_data.address) m_watch_data.address = 0;`) only covers an
exact hit, not the 32-byte band.

The `gen2loop_on == 2` arm of the same condition claims *any* watchpoint hit.
`gen2loop_on` is only refreshed by `gen2_loop()`, which ticks every 500 ms, so
there is a sub-500 ms window right after a client connects during which a stale
`2` claims and clears a hit that belongs to GDB.

This explains "fires silently never" and "the address you were investigating in
Breeze is the one that fails, an unrelated address works". It does **not**
explain "fires after 2 re-arms" - each re-arm would be hijacked again.

### Finding B - breakpoint slot pools are 16 deep on hardware with 6/4/2

`HardwareBreakPointManager::BreakPointCountMax = 0x10` (16 slots, `I0`..`I15`)
and `HardwareWatchPointManager::BreakPointCountMax = 0x10` (`D0`..`D15`).
`CountBreakPointRegisters()` computes `g_last_bp_register`,
`g_last_wp_register` and `g_first_bp_ctx_register`, and then **nothing uses them
to bound allocation**; `BreakPointManagerBase::GetFreeBreakPoint()` walks all 16.

On the Switch's Cortex-A57: 6 breakpoints (`I0`-`I5`), 4 watchpoints
(`D0`-`D3`), 2 context-aware comparators - which ARM requires to be the
highest-numbered ones, so `I4` and `I5`. dmnt uses `I4` as the execution context
register and `I5` as the watchpoint context register
(`GetWatchPointContextRegister()` returns `g_first_bp_ctx_register + 1`).

So the 5th simultaneous hardware execution breakpoint is allocated to `I4`, and
the 6th to `I5`. Writing `I5` as a plain execution breakpoint destroys the
context comparator that every armed watchpoint is *linked* to (the kernel sets
`WT=1`, and dmnt puts `ctx` in `LBN`). Every watchpoint then stops firing,
silently, with no error - and comes back the moment anything calls
`WatchPoint::Set` again, because `Set` re-writes the context breakpoint before
the data breakpoint. That is exactly the reported symptom, re-arm workaround
included.

This is latent for a `Z2`-only repro (gen2 uses one or two execution
breakpoints), but it is a real bug, and it is the mechanism that best matches
"re-arming brings it back".

### Finding C (current best fit for the repro) - the per-core write may not land

`MultiCoreThread` (`dmnt2_hardware_breakpoint.cpp:41`) walks the four cores:

    R_ABORT_UNLESS(svc::SetThreadCoreMask(CurrentThread, core, (1 << core)));
    R_ABORT_UNLESS(svc::GetThreadCoreMask(&cur_core, &core_mask, CurrentThread));
    const Result result = svc::SetHardwareBreakPoint(...);

`cur_core` is read and then **discarded**. That `GetThreadCoreMask` call looks
like a migration check whose assertion was never written. If the worker has not
actually migrated when `SetHardwareBreakPoint` runs, the write lands on the
wrong core and one or more cores never get the watchpoint - silently, since the
SVC succeeds on whichever core it did run on.

This fits every observation: a per-frame address written by the main thread on
its usual core always traps; an address written only by a job thread that
happens to run on a core the walk missed never traps; a re-arm is a fresh
four-core walk, so a couple of re-arms cover the missing core; and there is no
error anywhere.

### Status — all three fixed, 2026-09-06, on device

Shipped and confirmed working on the console (gen2 capture still triggers
normally, which is the path the ownership test gates). The root cause is still
**not** proven: none of this was measured, it was all found by reading. Two of
the three could each account for the symptom on their own, so the bug may simply
not reproduce next time — that is an answer, just not a satisfying one.

**Finding C — `MultiCoreThread` now verifies the migration.**
`svc::GetCurrentProcessorNumber()` replaces the discarded `GetThreadCoreMask`
result (which reported the *ideal* core we had just set, so it could never have
worked as a check). Spins 8 yields then 100 us sleeps, 64 max, ~5.6 ms worst
case. A core it never reaches is recorded and the request now **reports
failure** instead of leaving a watchpoint armed on three cores out of four; it
still walks the remaining cores, since arming what we can beats stopping.

Consequence worth knowing: if a migration miss happens during a watchpoint
*clear*, `WatchPoint::Clear()` still calls `Reset()`, so the manager frees the
slot while the register may stay armed on the missed core. That leaves a stale
watchpoint with no owner — self-healing in raw mode (the not-ours branch clears
it and continues), a spurious stop reply with a GDB session. Visible rather than
silent, which is the trade deliberately chosen throughout this change.

**Finding B — slot pools bounded.** `GetBreakPoint()` stops at
`GetUsableBreakPointCount()` (the registers below the reserved context
comparators) and the watchpoint pool at `g_last_wp_register + 1`. On the A57
that is 4 execution breakpoints and 4 watchpoints, down from a nominal 6 and 16
that were never safe to hand out. `I4`/`I5` can no longer be allocated as plain
execution breakpoints and destroy the comparator every watchpoint is linked to.

**Finding A — ownership no longer read from the stale global.** The claim is now

    const bool gen2_owns = m_gen2_watch_active
        && (address == m_gen2_watch_address || in_band || !m_session.IsValid());

Gated on `m_gen2_watch_active`, so if the gen2 side of *this session* has not
armed a watch it cannot claim anything — a stale address left in `m_watch_data`
by an earlier Breeze capture is inert. The band is computed from the shadow
fields and the subsequent `ClearWatchPoint` clears `m_gen2_watch_address`, which
is what `setw()` actually programmed into the hardware (matching what `clearw()`
already did). Raw mode reads `!m_session.IsValid()` directly instead of
`m_watch_data.gen2loop_on == 2`, closing the sub-500 ms window after a client
connects during which the stale `2` claimed hits belonging to GDB.

The band stays fuzzy on purpose: ARM reports a fault address within the region
the *access* touched, so a wide `STP`/`ST4` can land outside the watched qword.
It is simply not consulted until we know gen2 owns a watch at all.

**The rule this codifies: on ambiguity, favour the GDB client.** Guessing gen2
wrongly clears the watchpoint and sends no stop reply — silent, and it costs
hours. Guessing GDB wrongly sends a spurious stop reply — visible and harmless.

Deliberately left alone: the hardware *instruction* breakpoint branch has the
same shape but tests exact equality rather than a band, so its exposure is far
narrower, and its `next_pc` match is load-bearing for the capture loop.

**Not yet done (Part 2 of Finding A).** Even with fresh state, gen2 and a `Z2`
armed within 32 bytes of each other still resolve by guesswork. The proper fix
is slot ownership: have `HardwareWatchPointManager::SetWatchPoint` return the
slot index it allocated, record it per owner, and resolve a hit by the range the
hardware actually watches (`[AlignDown(addr, 8), +8)` for `size <= 8`, which is
literally what `SetDataBreakPoint` programs; `[AlignDown(addr, size), +size)`
otherwise). Left until `monitor dbgregs` has said something, so the watchpoint
path is not changed twice while the original bug is still unexplained.

### `monitor dbgregs` — available now

64-entry ring of every debug-register write: tick, requested core vs. core
actually observed, migrate spins, reg, `dbgbcr`, value, result. Rows that missed
their core are prefixed `MISS`. The header prints the probed register extents,
the usable counts, and which `I` register is the watchpoint's context
comparator.

Use it the moment a `Z2` will not fire: arm it, `vCont;c`, wait, interrupt,
`monitor dbgregs`.

- `MISS` on any row -> the migration was landing writes on the wrong core.
- a row *after* your arm writing the same `D` register, or the `I` register the
  header names as the watch context -> something overwrote it.
- **no rows at all after the arm** -> nothing in dmnt touched the registers and
  the cause is below us.

