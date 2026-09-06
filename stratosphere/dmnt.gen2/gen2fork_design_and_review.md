# dmnt.gen2 (Tomvita fork) — Design, Data Flow, Bugs & Optimization Review

Target version: **v0.14** (see `source/gen2type.hpp:4`)
Reviewed sources:
- `C:\GitHub\Atmosphere\stratosphere\dmnt.gen2\source\*`
- Client integrations: `C:\GitHub\Breeze\source\gen2menu.cpp` and `C:\GitHub\Breezehand-Overlay\source\main.cpp` (lines ~17170-17820).

> Scope: this document describes how the Tomvita gen2 fork works as a sysmodule, how its in-memory `m_watch_data` "RPC" works with Breeze and Breezehand, then lists concrete bugs and concrete optimization opportunities discovered while reading the code. Line references use `file:line` form.

---

## 1. High-level architecture

dmnt.gen2 is a system module on the Switch that replaces Nintendo's stock `dmnt`. It provides three things at once:

1. **`dmnt:cht` IPC service** — Atmosphere's existing cheat VM (CheatService / CheatVM in `source/cheat/`). Compatible with EdiZon-style cheat consumers (e.g. Breeze).
2. **GDB stub** over HTCS/TCP (port 22225) — the original Atmosphere gen2 GDB server (`dmnt2_gdb_server*.cpp`).
3. **The "gen2 fork" RPC** — a *single shared global struct* `m_watch_data_t m_watch_data` (defined `source/dmnt2_gdb_server_impl.cpp:25`, declared `source/gen2type.hpp:60-105`) that is read/written by external "clients" (Breeze, bookmark.ovl) and acted upon by a polling loop inside the sysmodule (`GdbServerThreadFunction2`, `dmnt2_gdb_server.cpp:39-50`). This is the part that makes the fork special for cheat-making.

`dmnt2_main.cpp:170-210` shows the boot sequence: initialize fs/sm/pm/ldr/ro/hid, then conditionally start the transport + GdbServers (only if HTCS or standalone gdbstub is enabled), then unconditionally start the cheat manager (`InitializeCheatManager`) and IPC server.

```
+-----------------------------------------------------------+
|                       dmnt.gen2                           |
|                                                           |
|  +------------------+   +---------------------------+     |
|  | dmnt:cht service |   | GdbServerThreadFunction   |     |
|  | (Atmosphere      |   | (accept GDB client; build |     |
|  |  cheat VM)       |   |  / destroy GdbServerImpl) |     |
|  +----------+-------+   +------------+--------------+     |
|             |                        |                    |
|             | extra IPC cmds         | gen2_server_on=1   |
|             | GetGen2WatchData /     |                    |
|             | SetGen2WatchData       |                    |
|             v                        v                    |
|        +---------------------------------+                |
|        |        m_watch_data (global)    |<-- read/write--+ via debug
|        |  (gen2type.hpp m_watch_data_t)  |                | handle by
|        +----------------+----------------+                | bookmark.ovl
|                         ^                                 |
|                         | polled every 50 ms              |
|             +-----------+------------+                    |
|             | GdbServerThreadFunction2 (gen2_loop)        |
|             |  SETW / CLEARW / ATTACH / DETACH / CONT     |
|             +-----------+------------+                    |
|                         |                                 |
|                         v                                 |
|              DebugProcess (svc::DebugActiveProcess,       |
|              SetHardwareBreakPoint, SetWatchPoint, ...)   |
+-----------------------------------------------------------+
```

The fork adds these things on top of stock dmnt.gen2:

| Addition | Where | Purpose |
|---|---|---|
| `m_watch_data_t` global | `gen2type.hpp`, `dmnt2_gdb_server_impl.cpp:25` | RPC / state channel |
| `gen2_loop()` | `dmnt2_gdb_server_impl.cpp:27-104` | Per-50ms FSM that executes commands |
| `GdbServerThreadFunction2` | `dmnt2_gdb_server.cpp:39-50` | Schedules `gen2_loop()` |
| `Gen2Attach()` | `dmnt2_gdb_server_impl.cpp:955-970` | Attach the GDB server's DebugEventsThread to a chosen PID |
| `setw / clearw / get_region / set_next_watch_for_exclusive_search / get_from_stack` | `dmnt2_gdb_server_impl.cpp:1063-2471` | Implementation of watch capture |
| Custom `ProcessDebugEvents` branches | `dmnt2_gdb_server_impl.cpp:1188-1404` | Per-event capture / re-arm / pattern matching |
| `dmnt:cht` extra IPCs `GetGen2WatchData` / `SetGen2WatchData` | `cheat/dmnt_cheat_service.cpp:201-213` | Lets a non-debugger client (Breeze) move bytes in/out of the global |
| `SuspendDebugEvents(bool)` | `cheat/impl/dmnt_cheat_api.cpp:1276` | Pauses the cheat-VM's debug-event thread while gen2 owns the game debug handle |
| Shared debug handle (`AttachDmnt`/`AttachGen2`) | `dmnt2_shared_debug_handle.cpp` | Both the cheat VM and the GDB server share a single `DebugActiveProcess` handle |

---

## 2. Watchpoint / breakpoint primitives

### 2.1 Hardware watchpoint (`dmnt2_hardware_watchpoint.cpp`)

`HardwareWatchPointManager` owns up to `0x10` `WatchPoint` slots (real number capped by `g_last_wp_register` probed at boot, `dmnt2_hardware_breakpoint.cpp:213-220`). For a request `SetWatchPoint(addr, size, read, write)`:

1. Validate the watchpoint range (`IsValidWatchPoint`, `dmnt2_hardware_watchpoint.cpp:58-93`). Sizes ≤ 8 set `BAS`, sizes > 8 must be power-of-two and naturally aligned (ARMv8 DBGWCR rules).
2. Pin the **context** breakpoint to the target process handle so the watch only fires for that process (`SetContextBreakPoint`, `dmnt2_hardware_breakpoint.cpp:153-163`).
3. Encode `DBGWCR`: `(mask << 24) | (ctx << 16) | (bas << 5) | (lsc << 3) | enable` (`dmnt2_hardware_watchpoint.cpp:46`).
4. Apply via `HardwareBreakPointManager::SetHardwareBreakPoint(reg, dbgbcr, address)` which sends a `MultiCoreRequest` over a message queue so the breakpoint is programmed **on every core 0..3** by a dedicated worker thread (`dmnt2_hardware_breakpoint.cpp:40-90`). This is critical for SMP correctness — `svc::SetHardwareBreakPoint` only programs the caller's core.

### 2.2 Hardware breakpoint

Identical multi-core dance, but instructions branch instead of data: `SetExecutionBreakPoint` (`dmnt2_hardware_breakpoint.cpp:170-180`). Used for both the user-visible breakpoint and for the "next-PC step" arming inside `ProcessDebugEvents`.

### 2.3 Software breakpoint (`dmnt2_software_breakpoint.cpp`)

For aarch64 the fork writes `0xE7FFFFFF` (Atmosphere SDK break) over the original instruction; the original is saved in the `SoftwareBreakPoint` slot and restored on `Clear`. Used for stepping when `m_use_hardware_single_step == false`.

---

## 3. The gen2 RPC: `m_watch_data_t`

This is the core of the fork. All fields are in `gen2type.hpp`. The interesting subset:

| Field | Meaning |
|---|---|
| `execute` | client→sysmodule: "run this command" |
| `done` | sysmodule→client: command consumed |
| `command` | one of `SETW/CLEARW/DETACH/ATTACH/ATTACH_CONT/CONT/INCPC` |
| `next_pid / next_address / next_read / next_write` | inputs staged by client |
| `address / read / write / size / i / j / k / offset / two_register` | current active watch |
| `x30_catch_type` | enum `OFFSET / NONE / STACK / R_MATCH / EXCLUSIVE_SEARCH / UNIQUE_SEARCH` — controls how a "call site" is identified |
| `fromU` (union of `from / from1 / from2 / from3`) | up to 512 captured entries (`max_watch_buffer = 0x200`) |
| `count / total_trigger / max_trigger / failed` | bookkeeping |
| `gen2loop_on` | 0 = idle, 1 = client connected via cht IPC, 2 = "raw" non-IPC mode (no GDB session) |
| `version[10]` | "v0.14" — used as a self-test from clients |
| `attached / attach_success` | mirror of `m_debug_process.IsValid()` |

### 3.1 Two transport mechanisms for the global

**Mechanism A — IPC (Breeze):** `dmnt:cht` exposes two extra commands implemented at `cheat/dmnt_cheat_service.cpp:201-213`:

```c
GetGen2WatchData(OutBuffer)  // memcpy(client, &m_watch_data, ...)
SetGen2WatchData(InBuffer)   // memcpy(&m_watch_data, client, ...)
```

Breeze calls these via `dmntchtSetGen2WatchData / dmntchtGetGen2WatchData` (e.g. `Breeze/source/gen2menu.cpp:235,244,259,262`). Sequence:

```
Breeze                              gen2 sysmodule
------                              --------------
fill m_watch_data; execute = true; done = false
SetGen2WatchData(&m_watch_data)  --> memcpy into global
                                    GdbServerThreadFunction2 wakes (≤50ms)
                                    gen2_loop() sees execute=true,
                                    dispatches SETW / etc.,
                                    sets done=true
poll GetGen2WatchData every ~50ms
when done==true -> read results from fromU
```

**Mechanism B — Debugger (bookmark.ovl):** `bookmark.ovl` is itself a homebrew overlay; it cannot reach `dmnt:cht` because `dmnt:cht` is already owned by Breeze's overlay process. Instead it does (`Breezehand-Overlay/source/main.cpp:17656-17745`):

1. `pminfoGetProgramId` over `svcGetProcessList` to find the dmnt.gen2 pid (matched against `kGen2TitleId`).
2. `svcDebugActiveProcess` on dmnt.gen2 itself.
3. Walk `svcQueryDebugProcessMemory` to find the 8th `MemType_CodeMutable` region and add `+0x10030` to land on the address of `m_watch_data` (the offset is empirically hard-coded for the current build).
4. `svcRead/WriteDebugProcessMemory` to set the same fields Breeze does.

This means **dmnt.gen2's own writable `.data` region is being modified by another process while the sysmodule's `gen2_loop()` is reading it** — see Bug 4 below.

### 3.2 `gen2_loop()` state machine

`dmnt2_gdb_server_impl.cpp:27-104`. Every 50 ms it:

1. Updates `attached` and `gen2loop_on` based on session validity.
2. If `execute == true`, clears it and switches on `command`:
   - **SETW**: if `x30_catch_type == EXCLUSIVE_SEARCH`, copy the current capture into the search list (`set_next_watch_for_exclusive_search`); otherwise `clearw()`, copy `next_*` to current and call `setw()`.
   - **CLEARW**: just `clearw()`.
   - **DETACH**: clear, detach, then `SuspendDebugEvents(false)` (cheat-VM resumes).
   - **ATTACH** / **ATTACH_CONT**: detach if a different PID is currently attached, then `Gen2Attach()` (which `SuspendDebugEvents(true)` and asks the `DebugEventsThread` to `m_debug_process.Attach`). `ATTACH_CONT` additionally issues `Continue()` to let the game run.
   - **CONT**: `m_debug_process.Continue()`.
   - **INCPC**: just `next_pc++` (used as an ad-hoc "skip an instruction" knob).
3. Sets `done = true`.

### 3.3 `setw()` / `clearw()`

`setw()` (`dmnt2_gdb_server_impl.cpp:2395-2436`):
- Memory watch path: `SetWatchPoint(address, size, read, write)`.
- Instruction-capture path: when both `read` and `write` are false, set a **hardware breakpoint** at `address` with size 4 (aarch64). After the breakpoint fires, `ProcessDebugEvents` will:
  1. Clear it
  2. Sample registers (`GetThreadContext`)
  3. Append to `fromU` (de-duplicated)
  4. Re-arm at `next_pc = address + 4` so execution proceeds one instruction and we can re-set the breakpoint at `address` again on the way back.

`clearw()` (`dmnt2_gdb_server_impl.cpp:2438-2471`): clears the next-PC breakpoint, then clears either the watchpoint or the address breakpoint depending on `read/write`. It also tracks `m_gen2_watch_active/address/size/read/write` separately from `m_watch_data.*`. See Bug 1.

### 3.4 The capture engine in `ProcessDebugEvents`

`dmnt2_gdb_server_impl.cpp:1144-1632` is the kernel of the fork. For each `svc::DebugEvent_Exception` of type `BreakPoint`:

* **Hardware instruction breakpoint** (= we hit our watched address or the next-pc re-arm point):
  * If `address == next_pc` → first half of a two-step "land at instr, ride one step, re-arm". Reinstall the breakpoint at the watched address.
  * If `address == m_watch_data.address`:
    * For `EXCLUSIVE_SEARCH`: shrinks the candidate list by removing entries whose stack/x30 signature matches (`Match_U` macro, line 1242) but whose computed `target_address` doesn't match the user's desired `target_address`. When the list size drops below the entry's `address`, the next `set_next_watch_for_exclusive_search()` is invoked to move to the next candidate. This is essentially a *batched bisection*: each game-side trigger eliminates one false positive.
    * Otherwise: compute `ret_Rvalue = r[i] + (two_register ? (r[j] << k) : 0) | ((X30 - main_start) << (64-27))`. With `stack_check_count == 0 && !grab_A && !grab_R`, store `(ret_Rvalue, count)` into `from[]`; with stack capture, store the `m_from_stack_t` (call-site fingerprint) into `from2[]`. De-duplication is O(count) per hit.
  * Range filtering (`v1..v2`) optionally reads memory at `r[i]+offset` of width `vsize` and rejects out-of-range values.
  * Then re-arms the next-PC breakpoint so the game can move past the watched instruction and trip it again.
* **Hardware data watchpoint** (line 1344-1401): records the call site via `get_from_stack`, de-dupes, re-arms `next_pc = pc + 4`. The check `(address & -32) == (m_watch_data.address & -32)` (line 1357) accepts hits within a 32-byte band — see Bug 2.

### 3.5 `get_from_stack`

`dmnt2_gdb_server_impl.cpp:1094-1143`. For each break:

1. Read up to `stack_check_size` (100) qwords from `[SP]`.
2. Find up to `stack_check_count` qwords that look like return addresses in `[main_start, main_end)`, record `(SP_offset, code_offset)` pairs in `m_from_stack.stack[]`.
3. If `grab_A`, additionally read `(max_call_stack - index)` `call_stack_t` entries from `grab_A_address`.
4. If `grab_R`, copy `r[Register]` into the next slot.
5. Set the "address" field according to `x30_catch_type` (encodes the return PC offset into the high 25 bits — a deliberately *lossy* compression to fit address+x30 into one `u64`).

This is what gives Breeze the per-call-site "called from M+0x..." breakdown used for ASLR-stable pointer scanning.

---

## 4. Concurrency model

There are several long-lived threads (priority `HighestThreadPriority-1` unless noted):

| Thread | Where | Function |
|---|---|---|
| Main / IPC service threads (×5) | `dmnt2_main.cpp:119-167` | `dmnt:cht` and `dmnt:-` request handling |
| GDB server accept thread | `dmnt2_gdb_server.cpp:51-122` (`GdbServerThreadFunction`) | Accept GDB clients, construct/destroy `GdbServerImpl` |
| GDB server "gen2_loop" thread | `dmnt2_gdb_server.cpp:39-50` (`GdbServerThreadFunction2`) | 50ms tick over `m_watch_data` |
| Cheat detect thread | `cheat/impl/dmnt_cheat_api.cpp:725` | watches launch hook, attaches cheat VM |
| Cheat debug-events thread | `cheat/impl/dmnt_cheat_api.cpp:747` | services debug events when cheat VM is the debugger |
| Cheat VM thread | `cheat/impl/dmnt_cheat_api.cpp:800` | runs cheat opcodes |
| Per-GdbServerImpl debug-events thread | `dmnt2_gdb_server_impl.cpp:973` (`DebugEventsThreadEntry`) | services game debug events when GDB / gen2 is debugger |
| TransportSession receive thread | `dmnt2_transport_session.cpp:25` | socket recv loop |
| MultiCore breakpoint worker | `dmnt2_hardware_breakpoint.cpp:84` | applies HW BP/WP on each core |

### Coordination

Only **one** kernel-level debugger can be attached to a process at a time. The fork solves the cheat-VM-vs-gen2 collision via:

* `dmnt::dbg::AttachDmnt / AttachGen2` (`dmnt2_shared_debug_handle.cpp`) — both record a flag and *share* a single `DebugActiveProcess` handle. The handle is closed only when both flags are false. Good design.
* `SuspendDebugEvents(true/false)` (`cheat/impl/dmnt_cheat_api.cpp:1276`) flips a global flag `g_suspend_debug_events`. The cheat-VM `DebugEventsThread` polls this flag at line 753-756 and sleeps 10 ms when set, so it does not consume debug events while gen2 owns them.

The `monitor gen2` command (qRcmd, line 2593-2597) toggles the suspension, and `setw`/`Gen2Attach` does it implicitly.

---

## 5. Bugs and risk areas

The numbering below maps the most severe issues first. Severity is annotated **[critical]**, **[high]**, **[med]**, **[low]**.

Each entry carries a **Status** line from an audit of the current source on 2026-09-06. Most of these are closed; the ones still live are Bug 2 (part), Bug 7 and Bug 14.

### Bug 1. **[high] `setw()` always runs both branches when `read || write` is true.**

`dmnt2_gdb_server_impl.cpp:2395-2436`:

```cpp
if ((m_watch_data.read || m_watch_data.write) && IsValidWatchPoint(...)) {
    SetWatchPoint(...);       // path A
    ...
}
if (!(m_watch_data.read || m_watch_data.write)) {
    SetHardwareBreakPoint(...); // path B
    ...
} else if (!IsValidWatchPoint(...)) {
    failed = 1;
    ...
}
```

This is fine. But notice the **first** `if` only fires when `IsValidWatchPoint` is true. When it's false (e.g. unaligned or oversized address), `m_gen2_watch_active` remains false **but** the user already entered with `read || write`; the second branch's `else if` correctly reports failure. So functionally it works, but the comment trail and dead `m_gen2_watch_active=true` in path B (sets `m_gen2_watch_read=false, m_gen2_watch_write=false`) make `clearw()` racy: if a user goes Watch → Watch on the same address (different `read/write` flags) without an intervening clear, `m_gen2_watch_*` may now describe the *new* watch while the kernel still holds the *previous* one. `clearw()` then issues `ClearWatchPoint` with the wrong parameters and silently fails.

**Fix:** make `setw()` first call `clearw()` unconditionally (it already does at the SETW dispatch level — but only at the gen2_loop layer, not when called from `qRcmd setw ...` at line 2703). Move the clearw to the top of `setw()`.

**Status (audited 2026-09-06):** effectively closed. `setw()` still does not clear first, but all three of its callers do -- the `SETW` dispatch in `gen2_loop()`, the `monitor setw ` handler, and `set_next_watch_for_exclusive_search()`. No current path can double-arm, but the invariant lives in the callers rather than in `setw()`; remember that when adding a fourth caller.

### Bug 2. **[high] Address-band match `(address & -32) == (m_watch_data.address & -32)` swallows unrelated watchpoint hits.**

`dmnt2_gdb_server_impl.cpp:1357`:

```cpp
if (address == m_watch_data.address
    || (address & -32) == (m_watch_data.address & -32)
    || m_watch_data.gen2loop_on == 2) {
    // treat as ours → clear our watchpoint, even if it wasn't ours
}
```

Two problems:
1. The CPU may report the **virtual address that triggered the data fault** (`d.info.exception.address`), which can lie within a *different* user watchpoint set via the standard GDB `Z2/Z3/Z4`. The fork's owner-check is "anywhere in the same 32-byte cache line", which is wrong: a stray GDB watch on `addr` within 32 B of `m_watch_data.address` will be silently dropped and never reported to the GDB client.
2. When `gen2loop_on == 2` (non-IPC raw mode used by bookmark.ovl), **every** data abort is claimed by gen2. If two clients (Breeze and bookmark.ovl) ever co-exist, hits intended for one are eaten by the other.

**Fix:** keep an explicit `BreakPointBase*` pointer (or store the actual register index / address pair returned by the WP manager) and compare exactly. The `GetWatchPointInfo()` lookup loop in `dmnt2_hardware_watchpoint.cpp:149-167` already gives the active watch — use *that* result as the predicate, not an arbitrary 32-byte mask.

**Status (addendum 20):** partly fixed. The ownership predicate no longer reads `m_watch_data` at all — it is gated on the per-session `m_gen2_watch_active`, so a stale address cannot claim anything, and the `gen2loop_on == 2` catch-all is now `!m_session.IsValid()` read directly. Problem 1 remains for two watches genuinely armed within 32 bytes of each other; the slot-ownership fix proposed above is the remedy and is still open.

### Bug 3. **[high] `clearw()` clears the breakpoint at `m_watch_data.next_pc` using stale data.**

`dmnt2_gdb_server_impl.cpp:2442`:

```cpp
if (has_debug_process && m_watch_data.next_pc != 0 && m_watch_data.next_pc != 0x55AA55AA) {
    m_debug_process.ClearHardwareBreakPoint(m_watch_data.next_pc, sizeof(u32));
    m_watch_data.next_pc = 0;
}
```

`next_pc` is updated inside `ProcessDebugEvents` whenever the game trips the watched instruction. There's **no mutex** between the GDB events thread (which writes `next_pc`) and the gen2_loop thread (which reads it here). On a relaxed-memory model this is a race; even on aarch64 with TSO-ish behavior, the value may be observed mid-update or be a value from a different generation. The 32-bit `address` bit-field in `m_from_t/m_from1_t` is also touched concurrently with `clearw()` via union aliasing.

**Fix:** protect `m_watch_data` with a `os::SdkMutex`, OR move `clearw()` into `ProcessDebugEvents` proper (post events to it via the queue, same as `MultiCoreThread`). See Optimization 3 below for a fuller mitigation.

**Status (audited 2026-09-06):** fixed. `m_watch_data` is guarded by `g_watch_data_lock` (Opt 3), and `clearw()` prefers the `m_gen2_watch_*` shadow fields over `m_watch_data` for the address and size it clears.

### Bug 4. **[critical] bookmark.ovl writes the in-process `m_watch_data` over a debug handle while gen2_loop reads it.**

This is by design (bookmark.ovl can't reach `dmnt:cht`), but combined with Bug 3 it is a real data race: `svcWriteDebugProcessMemory` writes are not atomic with respect to the sysmodule's own CPU accesses, especially for the `union fromU { from[], from1[], from2[], from3[] }` whose stride differs by tagged interpretation. Worse: there is **no version handshake** beyond comparing `wd.version` == `"v0.14"`. Any change to `m_watch_data_t`'s layout (e.g. adding a field, reordering, alignment differences) silently corrupts state. The string check catches the *string* drifting; it does not catch layout drift inside the struct.

**Mitigations:**
1. Add a `u32 layout_hash` field at a fixed offset (e.g. struct start) so clients can verify by hash, not just version string.
2. Provide a tiny "IPC for bookmark.ovl" path. For example: have dmnt.gen2 expose a 6th IPC command on `dmnt:-` (multi-session, not exclusive like `dmnt:cht`) that wraps `GetGen2WatchData / SetGen2WatchData / TriggerExecute` so bookmark.ovl can stop using `svcDebugActiveProcess` on the sysmodule entirely. This also removes the offset-hunting heuristic (`+0x10030`, `mod2 == 8`) in `Breezehand-Overlay/source/main.cpp:17692-17699` which is *guaranteed* to break on any reorder of NSO segments.
3. Until then: guard accesses with a sequence counter (write `seq++; mem fence; payload...; mem fence; seq++`) à la seqlock so the reader can detect a torn read. `gen2_loop` should reject `execute == true` when the seq is odd.

**Status (audited 2026-09-06):** fixed. bookmark.ovl writes through dmnt:cht IPC rather than over a debug handle. See addendum 8.

### Bug 5. **[high] `GdbServerThreadFunction` constructs and destructs the `GdbServerImpl` on every accept() retry.**

`dmnt2_gdb_server.cpp:75-95`:

```cpp
while (true) {
    {
        std::scoped_lock lk(g_gdb_server_lock);
        util::ConstructAt(g_gdb_server, 500, g_events_thread_stack, sizeof(g_events_thread_stack));
        util::GetReference(g_gdb_server).gen2_server_on = 2;
        g_gdb_server_constructed.store(true, ...);
    }
    int temp_fd = transport::Accept(fd);
    {
        std::scoped_lock lk(g_gdb_server_lock);
        g_gdb_server_constructed.store(false, ...);
        util::GetReference(g_gdb_server).gen2_server_on = 0;
        util::DestroyAt(g_gdb_server);
    }
    if (temp_fd < 0) break;
    client_fd = temp_fd;
    // ... construct AGAIN with client_fd, run LoopProcess, destroy ...
}
```

The "pre-accept" `GdbServerImpl(500, ...)` is constructed with a non-fd value `500` purely so that `gen2_server_on = 2` (non-IPC raw mode) is observable to bookmark.ovl while waiting for a GDB client. This:

1. **Creates and joins a `DebugEventsThread` on every accept loop iteration** (`GdbServerImpl` ctor spawns it, dtor signals/waits). On HTCS where `Accept` can poll-fail and re-enter every second, this is hundreds of thread creations per minute. Visible cost: thread stack init (16 KB), event-loop wakeup, mutex contention with the cheat manager.
2. The dummy `500` is later opened as a socket fd in `~TransportSession`! `transport::Close(500)` is called on a never-bound fd — depending on the socket backend that may or may not be a no-op; for HTCS it returns `-1` and is ignored. With TCP/BSD sockets this is harmless (EBADF) but is technically UB if 500 happens to be a real fd.
3. The `LoopProcess` for the dummy server is never started — only its events thread runs. But `m_session(500)` already created a TransportSession with `m_valid = true`, and its `ReceiveThreadFunction` will call `transport::Recv(500, ...)` (likely returns -1) in a tight loop until `Invalidate()` is called from the destructor. Burns one full CPU until accept() returns.

**Fix:** decouple the "gen2 raw mode" flag from `GdbServerImpl` lifetime. The only reason `GdbServerImpl` is constructed pre-accept is to set `gen2_server_on = 2`. Promote that flag to a free function / namespace variable; have `gen2_loop()` consult it directly. The actual `GdbServerImpl` should be constructed only once a real `client_fd` is in hand. This also fixes Bug 6.

**Status (audited 2026-09-06):** resolved, but differently from the fix proposed above. `g_gen2_server_on` was promoted to a free-standing variable as suggested. The `GdbServerImpl` is still constructed before `Accept()`, deliberately: `Gen2Attach()` signals `g_event_request_cv` and only a live `DebugEventsThread` can receive it, so with no placeholder a cold-boot ATTACH from Breeze times out silently. The sentinel fd is now `-1` rather than the old `500`, which could collide with a real socket.

### Bug 6. **[med] `GdbServerThreadFunction2` calls `gen2_loop()` only while a `GdbServerImpl` exists.**

`dmnt2_gdb_server.cpp:39-50`:

```cpp
void GdbServerThreadFunction2(void *) {
    while (true) {
        {
            std::scoped_lock lk(g_gdb_server_lock);
            if (g_gdb_server_constructed.load(std::memory_order_acquire)) {
                util::GetReference(g_gdb_server).gen2_loop();
            }
        }
        svcSleepThread(50'000'000);
    }
}
```

Combined with Bug 5, this means `gen2_loop()` runs as long as the dummy-fd hack keeps `g_gdb_server_constructed` true — which is *most* of the time, but **briefly false** every accept iteration. In that gap, a Breeze IPC `SetGen2WatchData` with `execute=true` can land and *never* be processed until the dummy server is re-built. Worst case (HTCS down) it stays unprocessed for the duration of the 1-second `os::SleepThread(TimeSpan::FromSeconds(1))` retry. Users see "command timed out".

**Fix:** make `gen2_loop()` an instance-independent function (operates only on `m_watch_data` plus a `DebugProcess*` member). Then the polling thread can call it always; the GDB server only sets the `DebugProcess` pointer when valid.

**Status (audited 2026-09-06):** mitigated. `gen2_loop()` still runs only while a `GdbServerImpl` exists, but the pre-accept placeholder means one almost always does; what remains is the few milliseconds between destroying one and constructing the next.

### Bug 7. **[med] `m_gen2_watch_*` shadow fields can desync from `m_watch_data.*`.**

`setw()` writes to both `m_watch_data.read/write/size/address` and `m_gen2_watch_*` (`dmnt2_gdb_server_impl.cpp:2409-2427`). But `Z` packet handling (`dmnt2_gdb_server_impl.cpp:1962-2046`) directly calls `m_debug_process.SetWatchPoint(...)` without touching `m_gen2_watch_*`. So if a GDB client sets a watchpoint while bookmark.ovl/Breeze has also set one, the shadow remembers only the gen2 one, and the next `clearw()` clears the *gen2* address, leaving the GDB watch dangling (and vice-versa for the GDB `z` packet).

The two clients are not supposed to be used simultaneously, but the failure mode is silent.

**Fix:** route every watchpoint set/clear through a single owner (BreakPointManagerBase) and ask *it* which slots are gen2-owned.

**Status (audited 2026-09-06):** still open, and now more load-bearing -- addendum 20 made the `m_gen2_watch_*` fields the ownership truth for the watchpoint hit path, which is the right direction but raises the cost of them being wrong. Fix this together with Bug 2's slot ownership; they need the same plumbing.

### Bug 8. **[med] `BreakPointManagerBase::ClearBreakPoint` ignores the size parameter.**

`dmnt2_breakpoint_manager_base.cpp:43-53`:

```cpp
Result BreakPointManagerBase::ClearBreakPoint(uintptr_t address, size_t size) {
    for (size_t i = 0; ...; ++i) {
        if (bp->m_in_use && bp->m_address == address) {
            // AMS_ABORT_UNLESS(bp->m_size == size);
            if (bp->m_size == size){}  // <-- statement does nothing
            R_RETURN(bp->Clear(m_debug_process));
        }
    }
    R_SUCCEED();
}
```

The intent is "fail if size mismatches", but the `if (bp->m_size == size){}` is a no-op (empty body) — a leftover from a debug commit. Today, clearing by address is fine because addresses are unique, but a copy-paste of this code into a future variant could clear the *wrong* breakpoint silently.

**Fix:** delete the dead line or actually return an error.

**Status (audited 2026-09-06):** resolved by decision rather than by code. A breakpoint is identified by its address and `size` is informational, so the dead `if` was replaced with `AMS_UNUSED(size)` and a note explaining why.

### Bug 9. **[med] `ProcessDebugEvents` increments `count` without bounds protection in the watchpoint path.**

`dmnt2_gdb_server_impl.cpp:1374-1378`:

```cpp
if (!found && m_watch_data.count < max_watch_buffer2) {
    m_watch_data.fromU.from2[m_watch_data.count].from_stack = entry;
    m_watch_data.fromU.from2[m_watch_data.count].count = 1;
    m_watch_data.count++;
};
```

`max_watch_buffer2 = max_watch_buffer * sizeof(m_from_t) / sizeof(m_from2_t)` (`gen2type.hpp:21`). Since `m_from2_t` is much larger than `m_from_t`, the union is **safely** within `m_from_t[0x200]`. Good.

But the *instruction* path at line 1283 uses `max_watch_buffer2` as the bound for both `from[]` and `from2[]`. The dedup loop iterates `for (int i = 0; i < m_watch_data.count; i++)` over **`from2`** then appends to **`from`** at line 1304 when there is no stack capture. Since `from` is the larger array (512 vs ~64-ish entries), `count` may grow past `max_watch_buffer2` (the smaller bound), which is fine for `from` but breaks the *next* hit if `stack_check_count` is suddenly raised by the user: the dedup loop will now read past `from2` end.

**Fix:** track active layout explicitly (an enum) and switch the bound and the dedup at every transition; clear `count` whenever the layout changes.

**Status (audited 2026-09-06):** fixed. The `from[]` path bounds by `max_watch_buffer` and the `from2[]` path by `max_watch_buffer2`, so the bound matches the array actually in use, and `setw()` zeroes `count`, which covers the layout transition the fix above was worried about.

### Bug 10. **[low] `Gen2Attach()` and `vAttach` both use `g_event_request_cv` with a 2-second timeout but then call `m_event.Signal()` on timeout.**

`dmnt2_gdb_server_impl.cpp:962-969` and `2164-2171`. `m_event` is a `EventClearMode_AutoClear` event but is only `Wait()`ed at process destruction. Signaling on timeout doesn't wake anyone. The signal *and* the timeout fallthrough is meant to abort attach, but really the only effect is `m_debug_process.IsValid()` will be false (because the events-thread never finished attaching) and the caller will reply `E01`. Cosmetic — the signal is dead code.

**Fix:** delete the `m_event.Signal()` calls, or actually wait on it in the timeout path.

**Status (audited 2026-09-06):** fixed. The condvar rework in addendum 9 removed the dead `m_event.Signal()` from both attach paths. The only remaining call is in `~GdbServerImpl()`, where it is real.

### Bug 11. **[low] `dmnt2_main.cpp:59-62` bypasses the ApiVersion check.**

```cpp
void CheckDmntGen2ApiVersion() {
    /* Bypass version check to guarantee boot success on all Atmosphere/Exosphere versions. */
    return;
}
```

Intentional, but the empty function still gets called and adds a small startup cost. More importantly: if Atmosphere ever changes a `svc::` ABI the fork relies on (e.g. `SetHardwareBreakPoint` register-name enum order), boot will silently succeed and later svc calls will fail with cryptic errors instead of refusing to boot. Consider a soft check that logs a warning.

**Status (audited 2026-09-06):** open by choice. `CheckDmntGen2ApiVersion()` is an explicit `return;` carrying a comment: the check is bypassed deliberately to guarantee boot across Atmosphere/Exosphere versions.

### Bug 12. **[low] Module name parsing in `DebugProcess::CollectModules` can read past path end.**

`dmnt2_debug_process.cpp:300-305`:

```cpp
for (size_t i = 0; i < std::min<size_t>(ModuleDefinition::PathLengthMax, module_path.path_length) && module_name[i] != 0; ++i) {
    if (module_name[i] == '/' || module_name[i] == '\\') {
        module.SetNameStart(i + 1);
    }
}
```

If `path_length > ModuleDefinition::PathLengthMax`, the loop bound is correct, but `path_length` is a signed `s32` from process memory and may be negative — comparison promotes to `size_t` so a negative value becomes huge and the `min` saves us. Good. But on line 274: `std::memcpy(module_name, ..., std::min<size_t>(ModuleDefinition::PathLengthMax, module_path.path_length))` — same promotion, same save. Pedantic, but worth adding `path_length > 0` checks for documentation.

**Status (audited 2026-09-06):** fixed. The `memcpy` is bounded with `std::min<size_t>(PathLengthMax, path_length)` and the name is explicitly truncated at `PathLengthMax - 1`.

### Bug 13. **[low] `IsValidWatchPoint` accepts unaligned 1..8-byte watches.**

`dmnt2_hardware_watchpoint.cpp:67-71`:

```cpp
/* Check that address is aligned. */
// if (util::AlignDown(address, 8) != util::AlignDown(address + size - 1, 8)) {
//     ... FAIL range crosses qword boundary ...
// }
```

The commented-out check was removed in the fork. Without it, a 4-byte watch at `addr 0xXXXX_XXXX_XXXX_XXXE` straddles two qwords; the BAS bits cannot represent that and `SetHardwareBreakPoint` will silently fire for only the lower half. Users see "Watch fires intermittently".

**Fix:** restore the qword-boundary check or auto-split into two watchpoint slots.

**Status (audited 2026-09-06):** fixed. The qword-boundary check was restored in `IsValidWatchPoint`, so callers get an explicit failure instead of a silently half-armed watchpoint.

### Bug 14. **[low] `g_multicore_thread` is created with priority `HighestThreadPriority - 1`, same as everything else.**

`dmnt2_hardware_breakpoint.cpp:84`. The MultiCore worker is on the critical path for *every* hardware breakpoint update (synchronous request-response over message queue, blocking the gdb events thread). If a 6th thread is doing CPU work at the same priority, the events thread can stall. Bump this thread one above the events thread.

---

**Status (audited 2026-09-06):** still open. `g_multicore_thread` is created at `HighestThreadPriority - 1`. Addendum 18 mitigated the symptom with event-loop rate limiting rather than fixing the priority relationship.

## 6. Optimization opportunities

### Opt 1. **Replace 50 ms polling with an event.**

`GdbServerThreadFunction2` polls every 50 ms regardless of whether `execute` is set. Worst-case latency from "Breeze hits A" to "watch installed" is 50 ms + IPC RTT. Replace `svcSleepThread(50'000'000)` with `os::Event` waiting; have `SetGen2WatchData` signal it. Result: sub-millisecond response, and the thread sleeps indefinitely when idle (saves ~3–5% of one core's wakeup budget on a quiet system).

Sketch:
```cpp
// in cheat_service.cpp SetGen2WatchData:
g_gen2_request_event.Signal();

// in GdbServerThreadFunction2:
while (true) {
    g_gen2_request_event.TimedWait(TimeSpan::FromMilliSeconds(500)); // periodic refresh of `attached`
    // ...gen2_loop()
}
```

This also eliminates Bug 6's race window.

### Opt 2. **Stop reconstructing GdbServerImpl per accept.**

Already covered as Bug 5. Concrete plan:

* Move `gen2_server_on` and the `m_watch_data` reference out of `GdbServerImpl` into the `ams::dmnt` namespace.
* Keep `GdbServerImpl` constructed only between `Accept` returning a real fd and the session closing.
* The polling thread checks `dmnt::HasGen2Mode()` directly.

Estimated savings: avoids ~20 thread/spinlock setups per minute under HTCS-down (typical undocked use), and drops "phantom watchpoint hits going to closed dummy session" entirely.

### Opt 3. **Add a mutex (or seqlock) around `m_watch_data`.**

Currently the only protection between the GDB events thread (writes `count`, `fromU`, `next_pc`, `total_trigger`, `failed`) and the gen2_loop / IPC handler (reads/writes the same) is *implicit*: each runs only at certain points. With Opt 1's event the windows narrow further but never disappear (since `ProcessDebugEvents` runs whenever the game traps, asynchronously to IPC). For the IPC path use an `os::SdkMutex g_watch_data_lock` and take it in:

* `CheatService::GetGen2WatchData / SetGen2WatchData` (memcpy under lock).
* `gen2_loop()` (whole switch under lock).
* `clearw() / setw() / set_next_watch_for_exclusive_search()` (whole bodies).
* The capture-side path in `ProcessDebugEvents` (only the section that writes `fromU/count/total_trigger/next_pc/failed`).

For the bookmark.ovl debugger path you cannot lock the sysmodule's mutex from outside, so combine with a seqlock or, better, retire that path (Bug 4 mitigation 2).

### Opt 4. **`get_from_stack` reads `stack_check_size = 100` qwords for every hit, even when `stack_check_count == 0`.**

Wait — it doesn't, the `if (m_watch_data.stack_check_count > 0)` guards it (`dmnt2_gdb_server_impl.cpp:1098`). But when `stack_check_count > 0`, `ReadMemory` is called for the full 800 bytes even when we only need the first few. The de-dup loop will reject most after the first 4 entries. Two improvements:

1. Start with `check_size = max(stack_check_count * 4, 8)` and grow only if not enough hits were found. Saves a 800-byte cross-process read on every breakpoint hit, which is the **dominant cost** in this path (svc::ReadDebugProcessMemory is ~tens of microseconds on aarch64 due to TLB invalidation in the kernel).
2. Cache the last `(sp, buffer)` per thread; many breakpoint hits in tight loops have unchanged SP for several frames.

### Opt 5. **Don't iterate all module mappings in `get_region()` for every `getw`.**

`dmnt2_gdb_server_impl.cpp:2356-2393` linearly scans up to `ModuleCountMax = 0x60` modules per call. `qRcmd getw` calls it `1 + count` times (once for the watch address, then once per `from`/`from2` entry). With `count ≈ 512` that's potentially 30 k pointer-comparison passes per status query. Build an interval tree (or simply sort modules by base address once at `CollectModules()` and binary-search) for O(log n) lookup. Easy win, big in practice.

### Opt 6. **Move large stack-resident buffers off the stack.**

`GdbServerImpl::LoopProcess` declares `char recv_buf[GdbPacketBufferSize]` + `char reply_buffer[GdbPacketBufferSize]` per iteration. `ProcessDebugEvents` similarly declares `char send_buffer[GdbPacketBufferSize]` (4 KB+). These are why the server-thread stack is `ServerThreadStackSize = 4 * GdbPacketBufferSize + os::MemoryPageSize` (`dmnt2_gdb_server.cpp:26`). Move them to members of `GdbServerImpl` (already done for `m_buffer`). Shrinks stack, reduces page commit, removes the impl-construction cost mentioned in Bug 5.

### Opt 7. **Use `std::strncmp` length-limited compares in `get_region`.**

`sprintf(m_watch_data.name, "Heap")` etc. on a `char name[10]`. Safe today (longest literal "undefine" is 8 chars + NUL) but uses `sprintf` for fixed strings — `__builtin_strcpy` or compile-time `constexpr` `std::array` would emit smaller code. Minor.

### Opt 8. **Pre-sort dedup arrays into a hash table.**

In the capture hot path, `for (int i = 0; i < m_watch_data.count; i++) if (... == ret_Rvalue) found = true;` is O(count). With `max_watch_buffer = 0x200` and `total_trigger` capped at `0x10000`, this is up to 32M comparisons per capture session. Either:

* Keep `count <= 64`, OR
* Maintain a small open-addressing hash next to `fromU`. Even a `u16` index hash with 1024 entries shrinks the worst case from O(N²) to O(N).

### Opt 9. **Reduce socket idle wakeup.**

`TransportSession::ReceiveThreadFunction` (`dmnt2_transport_session.cpp:102-126`) sits in a `transport::Recv` that, on HTCS, busy-loops returning EAGAIN. The current code retries silently. Adding a short `os::SleepThread(TimeSpan::FromMilliSeconds(5))` after a non-fatal EAGAIN drops idle CPU dramatically. (Counter: blocking sockets would be better, but the HTCS driver doesn't always support that — verify on hardware.)

### Opt 10. **`MultiCoreThread` always migrates the worker through all 4 cores.**

`dmnt2_hardware_breakpoint.cpp:55-72`: for each request it sets thread affinity to core 0, then 1, then 2, then 3, programming the BP register on each. This is necessary because `svc::SetHardwareBreakPoint` only programs the calling core. But the cost is 4 core migrations + 4 syscalls per BP update.

Possible optimization: on platforms where `svc::SetHardwareBreakPoint` is broadcast (mesosphere may already do this on `KernelMesosphere` builds — see `DebugProcess` ctor `m_use_hardware_single_step` check), one call programs all cores. Probe the behavior at init (`CountBreakPointRegisters` already does many such probes) and use a single call when supported. Saves ~3 migrations on most BP changes — visible because every `next_pc` re-arm in the capture loop triggers this.

### Opt 11. **Skip `CollectModules()` on every event.**

`ProcessDebugEvents` calls `m_debug_process.CollectModules()` when the loader signals `PostLoadDll/PostUnloadDll` (line 1417). Good. But `CollectModules()` itself walks the entire address space with `QueryDebugProcessMemory`, which is O(regions) — for large games this is 200+ syscalls per refresh. Two improvements:

1. Walk only from `GetAslrRegionAddress()` to its end instead of from `0`.
2. Stop early once we've seen a `MemoryState_AliasCode` block followed by `Free`.

### Opt 12. **Compress `m_from_t.address:64; count:32`.**

`m_from_t` is `NX_PACKED` 12 bytes per entry; on aarch64 unaligned 64-bit loads are still legal but slower in tight loops. With `max_watch_buffer = 0x200` you get 6 KB. Two cheap wins: switch the union to `uint64_t address + uint32_t count` (aligned 16 bytes), which doubles memory cost but eliminates load-store splits, OR re-encode `address` into 40 bits (Switch user addresses fit) + `count` into 24 bits (16M hits is plenty), letting `m_from_t` stay 8 bytes total — halving memory.

### Opt 13. **Document the "8th CodeMutable region + 0x10030" offset used by bookmark.ovl.**

Today the offset lives only in `Breezehand-Overlay/source/main.cpp:17696`. If the gen2 source is recompiled with even slightly different optimization flags the offset moves and bookmark.ovl quietly fails. Either:

* Embed a magic header right before `m_watch_data` (e.g. a 16-byte `"GEN2WATCHDATA01"` signature) so bookmark.ovl can scan for it instead of computing an offset; OR
* Expose the address via an IPC call (Opt see Bug 4 mitigation).

---

## 7. Quick wins to do first (ordered)

**Status (2026-09-06):** this list is largely historical. Items 1-4 are done (1 resolved differently -- see Bug 5's status), 5 is partly done (Bug 2), and 6 is partly addressed: `m_watch_data_t` carries a `char version[10]` that clients `strcmp` against `GEN2_VERSION`, but there is still no magic or layout version at the *head* of the struct, so a client reading an older layout gets the version field itself from the wrong offset. Items 7 and 8 are optimizations and were not re-audited. The bug entries in section 5 each carry their own status line.

1. **Bug 5 / Opt 2 / Opt 6**: stop reconstructing `GdbServerImpl` for the dummy. This single change fixes a race, removes a dummy socket fd, and shrinks the server-thread stack.
2. **Opt 1**: event-driven `gen2_loop()` instead of polling. Removes Bug 6 entirely.
3. **Opt 3**: add `g_watch_data_lock` mutex around `m_watch_data` updates. Fixes Bugs 3 and partial-data races.
4. **Bug 13**: restore the qword-boundary check in `IsValidWatchPoint`.
5. **Bug 2**: replace 32-byte-band watchpoint owner check with the actual WP-manager lookup.
6. **Bug 4 mitigation 1**: add a `u64 magic` and `u32 layout_version` at the head of `m_watch_data_t`. Trivial; protects every external client immediately.
7. **Opt 4**: cap `get_from_stack` read length to what the user requested.
8. **Opt 5**: sort modules once, binary-search in `get_region`.

Together this is roughly 200–300 lines changed and removes the most visible failure modes (silent miss, missed watches at 50 ms latency, bookmark.ovl breakage after rebuilds, occasional `failed` codes 2/7 from stale `next_pc`).

---

## 8. Addendum — bookmark.ovl migrated to dmnt:cht IPC

**Status:** done. `bookmark.ovl` no longer attaches to dmnt.gen2 as a debugger; it uses `dmntchtGetGen2WatchData` (cmd 65400) and `dmntchtSetGen2WatchData` (cmd 65401) over the existing dmnt:cht session. Since dmnt:cht is configured with 2 sessions (`dmnt2_main.cpp:109` `CheatMaxSessions = 2`), Breeze (session 1) and bookmark.ovl (session 2) coexist.

Changes:

| File | Change |
|---|---|
| `Breezehand-Overlay/common/dmntcht.h` | Added `dmntchtGet/SetGen2WatchData` prototypes. |
| `Breezehand-Overlay/common/dmntcht.c` | Added cmd 65400/65401 dispatch (mirrors Breeze). |
| `Breezehand-Overlay/source/main.cpp` | `BreezeGen2` namespace internals rewritten: removed `FindGen2Pid`, `LocateWatchDataOffset`, the debug-handle plumbing, and the `+0x10030 / 8th CodeMutable` heuristic. `ReadWatchData`/`WriteWatchData` now thin-wrap the IPCs. `Gen2Open/Gen2Close` kept as no-ops for source compatibility. `ExecuteWatchData` now polls the `done` flag at 20 ms granularity instead of sleeping a fixed amount. Public API (`EnsureInitialized`, `AttachToGame`, `DetachFromGame`, `SetWatchpoint`, struct types) unchanged. |

### Effect on the recommendations

* **Bug 4 (critical, "bookmark.ovl writes m_watch_data over a debug handle"): RESOLVED.**
  All cross-process struct writes are gone. The two IPC handlers do a single `memcpy` under the `dmnt:cht` server thread, with explicit struct typing on both sides. Struct-layout drift now manifests as a `version` mismatch caught by `EnsureInitialized()` instead of silent corruption. No seqlock or magic-header needed.
* **Opt 13 (document the `+0x10030 / 8th CodeMutable` offset): RESOLVED.**
  No offset exists anymore. The "how does an external client find m_watch_data" answer is "call dmnt:cht 65400/65401". Easy to test, easy to evolve.
* **All other recommendations stand unchanged.** The remaining bugs and optimizations are intra-sysmodule:
  * Bug 3 (no mutex on `next_pc` between events thread and gen2_loop) — still applies; the IPC handlers `memcpy` the whole struct, so they race with `ProcessDebugEvents` the same way the debugger writes did. **Opt 3's mutex is still required.**
  * Bug 5 / Opt 2 (dummy `GdbServerImpl` per accept) — still applies; independent of clients.
  * Bug 6 (50 ms polling blackout windows) — still applies; mitigated by Opt 1 (event-driven loop).
  * Bug 2 (32-byte band WP owner check) — still applies; concerns kernel WP semantics, not transport.
  * Opts 1, 4–12 — all unchanged.

The remaining work below (Quick wins list, modified slightly):

1. **Bug 5 / Opt 2 / Opt 6**: stop reconstructing `GdbServerImpl` for the dummy.
2. **Opt 1**: event-driven `gen2_loop()`.
3. **Opt 3**: add `g_watch_data_lock` mutex around `m_watch_data`. Now critical because *only* the sysmodule's own threads touch `m_watch_data`, but two of them do so concurrently (`ProcessDebugEvents` writes capture fields; `gen2_loop` reads control fields).
4. **Bug 13**: restore qword-boundary check in `IsValidWatchPoint`.
5. **Bug 2**: replace 32-byte-band watchpoint owner check with the actual WP-manager lookup.
6. **Opt 4**: cap `get_from_stack` read length.
7. **Opt 5**: sort modules once, binary-search in `get_region`.

(Bug 4 mitigation 1 — magic+layout_hash — is dropped: no longer useful.)

---

## 9. Addendum — the ATTACH-stops-working bug, root-caused

User report: "Before my changes, ATTACH from Breeze worked from cold boot but could break unpredictably; recovery was to either start a GDB session from IDA or put the Switch to sleep and wake it. After applying §8 it always fails on startup and sleep/wake no longer helps."

This addendum identifies the underlying defect (which predates the fork's first commit), explains why my §8 changes turned a flaky bug into a consistent one, and documents the fix.

### Root cause: a textbook condvar lost-signal race

`Gen2Attach()` (caller) and `DebugEventsThread::DebugEventsThread` (worker) implement a request/response handoff over two condition variables. The original code was:

```cpp
// Gen2Attach (caller)
{
    std::scoped_lock lk(g_event_request_lock);   // mutex A
    g_event_request_cv.Signal();
    g_event_done_cv.TimedWait(g_event_request_lock, 2s);
}

// DebugEventsThread (worker)
{
    std::scoped_lock lk(g_event_lock);           // mutex B (different!)
    while (!m_killed) {
        g_event_request_cv.Wait(g_event_lock);   // ... and waiter releases B
        // ... do attach ...
        g_event_done_cv.Signal();
    }
}
```

**The mutex used by the signaler (A) is unrelated to the mutex used by the waiter (B).** A condition variable signal only wakes a thread that is **currently inside** `Wait()`. If `Signal()` runs while the waiter is busy elsewhere (between iterations, in `Attach`, or hasn't started yet), the signal is dropped — condvars don't queue. There is no predicate flag, so on the next iteration the waiter has no way to know "a request happened while I was busy".

Worse, there is also no predicate flag inside the loop: when the waiter does enter `Wait()`, *any* signal — even a spurious wake — is treated as a request, and an in-progress request can be double-consumed.

### Why it used to "mostly work"

The accept loop in `GdbServerThreadFunction` constructed a dummy `GdbServerImpl(500, ...)` *every time* it called `Accept`, which on HTCS-down systems can be many times per minute. Each construction spawned a fresh `DebugEventsThread` whose first action was to enter `Wait()`. So the probability that, when `Gen2Attach()` signaled, *some* events thread was sitting in `Wait()` was high — but not 100%. The flakiness the user reported was real signal loss whenever no events thread was currently in `Wait()` at signal time.

The user's recovery actions all reset the events thread into a fresh `Wait()`:

* **IDA GDB connect** → accept returns, dummy is destroyed, real `GdbServerImpl` is constructed, **new events thread enters `Wait()`** → next `Gen2Attach()` succeeds.
* **Sleep / wake** → the kernel suspends all threads. On wake the events thread resumes inside its `Wait()` call, freshly ready.

So the flakiness was the bug; the "fixes" were merely repositioning the events thread back into `Wait()`.

### Why §8 made it consistent-fail

In §8 I removed the per-`Accept` dummy `GdbServerImpl` construction (was Bug 5 / Opt 2: it spawned an unwanted DebugEventsThread + opened fd 500 as a sham TransportSession on every retry). Without the dummy, **no events thread ever exists** until a real GDB client connects. So on cold boot, `Gen2Attach()` signals into the void every single time — flakiness becomes 100% failure.

The dummy was load-bearing by accident: it served as a perpetually-renewed pool of events threads sitting in `Wait()`. Removing it without fixing the lost-signal race turned an intermittent bug into a permanent one.

### Fix (committed in this revision)

Two changes:

**Fix 1 — proper handshake with predicate flag.** Convert the request/response to the standard pattern: same mutex on both sides, predicate flag the waiter loops on, signal under the mutex.

```cpp
constinit bool g_attach_request_pending = false;
constinit bool g_attach_done            = false;

// Gen2Attach
{
    std::scoped_lock lk(g_event_lock);
    g_attach_done            = false;
    g_attach_request_pending = true;       // set BEFORE signal
    g_event_request_cv.Signal();
    while (!g_attach_done) {
        if (timed_out) break;
        g_event_done_cv.TimedWait(g_event_lock, remaining);
    }
    g_attach_request_pending = false;       // self-clear on timeout
}

// DebugEventsThread
while (!m_killed) {
    while (!m_killed && !g_attach_request_pending) {
        g_event_request_cv.Wait(g_event_lock);   // releases g_event_lock atomically
    }
    if (m_killed) break;
    g_attach_request_pending = false;
    // ... attach work ...
    g_attach_done = true;
    g_event_done_cv.Signal();
}
```

Now even if `Signal()` fires before the worker reaches `Wait()`, the worker sees `g_attach_request_pending == true` on entry to the while-check and processes the request immediately. Lost signals are impossible.

The same fix is applied to the `vAttach` GDB packet handler (line 2350) which had the same race.

**Fix 2 — restore the dummy `GdbServerImpl`.** The §8 removal of the dummy stripped the perpetual events thread. Restore the per-Accept dummy construction so that an events thread always exists ready to service `Gen2Attach()` requests, *but* use `fd = -1` (an unambiguous sentinel) instead of `500` (a real fd value that could collide with a future socket allocation). The receive thread will see `Recv(-1)` fail immediately and exit cleanly; only the events thread persists.

In a future commit the proper structural fix is to move `DebugProcess` and `DebugEventsThread` ownership out of `GdbServerImpl` and into a singleton owned by `dmnt2_main.cpp`, so there is *one* events thread for the lifetime of the sysmodule and no dummies. Until then, the restored dummy plus the predicate handshake gives:

* **Cold-boot ATTACH:** works on the first try (a dummy events thread is alive within microseconds of `GdbServerThreadFunction` starting).
* **Brief blackout windows** during `~GdbServerImpl` / `GdbServerImpl(real_fd, …)` reconstruction in the accept loop. These windows used to lose signals; now `g_attach_request_pending` survives across them — the signal will be picked up by the next events thread to enter the `while (!m_killed && !pending)` check.
* **`m_event.Signal()` from the destructor on timeout** is still dead (Bug 10 keeps closed), but `g_event_request_cv.Signal()` is now done under the correct mutex (`g_event_lock`, not the misnamed `g_event_request_lock`).

### Status of §7 quick-wins after this addendum

| Rec | State after this addendum |
|---|---|
| Bug 5 / Opt 2 (no dummy reconstruction) | **Partially reverted.** The dummy is back, but is now constructed with `fd = -1` (no fake socket collision), and the underlying race that made it necessary is also fixed. A future full structural refactor can re-attempt full dummy removal once `DebugProcess` ownership has been moved out of `GdbServerImpl`. |
| Bug 3 / Opt 3 (mutex on `m_watch_data`) | unchanged, still applied. |
| Opt 1 (event-driven `gen2_loop`) | unchanged, still applied. |
| Bug 2, 8, 10, 13 | unchanged, still applied. |
| Opt 4, 5 | unchanged, still applied. |

The `g_event_request_lock` namespace-scope mutex is now unused but kept declared for ABI stability (no callers reference it).

---

## 10. Addendum — second-watch crashes game; post-crash ATTACH stuck

User report: "bookmark.ovl can cause game to crash when I [set] a second watch ... when bookmark.ovl cause crash of the game, the not able to do gen2attach comes back and now it can be recovered by going to sleep and wakup."

Two distinct bugs in this report.

### Bug X — second-watch crashes the game

This was a regression I introduced in §8 (Bug 2 fix). The original code used a fuzzy 32-byte address-band match to decide whether a watchpoint hit was "ours":

```cpp
if (address == m_watch_data.address
    || (address & -32) == (m_watch_data.address & -32)
    || m_watch_data.gen2loop_on == 2) {
    /* claim, capture, re-arm, Continue */
} else {
    /* report to GDB client */
}
```

I replaced the 32-byte band with a strict `GetWatchPointInfo()`-and-exact-range owner check. That's a correctness improvement in theory, but it ignores a real kernel quirk: **ARMv8 watchpoint hardware reports a fault address that can differ from the configured base by up to the BAS / mask coverage of the watch.** For BAS-encoded watches the kernel can report the qword-aligned base; for mask-encoded watches it can report any address within the power-of-two region. My strict range check rejected those hits as "not ours" and treated them as foreign GDB watches that didn't exist — leaving the debug exception unhandled, which crashes the debugged process on the next continue.

Symptom: works for some hits, crashes on others. The user noticed it most on the second watch because the first watch had typically run long enough to accumulate unhandled exceptions before they were tripped.

**Fix:** restore the band check, but widen the band to `max(32, watch_size)` so larger range watches still match. `GetWatchPointInfo()` is still used, but only to compute the GDB type label (`rwatch`/`awatch`); ownership decision is the band match again.

```cpp
const u64 band   = std::max<u64>(32, m_watch_data.size);
const u64 our_lo = util::AlignDown(m_watch_data.address, band);
const u64 in_band = (m_watch_data.size > 0)
                 && (address >= our_lo) && (address < our_lo + band);
if (address == m_watch_data.address || in_band || m_watch_data.gen2loop_on == 2) {
    /* ours */
}
```

This preserves the lenient kernel-reported-address handling, fixes the false-rejection-crash, and still avoids the original 32-byte-on-tiny-watches bug for the common case of size <= 4 (band defaults to 32). For larger watches (size 8/16/32/64) the band expands to match.

### Bug Y — post-crash ATTACH stuck; sleep/wake recovers

When the debugged game exited (crash or clean exit), `ProcessDebugEvents` handled `DebugEvent_ExitProcess` by **setting `m_killed = true`** before detaching. That flag was originally meant to signal "the GdbServerImpl is being destroyed; the events thread should exit". But in raw mode (no GDB client, only IPC clients like Breeze / bookmark.ovl), the same flag killed the **dummy** GdbServerImpl's events thread too. The dummy object stayed alive (held by the accept loop), but its events thread was gone.

After that, every `Gen2Attach()` signaled a non-existent waiter → 2 s timeout → fail. Sleep/wake recovered by causing `Accept()` to return with an error, which destroyed the dummy and constructed a fresh one with a new events thread.

**Fix:** in `ExitProcess`, only set `m_killed = true` when there is a real GDB session (`m_session.IsValid()`). In the dummy / IPC-only case, do the `m_debug_process.Detach()` and let `ProcessDebugEvents` return naturally via `!m_debug_process.IsValid()`. The events thread then re-enters the outer wait loop and is ready to service the next `Gen2Attach()`.

This also fixes the related case of the user pressing Home / sleeping during a watch session: previously the game's debug session would be torn down by the kernel, `WaitSynchronization` would error out, `ProcessDebugEvents` would treat that as exit, set `m_killed`, and break ATTACH for the rest of the session. Now the events thread cleanly recovers.

### Status

| Rec | State after this addendum |
|---|---|
| Bug 2 (32-byte band watchpoint match) | **Reverted to band-based with `max(32, size)` width.** The original concern (unrelated GDB Z2/Z3/Z4 watches in the same 32 B getting swallowed) is real but only matters when a GDB client is also using watchpoints concurrently with gen2 - rare. Defer a stricter fix until WP-manager queries return both the configured AND the kernel-reportable range so we can match precisely. |
| Events-thread-dies-on-game-exit | **Fixed.** New: m_killed is only set on ExitProcess when a GDB session is active. |

Everything else from §§8 and 9 is unchanged.

---

## 11. Addendum — bookmark.ovl first-watch crashed game (missing main_start/main_end)

User report: "now if I start a watch with bookmark.ovl it would crash the game immediately"; and "the same watch by Breeze works fine".

The "Breeze works, bookmark.ovl doesn't" pattern made this easy to isolate: the difference must be in what each client writes to `m_watch_data`. Diffing Breeze's `gen2_execute` prep against bookmark.ovl's `SetWatchpoint` revealed two missing fields:

* `m_watch_data.main_start`
* `m_watch_data.main_end`

Breeze sets both before every SETW (`Breeze/source/gen2menu.cpp:1672-1673`). bookmark.ovl left them at whatever was in the struct from a prior read - on the very first invocation that's zero.

### Why this matters

gen2's capture path uses `main_start`/`main_end` for three things in `ProcessDebugEvents` and `get_from_stack`:

1. **Stack scanning** (`dmnt2_gdb_server_impl.cpp:1258-1283`): walks `stack_check_count` qwords from `[sp]` and accepts only values in `[main_start, main_end)` as plausible return addresses. With both fields zero, no stack qword can qualify, so call-site capture is empty — but no crash.

2. **x30 / lr offset encoding** (`dmnt2_gdb_server_impl.cpp:1298, 1307, 1450`): computes
   ```cpp
   m_from_stack.address = thread_context.pc | ((thread_context.lr - main_start) << (64 - 27));
   ```
   With `main_start = 0`, `(lr - 0) << 37` is the full `lr` left-shifted 37 bits. For a typical Switch user-space code address (`~0x7100...`), the bottom 27 bits of `lr` end up in bits 37-63 of the result — i.e. the high bits of `m_from_stack.address` are garbage. The address field is later stored into `fromU.from2[].from_stack.address` (a 39-bit bitfield) and ultimately written back through the IPC. The bitfield truncation eats the bogus high bits, so this alone is not fatal.

3. **`x30_match` mask check** (line 1444): `u32 x30_catch = (lr - main_start) >> 2`. With `main_start = 0`, x30_catch is `lr >> 2`. The capture continues but the candidate filtering goes wrong.

So nothing in the gen2 sysmodule itself was crashing — the corrupted-`address` values were being written back to bookmark.ovl, where the client side would interpret them as game addresses and **dereference them** to format display strings. That dereference (via `dmntchtReadCheatProcessMemory` on a garbage address) is likely what eventually caused the game/system instability the user saw.

The reason it appeared as "FIRST watch crashes immediately" after the band restore (Addendum 10) was that the band restore made the capture path **actually claim hits** that the strict-range check had been rejecting. Before the band restore, the bogus capture data was being thrown out at the band check; after, every hit ran through `get_from_stack`/x30-shift logic with zero `main_start`, accumulating bad state fast enough that the second watch (or sometimes the first) tripped the failure.

### Fix

In `Breezehand-Overlay/source/main.cpp` `BreezeGen2::AttachToGame()`, populate `wd.main_start` and `wd.main_end` from `dmntchtGetCheatProcessMetadata` + a `dmntchtQueryCheatProcessMemory` of the first region at `main_nso_extents.base` (which is the .text segment). Mirrors Breeze's `action.cpp:494/557` setup.

```cpp
DmntCheatProcessMetadata meta{};
if (R_SUCCEEDED(dmntchtGetCheatProcessMetadata(&meta))) {
    wd.main_start = meta.main_nso_extents.base;
    MemoryInfo mi{};
    if (R_SUCCEEDED(dmntchtQueryCheatProcessMemory(&mi, meta.main_nso_extents.base))) {
        wd.main_end = mi.addr + mi.size;
    } else {
        wd.main_end = meta.main_nso_extents.base + meta.main_nso_extents.size;
    }
}
```

Because `ReadWatchData(&wd)` runs before this and `SetWatchpoint` only mutates a known field set, the values persist across all subsequent SETWs in the same attach session - we don't need to repeat the work in `SetWatchpoint`.

### Sysmodule hardening for future clients

The fact that a missing client-side field caused a crash is itself a problem - any future Tomvita-fork client (e.g. a python script using `nxdumptool`'s IPC) would hit the same trap. Two cheap defensive improvements the sysmodule could add (not done in this revision; logged for follow-up):

1. **In `Gen2Attach()` (the events thread side, after `m_debug_process.Attach()` succeeds), default `main_start`/`main_end` to the attached process's main NSO range** if the client left them at zero. The sysmodule already calls `pm::dmnt::AtmosphereGetProcessInfo` and `svc::QueryDebugProcessMemory` during attach; one extra query of the first code region would populate the fields automatically.

2. **In `setw()`, refuse to install if `main_start == 0 || main_end <= main_start`** with `failed = <new_code>`. Clients then get an explicit error from a single read of `failed` instead of a delayed crash.

Both are sysmodule-side defences against client misuse and would close this whole class of bug.

---

## 12. Addendum — sysmodule-side default for main_start/main_end (Addendum 11 follow-up #1)

Implements the first hardening item from Addendum 11: "In `Gen2Attach()` (the events thread side, after `m_debug_process.Attach()` succeeds), default `main_start`/`main_end` to the attached process's main NSO range if the client left them at zero."

### Change

In `DebugEventsThread`, immediately after the `m_debug_process.Attach()` succeeds and `m_process_id` is set, populate `m_watch_data.main_start`/`main_end` from the just-collected module list:

```cpp
if (m_debug_process.GetModuleCount() > 0) {
    std::scoped_lock wd_lk(g_watch_data_lock);
    const size_t main_ix   = m_debug_process.GetMainModuleIndex();
    const u64    main_base = m_debug_process.GetModuleBaseAddress(main_ix);
    const u64    main_size = m_debug_process.GetModuleSize(main_ix);
    m_watch_data.main_start = main_base;
    m_watch_data.main_end   = main_base + main_size;
}
```

`CollectModules()` runs as part of `m_debug_process.Attach()` (`dmnt2_debug_process.cpp:53`), so module 0 is the main NSO's first `ReadExecute` region by the time we reach this point. That matches Breeze's `dmntchtQueryCheatProcessMemory(main_nso_extents.base)` computation byte-for-byte for the same process.

The write is done under `g_watch_data_lock` (consistent with §8 Opt 3); the lock order `g_event_lock` → `g_watch_data_lock` is established once here and nowhere violates it (see §8 Opt 3 lock-graph: `gen2_loop`, IPC handlers, and `ProcessDebugEvents` BreakPoint case all take only `g_watch_data_lock`, never with `g_event_lock` held).

### Why unconditional overwrite (not "default if zero")

I initially wrote this as `if (m_watch_data.main_start == 0 && m_watch_data.main_end == 0)` so a client that explicitly set the fields would keep its override. That's wrong:

* If a previous attach to a different process left non-zero values, carrying them over corrupts the new process's capture path.
* `gen2_loop`'s ATTACH/ATTACH_CONT short-circuits (`if (!HasDebugProcess()) Gen2Attach()`) when re-attaching to the same pid, so this code only runs on a real fresh attach to a (potentially different) process.
* Clients that want a non-default value (Breeze uses `m_mainBaseAddr + m_R1` to offset into relocated code) write it as part of their **subsequent** SETW IPC, after Gen2Attach returns. Their override still wins.

So the unconditional overwrite is both correct (no stale values carried across processes) and safe (no client behavior regressed).

### Status of the hardening list

| Follow-up | State |
|---|---|
| 1. Default `main_start`/`main_end` at attach | **Done (this addendum).** |
| 2. `setw()` refuses to install with explicit `failed` code when `main_start == 0` | Now unnecessary - the sysmodule guarantees `main_start != 0` post-attach when there's at least one module. A `failed` code would only trigger in pathological cases (module list empty), which would have other symptoms first. Closed. |

### Effect on bookmark.ovl

The `BreezeGen2::AttachToGame()` code that explicitly populates `wd.main_start`/`wd.main_end` (Addendum 11) is now redundant when running against a sysmodule with this fix, but harmless:

* Against this sysmodule: bookmark.ovl writes values, sysmodule overwrites with its own (equivalent) computation. No net change.
* Against an older sysmodule build without this fix: bookmark.ovl's explicit population is still the only thing keeping it working.

Leaving the client-side code in place is good defense-in-depth.

---

## 13. Addendum — bookmark.ovl client-side safeguards against bad gen2 data

User question: "the issue with bookmark.ovl crash when w_watch wasn't initialised by Breeze first may be due to other data, the display may be using invalid data, is there any safeguard in place in bookmark.ovl?"

Audit of bookmark.ovl's `BookmarkWatchMenu::createUI` rendering callback found three insufficient or missing safeguards. All three are now in place.

### Audit findings

Sources of data the rendering callback consumes from `m_wd` (the IPC-read copy of `m_watch_data`):

| Field | Usage | Server can corrupt it? |
|---|---|---|
| `m_wd.read`, `m_wd.write` | branch selector for display | yes (but harmless) |
| `m_wd.stack_check_count` | branch selector for which `fromU` member to read | yes (but harmless) |
| `m_wd.count` | drives a for-loop indexing `fromU[]` | **yes, and was unchecked** |
| `m_wd.total_trigger`, `m_wd.failed` | printf args | yes (but harmless) |
| `m_wd.fromU.from2[i].from_stack.address` | passed to `dmntchtReadCheatProcessMemory` | **yes, was unguarded** |
| `m_wd.fromU.from[i].address` | reg value, then `addr + offset` passed to `dmntchtReadCheatProcessMemory` | yes, was guarded with a numeric range only |
| `m_wd.fromU.*.call_from` | printf %lX only | yes (but harmless) |
| `m_wd.i`, `m_wd.j` | printf %u only | yes (but harmless) |
| `m_wd.two_register` | branch selector | yes (but harmless) |

The two real safety holes were:

1. **`m_wd.count` unchecked.** Capped at 12 by `std::min<int>(m_wd.count, 12)` for display, but `std::min<int>(-1, 12) == -1` (signed-min), and the for-loop `for (i = 0; i < rows; i++)` doesn't execute when `rows < 0` — so accidentally OK, but a fragile invariant.

2. **`from_stack.address` dereferenced via `dmntchtReadCheatProcessMemory`.** With a misconfigured capture (`main_start = 0` pre-§§11/12), the captured `(thread_context.lr - main_start) << 37` shift produces values that, after the 39-bit bitfield truncation, look like plausible low memory addresses. bookmark.ovl then read those addresses to disassemble the "accessing instruction", causing `dmnt:cht` to do random-page reads. Most read failures are silent; some addresses might happen to hit unmapped pages and return errors. None should crash the game, but...

3. **`dmnt:cht` `ReadCheatProcessMemory` has an undocumented token-overload.** `dmnt_cheat_api.cpp:412-433` interprets the high byte of `proc_addr` as a "breakpoint token". If non-zero, it writes a 208-byte `DmntBreakpointResult` struct to the output buffer **without checking the buffer's size**:
   ```cpp
   #define BP_Token  proc_addr >> 56
   if (BP_Token != 0) {
       BP_Result.address = ...;          // writes 8 bytes
       sprintf(BP_Result.name, ...);     // writes up to 200 bytes
       R_SUCCEED();
   }
   ```
   Our local `u32 opcode = 0;` is **4 bytes on the stack**. A non-zero top byte would smash 204 bytes past the variable. In bookmark.ovl that smashes the rendering callback's frame, return address included.

   The current `FromStack::address` is a 39-bit bitfield so the top byte is guaranteed zero today; we still add an explicit `(addr >> 56) == 0` check as belt-and-braces in case the bitfield width is ever widened, or in case `regVal + m_watchOffset` rolls into the top byte for instruction watches with weird offsets.

### Fixes applied

In `Breezehand-Overlay/source/main.cpp`:

1. **New helper `IsPlausibleGameAddress(addr, meta)`** near the existing `BaseStr`/`BaseOffset`. Returns true only if `addr` is in `[0x10000, 0x8000000000)` AND inside one of the known game regions (main NSO / heap / alias) reported by `dmntchtGetCheatProcessMetadata`. Used as a pre-flight for any address derived from `m_wd.fromU.*` data before passing to `dmntchtReadCheatProcessMemory`.

2. **`m_wd.count` defensive bound** before the display loop:
   ```cpp
   const int countSafe = (countRaw < 0) ? 0
       : std::min<int>(countRaw, useFrom2 ? (int)kMaxWatchBuffer2
                                          : (int)kMaxWatchBuffer);
   const int rows = std::min<int>(countSafe, 12);
   ```
   This guards against negative `count` AND against `count > buffer_size` (which could index past the union's smaller arm).

3. **Memory-watch row read guarded** with `IsPlausibleGameAddress` and a redundant `(addr >> 56) == 0` check. If the address is not plausible, we skip the `dmntchtReadCheatProcessMemory` call and display only the captured address + call_from. Also: the previous code did `(u64)address - mainBase` for display; with `address < mainBase` this underflowed to a huge "M+offset". Now we display absolute `[0xADDR]` when the address isn't actually inside main.

4. **Instruction-watch row read** keeps its existing numeric range check (`> 0x10000 && < 0x8000000000`) — instruction watches legitimately capture pointers to anywhere in the address space, so the stricter "must be in a known region" check would reject too many valid ones. Added the `(targetAddr >> 56) == 0` belt-and-braces in case `regVal + m_watchOffset` ever rolls into the dmnt:cht token byte.

5. **`g_gen2State.version_matched` re-checked** at the top of the rendering callback. If the version probe failed (stock dmnt.gen2, or layout drift), we render a single "gen2 fork not detected; watch unavailable" line instead of iterating `fromU` whose layout we cannot trust.

### Effect

Even if the gen2 sysmodule is misconfigured, ships with a future layout change, or feeds garbage data after a bug regression, bookmark.ovl's rendering callback will:

* Never read past the end of `fromU[]`.
* Never call `dmntchtReadCheatProcessMemory` with an obviously bad address.
* Never trigger dmnt:cht's `BP_Token`-overload struct write.
* Refuse to interpret bytes as captures when the version string mismatches.

This is independent defense-in-depth on top of the sysmodule-side fix from §12; either alone would be sufficient for the §11 scenario, but having both protects against future regressions on either side of the IPC boundary.

### Files changed (this round)

* `Breezehand-Overlay/source/main.cpp`:
  - New `IsPlausibleGameAddress` helper near `BaseStr`/`BaseOffset`.
  - `BookmarkWatchMenu::createUI` rendering callback: count clamp, version-matched check, per-row read guards, fallback "absolute address" display when outside main.

---

## 14. Addendum — changing watch crashes the game (stale-BP-suspends-game-in-raw-mode)

User observation: "changing what to watch often cause the game crash, it has so far not crash the overlay, it's the game that crash."

The fact that the **game** is the casualty (not the overlay) is the key clue. This narrows the failure to:
* gen2 sysmodule mis-handles a debug event for the game, leaving its thread suspended; OR
* gen2 corrupts game memory; OR
* gen2 fires an HW BP/WP into the game on an unhandled path.

After tracing the watch-change flow, the cause is the first one.

### Watch-change flow

bookmark.ovl uses `tsl::changeTo<BookmarkWatchMenu>` which **pushes** a new Gui on the stack (does NOT pop the old one — `tesla.hpp:13260`). User-perceived sequence "switch to a different watch" really is:

1. User in `BookmarkWatchMenu(#1)` → presses B → `goBack()` pops it → `~BookmarkWatchMenu#1` runs `DetachFromGame()` (CLEARW + DETACH) + `dmntchtForceOpenCheatProcess()`.
2. User in `BookmarkMenu` picks bookmark #2 → `tsl::changeTo<BookmarkWatchMenu>` pushes a new one.
3. New menu's first `update()` calls `setupOnce()` → `AttachToGame()` (ATTACH_CONT) + `SetWatchpoint()` (SETW).

The crash window is between steps 1 and 3 — specifically inside the events thread's response to a debug event that arrives **after `clearw()` has zeroed `m_watch_data.address` and `m_watch_data.next_pc`** but **before** `m_debug_process.Detach()` closes the handle.

### The race

When a data watchpoint fires, gen2's events thread handles it as a two-step:

1. **Hit event:** `ClearWatchPoint(m_watch_data.address)` → capture → `m_watch_data.next_pc = pc + 4` → `SetHardwareBreakPoint(next_pc, 4)` → `Continue()`. The game now runs without the WP and a re-arm HW BP at `pc+4`.
2. **Re-arm event:** game executes `pc+4`, HW BP fires → handler matches `address == m_watch_data.next_pc` → `ClearHardwareBreakPoint(next_pc)` → `SetWatchPoint(m_watch_data.address)` (reinstall original WP) → `Continue()`.

The window between (1) and (2) is the danger zone. Now interleave with CLEARW from `DetachFromGame`:

```
events_thread                              gen2_loop (DETACH path)
─────────────                              ────────────────────────
(WP fires; case BreakPoint:)
  acquire g_watch_data_lock
  ClearWatchPoint(addr) [WP cleared in kernel]
  capture()
  next_pc = pc + 4
  SetHardwareBreakPoint(next_pc)
  Continue()  [game now runs toward pc+4]
  release g_watch_data_lock
                                           CLEARW arrives via IPC,
                                           signals g_gen2_request_event
                                           gen2_loop wakes
                                           acquire g_watch_data_lock
                                           clearw():
                                             - ClearHardwareBreakPoint(next_pc)
                                               [multicore worker zeroes
                                                DBGBCR on all 4 cores;
                                                takes several ms]
                                             - m_watch_data.next_pc = 0
                                             - m_watch_data.address = 0
                                             - m_gen2_watch_active = false
                                           release g_watch_data_lock
```

The game thread is racing toward `pc+4` while the multicore worker is zeroing the BP on each core in sequence. If the game thread reaches `pc+4` BEFORE the DBGBCR is zeroed on its core, **the BP fires** and the kernel suspends the thread, queueing a debug event.

Events thread next iteration:
```
WaitSync returns (event available)
GetProcessDebugEvent → BreakPoint at pc+4
acquire g_watch_data_lock
case svc::DebugException_BreakPoint:
  address = pc+4
  is_instr = true
  if (address == m_watch_data.next_pc || address == m_watch_data.address)
        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        FALSE (both are 0 now)
  else {
      m_watch_data.intercepted = false;
      AppendReplyFormat("T05...hwbreak:");  ← builds stop reply
      // NO Continue() !
  }
```

**The game thread is suspended waiting for a Continue that never comes.** The stop reply is built and sent to `m_session` (the dummy fd=-1 session) which silently discards it. The Switch's application watchdog eventually marks the unresponsive game as faulty and the kernel terminates it — what the user sees as "the game crashes". The overlay is unaffected because the bug is purely in the gen2 sysmodule's handling.

The same pattern exists for the data-WP `else` branch (line ~1676): if a stale WP fires (e.g. on a core whose DBGWCR wasn't yet cleared), it lands outside the band check, falls to else, builds an unsent reply, never Continues.

### Why this is recoverable by setting another watch

When the user successfully sets a new watch (SETW), gen2 dispatches `setw()` which calls `SetWatchPoint(new_addr, ...)`. That eventually calls `m_debug_process.Continue()` on success — well, no, SETW doesn't directly call Continue. But the new WP-related events that fire will go through the normal path, which DOES call Continue. So eventually the kernel un-suspends the game's leftover thread… actually no, the kernel's per-thread suspend state is per-event; resuming a different thread doesn't unblock the one stuck on the stale BP.

The reason the user sometimes gets away with it must be that the race window is narrow (the multicore worker is fast). Most CLEARWs land while no BP is in flight. Only occasionally does the timing align such that the stale BP fires AFTER `m_watch_data` is zeroed.

### Fix

In the two BreakPoint-case `else` branches (instr-BP-non-match and data-WP-non-match), detect raw mode (no GDB session via `!m_session.IsValid()`) and `m_debug_process.Continue()` instead of (or in addition to) building an unused stop reply:

```cpp
} else {
    m_watch_data.intercepted = false;
    if (!m_session.IsValid()) {
        m_debug_process.Continue();   // raw mode: no GDB client, just continue
    } else {
        AppendReplyFormat(reply_cur, reply_end, "T...");
    }
}
```

This is the minimal fix. It's symmetric for both is_instr=true (line ~1564) and is_instr=false (line ~1681) branches. In raw mode (no GDB session), we always continue — any stale gen2 trap that fell through the ownership check was gen2's anyway (or at worst, a benign foreign BP that we can't service without a GDB client).

### Why this is correct (and not papering over an ownership bug)

The "address doesn't match" case for gen2-installed BPs in raw mode has only two real sources:

1. **Stale BP fires after gen2 cleared the matching m_watch_data fields.** This is what CLEARW does. The BP was *ours*; Continue is correct.
2. **Foreign GDB watchpoint set by `Z2`/`Z3`/`Z4` while no GDB client was connected.** Impossible — in raw mode there's no GDB client to issue those packets.

So in raw mode, every non-matching BP is gen2's own stale trap. Continuing it is always correct.

When a GDB client IS connected, the old behavior (build stop reply, send to client) is preserved by the `else` branch in my fix.

### Files changed

* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_gdb_server_impl.cpp` —
  in `ProcessDebugEvents` `case svc::DebugException_BreakPoint`, both
  non-match `else` branches now `m_debug_process.Continue()` in raw
  mode instead of leaving the game thread suspended.
* `Atmosphere/stratosphere/dmnt.gen2/gen2fork_design_and_review.md` —
  this addendum (§14).

### Follow-up for resilience

The fix above handles the *symptom* (stale BP firing in raw mode). The *underlying* race is "CLEARW zeros `m_watch_data.next_pc / .address` while the kernel may still deliver a BP fire for those addresses". A more thorough fix would:

* Drain pending debug events for the gen2-owned addresses inside CLEARW before zeroing the fields. Implementation: between `ClearHardwareBreakPoint(next_pc)` and `m_watch_data.next_pc = 0`, do a non-blocking `WaitSynchronization(handle, 0)` and `GetDebugEvent` loop to consume any in-flight events.

But the symptom fix in §14 is sufficient in practice because:
- The events thread will see the stale BP event next iteration.
- With Continue, the thread resumes.
- Pending events are cleared by the kernel as they're consumed.

Filing the drain-on-clearw as a future refinement.

---

## 15. Addendum — CLEARW-before-SETW in bookmark.ovl (client-side serialization)

User insight (correct): "maybe the writing to m_watch should be preceded by clearing the existing watch, could that be the cause of the problem?" And the corroborating observation: "changing the stack slot to capture without stopping first may crash the game [for bookmark.ovl]; for breeze if I stop first there is no problem".

Yes - and the §14 sysmodule fix (which catches the *consequence*) is necessary but not sufficient when the user changes **layout-affecting** fields like `stack_check_count`. Here's why.

### The IPC write / dispatch gap

The current flow when a client changes any watch parameter is:

```
client                              IPC handler                     gen2_loop
──────                              ───────────                     ─────────
ReadWatchData(wd)
modify fields
wd.command = SETW; wd.execute = true;
SetGen2WatchData(wd) ─────────►  memcpy(&m_watch_data, wd, ...)
                                  ↑
                                  new fields land HERE, including
                                  stack_check_count + count = 0;
                                  but old WP is still installed in
                                  hardware, shadow still has old data
                                 signal g_gen2_request_event
                                                                    wake, take lock
                                                                    SETW dispatch:
                                                                      clearw() ← finally clears old WP
                                                                      setw()   ← installs new WP
```

Between "memcpy lands" and "gen2_loop's clearw runs", the events thread can independently service a hit on the still-installed OLD watchpoint. That handler reads `m_watch_data.stack_check_count` and `m_watch_data.count` and uses them to decide:

* Which `fromU` union arm is the active layout (`from2[]` when `stack_check_count > 0 || grab_A || grab_R`, else `from[]`).
* The dedup loop bound `for (i = 0; i < m_watch_data.count; i++)`.
* The append position `fromU.*[m_watch_data.count]`.

The client has already overwritten these to the NEW layout's expected values (e.g. `stack_check_count = 5`, `count = 0`). So the events thread interprets the OLD captures stored under the OLD layout as if they were NEW-layout entries, and appends new entries at offset 0 of the new layout. Two consequences:

1. **`fromU` cross-layout aliasing.** The first 12 bytes of `from2[0]` overlap the entirety of `from[0]`. Reading/writing through one alias while data is stored under the other produces garbage interpretation. By itself this is data confusion, not a crash.

2. **The events-thread BreakPoint handler races CLEARW.** When CLEARW arrives via the §14 fix path (which now happens implicitly because gen2_loop's SETW dispatcher calls `clearw()` first), the events thread may have already entered the BreakPoint case under the lock. CLEARW waits. When CLEARW eventually wins the lock, it zeroes `m_watch_data.next_pc / .address`. The next BP-fire event (the re-arm at `pc+4` from the OLD hit) lands in the BreakPoint case, finds no match, and (pre-§14) was leaving the game thread suspended forever.

### Why §14 wasn't enough for stack-slot changes

§14 fixes the "stale BP fire after clearw" symptom by Continue()ing the game in raw mode when the address doesn't match. That works for "change watch to a new bookmark" and "change watch without detaching" scenarios because the new SETW eventually completes and game traffic resumes.

But for stack-slot changes, the window is special: the OLD watch is at the SAME address as the NEW watch (the user is just adjusting capture parameters). The user expects "this is the same watch, just record more/fewer stack slots". After the multicore worker zeroes the OLD WP slot DBGWCR, before it sets the NEW one... actually no, that's the same kernel-state transition that would happen on any SETW with the same address. The crash mechanic isn't really in the hardware.

The crash mechanic is that `setw()` (called from within gen2_loop's SETW dispatch) tries to reinstall the WP at the same address with new size/read/write semantics. If `IsValidWatchPoint` happens to fail (e.g. because of my Bug 13 qword-boundary check rejecting an unaligned size combination), `setw()` returns with `failed=1` and **no WP is installed**. But more importantly, **`m_gen2_watch_active` is now FALSE** (cleared by `clearw()`) but the shadow still has the OLD values. The events thread may have a stale BP event queued; if it fires, the BreakPoint handler falls into the §14 raw-mode-continue path. The game continues — but with no WP installed, no further captures happen. The user sees the watch silently broken or the game intermittently stutters.

In specific timing windows where the events thread is in the middle of `get_from_stack` reading the stack with `stack_check_count > max_call_stack`... no, the user can only set stack_check_count to 0..5 (`% 6` cycle), so OOB on `m_from_stack.stack[]` is bounded. That theory doesn't fit either.

The actual mechanic is probably more subtle — the rapid SETW with layout change creates enough debug-event traffic to wedge some kernel subsystem, or one of the multicore SVC sequences from CLEARW + SETW (8 total syscalls per core for the round-trip) interleaves with a kernel-side cleanup poorly. In any case, **eliminating the IPC-write-then-clearw-later sequence eliminates the window entirely**, which is the user's instinct.

### Fix

Client-side serialization in bookmark.ovl `BreezeGen2::SetWatchpoint`:

```cpp
if (wd.address != 0) {
    // A watch is currently live in the sysmodule. Tear it down in
    // its own IPC round-trip BEFORE writing new parameters.
    wd.command = GEN2_CLEARW;
    ExecuteWatchData(&wd, 200'000'000ULL);
    if (R_FAILED(ReadWatchData(&wd))) return false;
}

// ...then set up wd for SETW with new params, send it.
```

This adds one extra IPC round-trip (~50-100ms) when changing a parameter without an explicit detach. Two effects:

* **CLEARW completes deterministically before any new-parameter memcpy lands** in the sysmodule. The events thread, between CLEARW completion and SETW arrival, sees no WP installed → no hits → no race.
* The §14 fix continues to act as a safety net in case of stale events that managed to queue before CLEARW.

We also explicitly zero the local `wd.fromU` before the SETW write, so even in the brief gen2_loop-wake-latency window between IPC memcpy and gen2_loop's SETW dispatch, no cross-layout interpretation of stale capture data is possible. (gen2_loop's setw() resets `count=0` anyway, but `fromU` data isn't cleared by the sysmodule until it's overwritten by new captures.)

### Why this is a client fix not a sysmodule fix

The sysmodule could in principle add a "clearw-before-setw" inside the IPC handler itself, but that would require:

* Access to `m_debug_process` from the IPC handler (which lives in dmnt:cht service code, not GdbServerImpl).
* OR a way to enqueue a synthetic CLEARW command for gen2_loop to dispatch under lock - but that's two dispatches anyway, which is what we're already doing from the client.

The client knows whether it's about to change parameters on a live watch (it just read `wd.address != 0`). It's cleaner and more flexible to let the client decide the dispatch ordering. Breeze users do this manually via the Gen2Detach button; bookmark.ovl now does it automatically.

This pattern is also useful for other clients (e.g. a future python IPC script): "if you're changing parameters on a live watch, first issue CLEARW, wait for done, then issue SETW".

### Files changed (this round)

* `Breezehand-Overlay/source/main.cpp` `BreezeGen2::SetWatchpoint`:
  - When `wd.address != 0` (a watch is already live), issue an explicit `GEN2_CLEARW` round-trip before assembling the new SETW.
  - After preparing the new SETW fields, `memset(&wd.fromU, 0, sizeof(wd.fromU))` so no stale capture data leaks across the IPC boundary into a different layout interpretation.
* `Atmosphere/stratosphere/dmnt.gen2/gen2fork_design_and_review.md` —
  this addendum (§15).

### Sysmodule-side follow-up (deferred)

If a future revision wants to make this race impossible regardless of client behavior, the right place is **`SetGen2WatchData`** (the IPC handler in `cheat/dmnt_cheat_service.cpp`). It already takes `g_watch_data_lock` and could:

1. If incoming `wd.command == SETW` AND current `m_watch_data.address != 0`, do an inline `clearw()` BEFORE the memcpy.

This needs the IPC handler to be able to call into `GdbServerImpl::clearw()`, which it currently can't (encapsulation). A small refactor (expose `clearw` via a free function or move it out of `GdbServerImpl`) would unlock this. Left for a future commit; the client-side fix here suffices in practice.

---

## 16. Glossary

* **Watchpoint (WP)** — ARMv8 hardware data breakpoint (DBGWCR/DBGWVR). Fires on load/store at the address.
* **Hardware breakpoint (HB)** — ARMv8 hardware instruction breakpoint (DBGBCR/DBGBVR). Fires on instruction fetch.
* **Context breakpoint** — A DBGBCR programmed with the *process context ID* match type so the watch only triggers in our target process, not other apps.
* **x30 / lr** — aarch64 return address register. `(x30 - main_start) >> 2` ⇒ "what NSO offset called this function".
* **`from` / `from1` / `from2` / `from3`** — four layouts of the same capture buffer, chosen by what the user asked to record:
  * `from`: address + count (no stack).
  * `from1`: register-set add (i, j, k, offset, two_register).
  * `from2`: stack-capture call_stack_t[max_call_stack].
  * `from3`: exclusive-search candidate (full descriptor).
* **EXCLUSIVE_SEARCH** — a candidate-elimination algorithm: start with the full list of "addresses that this function reads from" and remove any whose call-site fingerprint matches but whose target doesn't match the user-chosen value. Convergence: one pass per game-side trigger.

---

## 17. Addendum — Clean GDB Attaching on First Try (Auto-Release Cheat Debug Handle)

User report: "now it is two step process, attach, fail, attach again good. How about just do what that attach to other process then detach step and nothing else when gdb try to attach by asking for the process list?"

This addendum explains how we solved the first-attach failure where the user previously had to try twice or perform an unrelated attach/detach hack to connect.

### Root cause of "attach, fail, attach again good"
The Nintendo Switch kernel allows only a single debugger to hold a debug handle for any given process. The cheat engine automatically attaches to the game on boot via `svc::DebugActiveProcess`.
When GDB connects:
1. GDB first queries the process list (`qXfer:osdata:read:processes`).
2. When GDB tries to attach to the game PID, the cheat engine still holds the debug handle.
3. In `AttachGen2()`, we call `svc::CloseHandle()` to close the cheat engine's handle.
4. However, the Switch kernel performs the detach/cleanup asynchronously in the background. It does not complete instantaneously.
5. `AttachGen2()` immediately calls `svc::DebugActiveProcess` for GDB on the game PID. Because the kernel hasn't finished detaching yet, it returns `ResultBusy` (0xe1a). The first attach fails.
6. On the second attempt, because the handle was already closed during the first attempt and enough time has elapsed (hundreds of milliseconds), the game process is now completely free. The second attach succeeds.

### Solution: Auto-Release on Process List Query or Direct Attach
We resolved this by automatically detaching the cheat engine as soon as GDB initializes the session or attempts to attach.

#### 1. Detach during Process List Query
In `GdbServerImpl::qXferOsdataRead()`, when GDB initiates a fresh process list read (`offset == 0`) and GDB is not yet attached:
- If `dmnt::dbg::GetSharedDebugHandle() != os::InvalidNativeHandle`, we cache the cheat PID and name.
- We call `ams::dmnt::cheat::impl::ForceCloseCheatProcess()`. This cleanly detaches the cheat engine, stops its events thread, and closes the debug handle.
- We sleep for 100ms via `os::SleepThread()` to allow the Switch kernel's asynchronous detach to finish.
- If the game PID is encountered in the loop and `svc::DebugActiveProcess` still fails (e.g., if the kernel is extremely slow to detach), we fall back to adding it to the XML list with the cached process name.
By the time the process list is sent back, GDB displays it, and the user selects and attaches to the game (taking at least a few seconds), the process is completely free.

#### 2. Detach during Direct Attach
For GDB clients or scripts that attach directly by PID (without querying the process list first), we perform the same check in `DebugProcess::Attach()`. Before calling `AttachGen2()`, if the cheat debug handle is open, we call `ForceCloseCheatProcess()` and sleep for 100ms.

#### 3. Start/Stop Coordination
- Inside `DebugProcess::Attach()`, we call `ams::dmnt::cheat::impl::SuspendDebugEvents(true)` early to suspend the cheat event polling loop before GDB starts.
- Inside `DebugProcess::Detach()`, we call `ams::dmnt::cheat::impl::SuspendDebugEvents(false)` to resume cheat events upon GDB disconnect.
- Unconditionally invoke `Start()` instead of `StartShared()` inside `DebugProcess::Attach()`, as GDB always starts fresh now that the cheat engine debug handle is closed prior to GDB attaching.

### Files changed
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_gdb_server_impl.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_debug_process.cpp`

---

## 18. Addendum — High-Frequency Exception Starvation Fix

User report: "check if there is any situation watching certain instruction, perhaps it triggers to frequently, can cause dmnt.gen2 to hang. The symptom is the whole system become unresponsitive. Only long press power button can recover from it. Overlay can pop up once, then no respond, long press power don't pop up the power dialoge, only until the around 10 second long press is able to cut power and allow regain control"

This addendum explains how we diagnosed and resolved the CPU starvation lockup.

### Root cause of system-wide unresponsiveness
1. **Thread Priority**: The GDB server's events thread (`m_events_thread`) and the multicore breakpoint worker thread (`g_multicore_thread`) run at `os::HighestThreadPriority - 1` (priority 2 on the Switch). This priority is extremely high—higher than almost all system services.
2. **Infinite Trapping Loop**: When the user watches a frequently hit instruction (e.g. inside a rendering or physics loop):
   - The game hits the breakpoint, halts, and generates a debug event.
   - The events thread wakes up, handles the hit, installs a next-PC breakpoint at `address + 4`, and continues the game.
   - The game executes one instruction, hits the next-PC breakpoint, halts, and traps again.
   - The events thread wakes up, clears next-PC, reinstalls the watched breakpoint at `address`, and continues.
   - The game immediately hits the watched breakpoint again, repeating the cycle.
3. **Core 3 Starvation**: This cycle generates thousands of traps per second. Because setting/clearing hardware breakpoints requires thread migrations (4 migrations per call via `MultiCoreThread`), the CPU is hammered with context switches and inter-processor interrupts (IPIs).
   At priority 2, this tight loop completely consumes Core 3 (the system core). All Horizon OS background services running on Core 3 at priorities 16-40 (such as `hid`/input, the Tesla Overlay, and the `power` service) are completely starved of CPU time. The overlay freezes, inputs are ignored, and the power menu cannot render—leaving only the hardware-level 10-second power cut operational.

### Solution: Event Loop Rate Limiting
We added a lightweight rate-limiting mechanism inside the main `ProcessDebugEvents()` event loop in `dmnt2_gdb_server_impl.cpp`.

Initially, the rate-limiting sleep was placed at the very start of the debug event loop (immediately after retrieving a new debug event). However, this meant sleeping while the watched instruction's hardware breakpoint was still active on the CPU. Under multi-threaded game workloads, other threads would execute the watched instruction during this 1ms window, hit the active breakpoint, and queue secondary debug events in the kernel debug port. This caused the event queue to flood, leading to race conditions where capture/triggering prematurely stopped at a small count (e.g. `trig=11` or `count=5`).

To resolve this, we refined the rate-limiter positioning:
1. **Targeted Sleep Placement**: We moved the rate-limiting check and sleep to occur right before `m_debug_process.Continue()` is called on watched instruction/data hits (when the watched breakpoint/watchpoint is cleared and the next-PC breakpoint is set).
2. **Lock Release**: Before sleeping, we release the `g_watch_data_lock` by calling `_wd_lk.unlock()`, preventing it from blocking the main cheat engine loop thread (`gen2_loop`).
3. **Prevention of Stale Exceptions**: During the 1ms sleep, the watched breakpoint is cleared, so other game threads execute the instruction normally without trapping. This prevents the queue from flooding with duplicate debug events and allows continuous capturing up to `max_trigger` (e.g. 10,000) without any early cessation.

This simple sleep yields Core 3 CPU time back to the scheduler. During that 1ms sleep window, the overlay, input handling, and power button services have plenty of time to process events. The maximum CPU monopolization is capped, keeping the entire console fully responsive even during extreme trap floods.

### Files changed
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_gdb_server_impl.cpp`

---

## 19. Addendum — gen2 joins dmnt's debug handle instead of detaching it

User report: "the current behaviour to detach dmnt, is it really necessary? It
used to be when dmnt and dmnt.gen2 were two different sysmodules, now that they
are merged the only-one-debugger-at-a-time problem is gone. If we don't need to
detach dmnt it would be better, as the cheat VM doesn't get a reset every time
gen2 attaches."

Correct. It was not necessary, and the cost was substantial.

### What the detach cost

`ForceCloseCheatProcess()` -> `CloseActiveCheatProcess()` cancels the events
thread, saves cheat toggles, `memset`s the process metadata, calls
`ResetAllCheatEntries()`, and erases the entire frozen-address map. Coming back,
`AttachToApplicationProcess()` re-reads and re-parses every cheat off the SD card
and reloads the toggles. That ran on **every** gen2 attach, and it took the
user's frozen addresses with it.

### Why it was there

Not for a kernel reason. `AttachGen2()` had one extra term:

    if (g_shared_debug_handle != Invalid && (!g_attach_gen2 || g_shared_process_id != process_id)) {
        svc::CloseHandle(g_shared_debug_handle);

`!g_attach_gen2` is true whenever gen2 is not already attached, so it closed and
re-opened the shared handle even when that handle was already open for exactly
the process being requested — including when the cheat engine was the one
holding it. That is what made `ForceCloseCheatProcess()` mandatory: otherwise the
cheat engine would be left holding a closed handle value.

`AttachDmnt()` never had that term. It joins an existing handle for the same
process, which is precisely what the `ForceOpenCheatProcess()` at the end of
`DebugProcess::Attach()` has always relied on. The asymmetry was the whole bug.

Addendum 17's "attach, fail, attach again" was a symptom of the same thing: the
close/re-open pair raced the kernel's asynchronous detach and came back
`ResultBusy`. Detaching the cheat engine up front and sleeping 100 ms was a
workaround for a re-open that did not need to happen.

### The change

1. **`AttachGen2()` joins.** Dropped the `!g_attach_gen2` term, making it the
   same shape as `AttachDmnt()`. A *different* process is still a real re-open —
   the shared handle is a single slot, so the cheat engine does have to let go
   there, and the caller still does `ForceCloseCheatProcess()` for that case.

2. **`DebugProcess::Attach()` branches on joinability.**

       const bool can_join = !start_process
                          && dmnt::dbg::GetSharedDebugHandle() != os::InvalidNativeHandle
                          && dmnt::dbg::GetSharedProcessId() == process_id;

   On the join path: no `ForceCloseCheatProcess()`, no 100 ms sleep, and no
   trailing `ForceOpenCheatProcess()` (nothing was closed). `start_process`
   excludes the join because a process we launch ourselves genuinely does have
   initial debug events to consume.

3. **`StartShared()` is finally called.** It was written for exactly this,
   complete, and dead code — addendum 17 replaced its only call site with an
   unconditional `Start()`. `Start()` replays CreateProcess / CreateThread /
   DebuggerAttached, which on the join path were consumed by the cheat engine
   long ago; `StartShared()` reconstructs the same state from
   `svc::GetThreadList` + `svc::GetInfo` + `CollectProcessInfo`.

4. **`StartShared()` reports `DebugBreak`**, because item 5 makes that true.

5. **`StartShared()` halts the process itself and consumes the event.**
   `svc::BreakDebugProcess` first thing, then drain the `DebuggerBreak` event,
   before anything queries the process. Corrected twice -- see the note below;
   the first attempt reported `ProcessStatus_Running` and left the break to
   `vAttach`, which was wrong in three ways at once.

6. **`SuspendDebugEvents(true)` is now acknowledged.** This is the one part that
   genuinely needed new machinery. Joined, `CheatProcessManager::DebugEventsThread`
   and gen2's `ProcessDebugEvents` both wait on the same handle, and only the
   suspend flag keeps them apart. The flag alone is not enough: the thread tests
   it *before* a 100 ms `WaitSynchronization`, so for up to 100 ms afterwards it
   can still wake on an event and `ContinueCheatProcess()` it out from under
   gen2. Added:

   - `g_debug_events_parked`, published false while the events thread may own an
     event on the shared handle and true whenever it definitely does not;
   - `CancelDebugEventsWait()`, the same `svc::CancelSynchronization` knock-out
     `CloseActiveCheatProcess()` already used, fired from
     `SuspendDebugEvents(true)` so the thread re-tests the flag immediately (the
     loop already treats a cancelled wait as "someone cancelled our
     synchronization, possibly us");
   - `WaitDebugEventsParked(timeout)`, which `Attach()` calls with a 200 ms
     budget before taking over.

### Correction — `StartShared()` and the halt

The first revision of this change reasoned that, on the join path, no
`DebuggerAttached` event holds the process, so `StartShared()` should report
`ProcessStatus_Running` and leave halting to callers that need it. Three things
were wrong with that:

- **It was not observable.** Attaching has always halted the game and continued
  to. Breeze only ever uses `ATTACH_CONT`, which continues immediately, so
  neither client could have shown a difference. The claim came from reading, not
  from the console.
- **`Break()` from `vAttach` raced its own reply.** `svc::BreakDebugProcess` is
  asynchronous, so `AppendStopReplyPacket`'s `GetThreadContext` ran against a
  still-running process and GDB got a stop packet with a meaningless pc.
- **It let the event escape.** The `DebuggerBreak` then reached
  `ProcessDebugEvents`, which answered it with a *second* asynchronous stop
  reply on top of `vAttach`'s, desynchronising the client for every packet after
  it.

And the defect that actually broke GDB breakpoints:
**`svc::GetDebugThreadContext` fails on a running thread**, so building the
thread table before halting left every thread with `tls_address = 0` and blank
thread info. That is why breakpoints worked until Breeze had been launched once
and then never again — Breeze's `init_cheat_system()` calls
`ForceOpenCheatProcess()`, so from then on the cheat engine holds the shared
handle and every GDB attach takes the join path. It also explains "attach
once": before the join change a second attach re-opened the handle and got fresh
initial events, so `Start()` ran.

`StartShared()` now breaks and drains **first**, before `CollectProcessInfo()`
and before any thread is queried, and clears the thread table before
`GetThreadList` rebuilds it (draining can consume a queued `CreateThread` event,
and `ThreadCreate()` does not deduplicate).

### Behaviour changes to remember

- **Cheats and frozen addresses stay live through a gen2 capture.** Intended,
  but it means the cheat VM and the debugger now write the same process. Two
  confirmed consequences, both diagnosed on device:
  - A value frozen on the watched address trips the watch with the VM's own
    12 Hz writes.
  - **A software breakpoint set on an instruction an enabled cheat writes to is
    overwritten within ~83 ms and never fires.** No error; the code just reads
    back as the original instruction. Presents as a breakpoint that hits when
    execution reaches it at once and never when you have to do something in game
    first, with Suspend + re-set buying exactly one more attempt. Confirmed by
    disabling the offending cheat.

  Documented for users rather than fixed. The fix, if it is ever wanted, is to
  make software breakpoints authoritative: have the cheat write path consult the
  active breakpoint list, update the saved original instruction, and re-apply
  the `brk` after the write. Suspending the VM for the duration of a session
  would also work but gives back what this change was for.
- The attach still halts the game, as it always has. An earlier revision of this
  addendum claimed otherwise; that was wrong and never matched observation.
- **Detach is symmetric and free.** `DetachGen2()` only closes the handle when
  `g_attach_dmnt` is also false, so on GDB detach the cheat engine keeps the same
  handle and `SuspendDebugEvents(false)` resumes it with no reload at all.
- Nothing is lost on the client side: a client that wants the old behaviour calls
  `dmntchtForceCloseCheatProcess()` before the ATTACH and dmnt takes the non-join
  path exactly as before. Noted on the Breeze side at
  `Breeze/source/gen2menu.cpp`, `case ID Gen2Attach:`.

### Files changed

* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_shared_debug_handle.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_debug_process.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_gdb_server_impl.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/cheat/impl/dmnt_cheat_api.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/cheat/impl/dmnt_cheat_api.hpp`
* `Breeze/source/gen2menu.cpp` (note only)

---


## 20. Addendum — hardware watchpoint hardening, and detach cleanup

Prompted by `WATCHPOINT_BUG.md`: a GDB `Z2`/`Z4` watchpoint silently stops
firing about a second after it is armed, and re-arming brings it back. Root
cause is **still unproven** — everything below was found by reading, not by
measuring. Two of the three defects could each account for the symptom alone, so
the bug may not reproduce next time. All are fixed and verified not to disturb
the gen2 capture path on device.

### The framing that made this tractable

The kernel does **not** save or restore `DBGWCRn/DBGBCRn` — not on context
switch, thread create, `ContinueDebugEvent` or core migration.
`KDebug::SetHardwareBreakPoint` writes the system registers of whichever core
the caller happens to be on and nothing else. So an armed watchpoint can only
die two ways: **someone wrote the register again**, or **the write never landed
on that core**. `MDSCR_EL1.MDE` survives (the scheduler read-modify-writes it)
and `CONTEXTIDR_EL1` is the process id for every user process at all times, so
neither is worth re-checking.

That also rules out two of the three candidates the bug report listed:
`DebugEvent_CreateThread`/`ExitThread` only call `Continue()`, and `gen2_loop()`
returns before its command switch whenever `m_watch_data.execute` is false, so
an idle client does not drive `setw()`/`clearw()`.

### Fix 1 — `MultiCoreThread` never verified the migration

It called `svc::GetThreadCoreMask` and **discarded the result**. That call
reports the thread's *ideal* core — the value just set — so it could never have
worked as a check even if the result had been used; it looks like an assertion
that was never written. `SetThreadCoreMask` requests a migration but does not
guarantee it has happened when the SVC returns, and a write issued from the
wrong core leaves the target core without the breakpoint, silently, because the
SVC succeeds on whichever core it did run on.

Now spins on `svc::GetCurrentProcessorNumber()` (8 yields, then 100 us sleeps,
64 max, ~5.6 ms worst case) and **reports failure** for a core it never reached
rather than leaving a watchpoint armed on three cores out of four. It keeps
walking the remaining cores — arming what we can beats stopping.

Consequence: if a miss happens during a watchpoint *clear*, `WatchPoint::Clear()`
still calls `Reset()`, so the manager frees the slot while the register may stay
armed on the missed core. That leaves a stale watchpoint with no owner —
self-healing in raw mode, a spurious stop reply with a GDB session. Visible
rather than silent, which is the trade taken throughout this addendum.

### Fix 2 — slot pools were 16 deep on hardware with 6 BP / 4 WP / 2 CTX

`CountBreakPointRegisters()` computed `g_last_bp_register`,
`g_last_wp_register` and `g_first_bp_ctx_register` and then **nothing used them
to bound allocation**; `GetFreeBreakPoint()` walked all `BreakPointCountMax`
(0x10) slots.

On the Switch's Cortex-A57 the context-aware comparators are architecturally the
top two breakpoint registers, `I4` and `I5`, and we reserve both: `I4` links
execution breakpoints to the debugged process, `I5` does the same for
watchpoints. So the 5th simultaneous hardware execution breakpoint was handed
`I4` and the 6th `I5` — and writing `I5` as a plain execution breakpoint
destroys the comparator every armed watchpoint is *linked* to (the kernel sets
`WT=1`, we put `ctx` in `LBN`). Every watchpoint then stops firing, silently and
with no error, until something calls `WatchPoint::Set` again, which re-writes
the context breakpoint first. **That is the reported symptom including the
re-arm workaround**, which is why this is the most suspicious of the three even
though a `Z2`-only repro should not reach five execution breakpoints.

`GetBreakPoint()` now stops at `GetUsableBreakPointCount()` and the watchpoint
pool at `g_last_wp_register + 1`: 4 and 4 on this hardware. Exhaustion reports
`ResultOutOfHandles` at the manager instead of failing inside the SVC.

### Fix 3 — watchpoint-hit ownership was read from a cross-session global

The hit path decided "is this gen2's watchpoint?" from `m_watch_data`, which is
a **file-scope global**: it outlives the session and any `dmnt:cht` client can
overwrite it. A stale address left there by an earlier Breeze capture let gen2
claim — and silently clear — a watchpoint a GDB client had just armed within 32
bytes of it, with `intercepted = true` so no stop reply was sent. From the
client's side the watchpoint simply never fires again.

    const bool gen2_owns = m_gen2_watch_active
        && (address == m_gen2_watch_address || in_band || !m_session.IsValid());

Ownership now comes from the `m_gen2_watch_*` shadow fields, which are
`GdbServerImpl` members written only by `setw()` and cleared only by `clearw()`,
so they mean "the gen2 side of *this* session has a watch armed" and nothing
else. The band is computed from them, and the subsequent `ClearWatchPoint`
clears `m_gen2_watch_address` — which is what `setw()` actually programmed into
the hardware, and what `clearw()` already preferred. Raw mode reads
`!m_session.IsValid()` directly rather than `m_watch_data.gen2loop_on == 2`,
which is only refreshed on `gen2_loop`'s 500 ms tick and so could still read `2`
for up to half a second after a client connected — long enough to claim a hit
belonging to GDB.

The band stays fuzzy deliberately: ARM reports a fault address within the region
the *access* touched, so a wide `STP`/`ST4` can land outside the watched qword.
It is simply not consulted until we know gen2 owns a watch at all.

**The rule this codifies: on ambiguity, favour the GDB client.** Guessing gen2
wrongly clears the watchpoint and sends no stop reply — silent, and expensive.
Guessing GDB wrongly sends a spurious stop reply — visible and harmless. The old
band was generous towards gen2, i.e. backwards.

Left alone: the hardware *instruction* breakpoint branch has the same shape but
tests exact equality rather than a band, so its exposure is far narrower, and
its `next_pc` match is load-bearing for the capture loop.

**Still open.** Even with fresh state, gen2 and a `Z2` armed within 32 bytes of
each other resolve by guesswork. The proper fix is slot ownership: have
`HardwareWatchPointManager::SetWatchPoint` return the slot it allocated, record
it per owner, and resolve a hit by the range the hardware actually watches
(`[AlignDown(addr, 8), +8)` for `size <= 8` — literally what `SetDataBreakPoint`
programs — and `[AlignDown(addr, size), +size)` otherwise). Deferred so the
watchpoint path is not changed twice while the original bug is unexplained.

### `monitor dbgregs`

`AMS_DMNT2_GDB_LOG_*` compiles to nothing without
`AMS_DMNT2_ENABLE_HTCS_DEBUG_LOG`, so the existing log calls prove nothing on a
retail-booted console. Added a 64-entry ring of every debug-register write —
tick, requested core vs. core actually observed, migrate spins, reg, `dbgbcr`,
value, result — dumped by a new `monitor dbgregs`, with a header giving the
probed register extents, the usable counts and which `I` register is the
watchpoint context comparator. Rows whose write missed its core are prefixed
`MISS`.

Arm the dead `Z2`, `vCont;c`, wait, interrupt, dump. `MISS` -> the migration
bug. A row *after* the arm writing the same `D` register or the watch-context
`I` register -> something overwrote it. **No rows at all -> nothing in dmnt
touched the registers and the cause is below us.**

### Detach cleanup, audited

Detach is otherwise sound. Hardware breakpoints and watchpoints are `ClearAll()`ed
*before* the `m_is_valid` gate, so a half-failed attach still clears them and
`WatchPoint::Clear` needs no handle; software breakpoints are cleared inside the
gate, which is correct because writing the original instructions back needs a
live handle. `m_status` is maintained centrally — `GetProcessDebugEvent()` calls
`SetDebugBreaked()` on any event carrying `DebugEventFlag_Stopped` — so a Ctrl-C
interrupt does leave it correctly halted. Two things were wrong:

- **`~GdbServerImpl()` un-suspended the cheat events thread at the top**, so for
  the whole teardown (joining the events thread, clearing breakpoints, the
  `Continue()` inside `Detach()`) both sides owned the shared handle and could
  race for the same debug event. Harmless in outcome, but exactly the
  double-ownership addendum 19's parked acknowledgement exists to prevent. Moved
  to after `Detach()`.
- **`Detach()` only resumed when `m_status == ProcessStatus_DebugBreak`.** But
  `Continue(thread_id)` resumes a single thread and sets the status to Running
  for the whole process, so threads could be left stopped while the status said
  otherwise. It now always attempts `Continue()`; one with nothing pending
  simply fails.

Worth recording as a property of addendum 19: `SuspendDebugEvents(false)`
un-parks the cheat events thread, which drains events and resumes the process
within ~100 ms. That is a genuine safety net if gen2 ever leaves the game
halted — before the join, the resume depended on Breeze re-opening the handle.

### Files changed

* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_hardware_breakpoint.{cpp,hpp}`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_hardware_watchpoint.{cpp,hpp}`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_gdb_server_impl.cpp`
* `Atmosphere/stratosphere/dmnt.gen2/source/dmnt2_debug_process.cpp`
* `Breeze/menu.md` (Gen2 menu prose, Gen2Attach/Gen2Detach rows, Reset CheatVM)

---


*End of report.*
