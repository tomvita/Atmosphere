# Breeze Sysmodule Design — Stock Home Menu (qlaunch) Compatible

## Table of Contents

1. [Introduction](#introduction)
2. [Goals and Constraints](#goals-and-constraints)
3. [Architecture Overview](#architecture-overview)
4. [MITM Layer Design](#mitm-layer-design)
   - [Service Target: appletAE](#service-target-appletae)
   - [ShouldMitm Filter](#shouldmitm-filter)
   - [Interface Wrapping Hierarchy](#interface-wrapping-hierarchy)
5. [Home Button Interception](#home-button-interception)
   - [Dual Channel Problem](#dual-channel-problem)
   - [IHomeMenuFunctions::PopFromGeneralChannel (SAMS)](#ihomemenufunctionspopfromgeneralchannel-sams)
   - [ICommonStateGetter::ReceiveMessage (AE)](#icommonstategetterreceivemessage-ae)
   - [Suppression Strategy](#suppression-strategy)
6. [Album (Breeze) Lifecycle Management](#album-breeze-lifecycle-management)
   - [Creating the Library Applet](#creating-the-library-applet)
   - [ILibraryAppletAccessor Lifecycle](#ilibraryappletaccessor-lifecycle)
   - [Persistent Session (Foreground Toggling)](#persistent-session-foreground-toggling)
7. [Foreground Management](#foreground-management)
   - [RequestToGetForeground](#requesttogetforeground)
   - [Game Suspend/Resume](#game-suspendresume)
   - [Toggling Flow](#toggling-flow)
8. [Flag File Protocol](#flag-file-protocol)
9. [State Machine](#state-machine)
10. [qlaunch Compatibility](#qlaunch-compatibility)
    - [Preventing qlaunch Interference](#preventing-qlaunch-interference)
    - [Transparent Passthrough](#transparent-passthrough)
11. [Build System and Integration](#build-system-and-integration)
    - [Option A: Add Module to ams_mitm](#option-a-add-module-to-ams_mitm)
    - [Option B: Standalone Sysmodule](#option-b-standalone-sysmodule)
12. [File Layout](#file-layout)
13. [Open Questions and Risks](#open-questions-and-risks)
14. [IPC Command Reference](#ipc-command-reference)

---

## Introduction

This document describes the architecture for a standalone sysmodule that enables Home button toggling between a running game and **Breeze** (a cheat/memory editor by Tomvita) while using the **stock Nintendo Switch home menu (qlaunch)** — no custom home menu replacement required.

The SwitchU integration (documented in `breeze_mod.md`) achieves this by replacing qlaunch entirely with a custom daemon. This sysmodule achieves the same result by sitting between qlaunch and the AM (Applet Manager) service using Atmosphere's MITM framework, transparently intercepting and redirecting Home button events when Breeze mode is active.

### Why a Sysmodule?

- **No qlaunch replacement needed** — works with the stock Nintendo home menu
- **Broader compatibility** — does not require SwitchU to be installed
- **Lower risk** — if the MITM fails or is uninstalled, qlaunch continues to work normally
- **Composable** — can coexist with other system modifications

---

## Goals and Constraints

### Goals

1. When `breeze_active` flag exists and Home is pressed from a game: launch or resume Breeze (Album/hbmenu) instead of showing the stock home menu
2. When Home is pressed from Breeze: resume the game
3. When Breeze exits via "Goto Game": resume the game, keep Breeze mode armed for next Home press
4. When Breeze exits via "Exit": deactivate Breeze mode, revert to stock Home behavior
5. Transparent to qlaunch when Breeze mode is inactive (zero behavioral change)

### Constraints

- Only one library applet can be alive at a time (OS constraint)
- Breeze runs as an NRO in the Album applet slot (`AppletId_LibraryAppletPhotoViewer`, title 010000000000100D)
- Breeze communicates via flag files (no IPC access from NRO context)
- The sysmodule has no direct AM applet access — must operate through qlaunch's MITM'd session
- qlaunch must not interfere when Breeze is active (suppress its Home handling)
- Must not break qlaunch's normal operation when Breeze mode is inactive

---

## Architecture Overview

```
                    ┌──────────────┐
                    │   HID (hid)  │
                    │  Home Button │
                    └──────┬───────┘
                           │ event
                    ┌──────▼───────┐
                    │  AM sysmodule │
                    │  (am service) │
                    └──────┬───────┘
                           │ SAMS msg + AppletMessage
               ┌───────────▼───────────┐
               │  Breeze MITM Module   │  ◄── NEW: intercepts appletAE
               │  (in ams_mitm or      │
               │   standalone)         │
               └───┬───────────┬───────┘
                   │           │
          ┌────────▼──┐   ┌───▼────────┐
          │  qlaunch  │   │   Album    │
          │  (stock   │   │  (Breeze)  │
          │  home     │   │  via MITM  │
          │  menu)    │   │  forwarded │
          └───────────┘   │  service   │
                          └────────────┘
```

The module MITMs `appletAE` and wraps qlaunch's session interfaces. When Breeze mode is active, it:
1. Suppresses Home button messages to qlaunch (both SAMS and AppletMessage channels)
2. Calls `RequestToGetForeground` on the real IHomeMenuFunctions (bringing qlaunch to foreground, which AM requires)
3. Creates/resumes Album via the real ILibraryAppletCreator (which AM sees as qlaunch creating it)
4. Manages Album lifecycle and foreground transitions

When Breeze mode is inactive, ALL IPC passes through unchanged — qlaunch behaves exactly as stock.

---

## MITM Layer Design

### Service Target: appletAE

We MITM `appletAE`, the service used by SystemApplets, LibraryApplets, and OverlayApplets. This is the only path to qlaunch's `ISystemAppletProxy`.

**Why not `appletOE`?** That service is for applications (games). We don't need to intercept the game's AM calls — we only need to control qlaunch's.

**Why not MITM at a lower level (hid)?** The Home button event flows: HID → AM (internal) → SAMS/AppletMessage → qlaunch. There is no HID MITM in Atmosphere, and intercepting at the HID level would require reimplementing AM's button processing logic. Intercepting at the `appletAE` level is surgically precise.

### ShouldMitm Filter

```cpp
static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
    // Only MITM qlaunch (SystemAppletMenu, title ID 0x0100000000001000)
    return client_info.program_id == ncm::SystemProgramId::Qlaunch;
}
```

All other processes (library applets, overlay, system applications) get unmodified `appletAE` access.

### Interface Wrapping Hierarchy

Following the `ns_web` MITM pattern (`stratosphere/ams_mitm/source/ns_mitm/`):

```
AmMitmService : MitmServiceImplBase
├── Intercepts cmd 100: OpenSystemAppletProxy
│   Forward to real AM → get real ISystemAppletProxy → wrap in:
│
└── SystemAppletProxyService : IServiceObject
    ├── Intercepts cmd 0: GetCommonStateGetter
    │   Forward → get real ICommonStateGetter → wrap in:
    │   └── CommonStateGetterService
    │       ├── Intercepts cmd 1: ReceiveMessage (filter AppletMessage 20)
    │       └── All other cmds → forward to real service
    │
    ├── Intercepts cmd 20: GetHomeMenuFunctions
    │   Forward → get real IHomeMenuFunctions → wrap in:
    │   └── HomeMenuFunctionsService
    │       ├── Intercepts cmd 20: PopFromGeneralChannel (filter SAMS msg=2)
    │       └── All other cmds → forward to real service
    │
    ├── Intercepts cmd 11: GetLibraryAppletCreator
    │   Forward → get real ILibraryAppletCreator → wrap in:
    │   └── LibraryAppletCreatorService
    │       ├── Intercepts cmd 0: CreateLibraryApplet (block if we own Album)
    │       └── All other cmds → forward to real service
    │
    └── All other cmds (1,2,3,4,10,21,22,23,1000) → forward to real proxy
```

Each wrapper holds a `Service` object pointing to the **real** sub-interface (domain sub-object on qlaunch's forwarded session). The wrapper can:
- **Forward** the call to the real service (passthrough)
- **Suppress** the call by returning success without forwarding
- **Initiate** its own calls on the real service (e.g., create Album)

---

## Home Button Interception

### Dual Channel Problem

When the user presses Home, AM delivers the event through **two independent channels** to qlaunch:

| Channel | Interface | Command | Data |
|---------|-----------|---------|------|
| **SAMS** (General Channel) | IHomeMenuFunctions | cmd 20 `PopFromGeneralChannel` | IStorage with SAMS header: `{magic=0x534D4153, ver=1, msg=2, reserved=1}` |
| **AE** (AppletMessage) | ICommonStateGetter | cmd 1 `ReceiveMessage` | `AppletMessage` value 20 (`DetectShortPressingHomeButton`) |

Both channels must be intercepted to fully control Home behavior. If only one is suppressed, qlaunch will still process the Home event through the other.

### IHomeMenuFunctions::PopFromGeneralChannel (SAMS)

When qlaunch calls `PopFromGeneralChannel`:

1. Forward to real AM service to get the IStorage
2. Read the SAMS header from the storage (first 16 bytes)
3. If `msg == 2` (Home) AND `breeze_active` flag exists AND a game is running:
   - **Consume** the message (do not return it to qlaunch)
   - Trigger Breeze logic (launch/resume Album, manage foreground)
   - Return error `0x680` (queue empty / no data available) to qlaunch, OR re-signal the event later when we have a non-Home message to deliver
4. If `msg != 2` OR Breeze mode inactive:
   - **Forward** the IStorage to qlaunch unchanged

### ICommonStateGetter::ReceiveMessage (AE)

When qlaunch calls `ReceiveMessage`:

1. Forward to real AM service to get the AppletMessage value
2. If message == 20 (`DetectShortPressingHomeButton`) AND Breeze mode active:
   - **Suppress** by returning the queue-empty error, or substitute with a harmless message
3. Otherwise:
   - Return the message unchanged

**Important:** Messages like `FocusStateChanged` (15), `OperationModeChanged` (30), `ExitRequest` (4) must ALWAYS pass through to qlaunch — they are critical for system stability.

### Suppression Strategy

The cleanest approach is a **message queue buffer**:

1. When the GeneralChannel event signals, our MITM proactively pops ALL pending SAMS messages from the real service
2. Filter out Home messages (msg=2) when Breeze mode is active
3. Buffer the remaining messages
4. When qlaunch calls `PopFromGeneralChannel`, serve from our buffer
5. If buffer empty, return the no-data error

This avoids race conditions where qlaunch might call `PopFromGeneralChannel` before we can intercept.

Similarly for `ReceiveMessage`: proactively drain the AppletMessage queue, filter, and re-serve.

---

## Album (Breeze) Lifecycle Management

### Creating the Library Applet

When Home is intercepted and Breeze mode is active for the first time (no Album running):

```
MITM module calls on the REAL ILibraryAppletCreator (qlaunch's session):
1. CreateLibraryApplet(AppletId_LibraryAppletPhotoViewer, LibAppletMode_AllForeground)
   → Returns ILibraryAppletAccessor as domain sub-object
   → AM sees this as qlaunch creating Album (PID identity from session)
2. CreateStorage(sizeof(LibAppletArgs))  // cmd 10
   → Returns IStorage
3. Write LibAppletArgs to storage
4. PushInData(storage)  // cmd 100 on accessor
5. Start()  // cmd 10 on accessor
   → AM launches the Album process (which loads hbmenu → Breeze via Atmosphere override)
```

The `LibAppletArgs` struct (0x20 bytes):
```c
struct LibAppletArgs {
    u32 CommonArgs_version;  // = 1
    u32 CommonArgs_size;     // = 0x20
    u32 LaVersion;           // = 0
    u32 ExpectedThemeColor;  // = 0 or from appletGetThemeColorType()
    u8  PlayStartupSound;    // = 0
    u8  pad[7];
    u64 tick;                // = armGetSystemTick()
};
```

### ILibraryAppletAccessor Lifecycle

The MITM module holds the `ILibraryAppletAccessor` `Service` object and monitors it:

| Operation | Command | When |
|-----------|---------|------|
| `GetAppletStateChangedEvent` | cmd 0 | At creation — get event handle for monitoring exit |
| `Start` | cmd 10 | After pushing input data |
| `RequestExit` | cmd 20 | When we want Breeze to gracefully exit |
| `Terminate` | cmd 25 | Force kill (if needed) |
| `GetResult` | cmd 30 | After exit — check exit reason |
| `PushInteractiveInData` | cmd 103 | (Not needed for Album) |
| `PopInteractiveOutData` | cmd 104 | (Not needed for Album) |

The module runs a monitoring thread that waits on the `StateChangedEvent`. When Album exits:
1. Check `breeze_goto_game` flag → resume game
2. Check `breeze_active` flag → resume game (Breeze was toggled away by Home)
3. Neither flag → Breeze exited normally, revert to stock Home behavior

### Persistent Session (Foreground Toggling)

Like the SwitchU integration, we keep the Album holder alive across Home presses to enable instant toggling:

- **First Home from game (Album not running):** Create Album via `CreateLibraryApplet`, start it, set `g_albumActive = true`
- **Subsequent Home from game (Album already running):** Don't create new Album — just resume foreground to the existing Album
- **Home from Album (back to game):** Resume the game's foreground. Album stays alive in background.
- **Album exits (user pressed Exit/GotoGame in Breeze):** Clean up accessor, set `g_albumActive = false`, process flags.

**Key question:** Can we keep Album alive while the game has foreground? In the SwitchU integration this works because the daemon directly manages foreground. With MITM, we need to verify that AM allows a library applet to remain alive while the application has foreground. This should work because `LibAppletMode_AllForeground` applets can be moved between foreground/background by the system applet.

---

## Foreground Management

### RequestToGetForeground

`IHomeMenuFunctions::RequestToGetForeground` (cmd 10) tells AM that qlaunch wants the foreground. This is a prerequisite for any foreground transition — AM requires the system applet to mediate all foreground changes.

In our MITM:
- We call `RequestToGetForeground` on the REAL IHomeMenuFunctions when we want to initiate a transition
- We can suppress qlaunch's own calls when we're managing the transition ourselves

### Game Suspend/Resume

When transitioning game → Breeze:
1. AM automatically sends `AppletMessage_FocusStateChanged` to the game (changing focus to Background)
2. The game suspends (if using default FocusHandlingMode)
3. Our MITM calls `RequestToGetForeground` → AM gives qlaunch foreground
4. Album (already started or now starting) becomes visible

When transitioning Breeze → game:
1. We need the game to get foreground back
2. On `IApplicationAccessor` (which qlaunch holds): `RequestForApplicationToGetForeground` (cmd 101)
3. AM sends `AppletMessage_FocusStateChanged` back to the game with `FocusState_InFocus`
4. Game resumes

**Challenge:** We need access to qlaunch's `IApplicationAccessor` to call `RequestForApplicationToGetForeground`. This requires also intercepting `IApplicationCreator` (cmd 22 on ISystemAppletProxy) and wrapping the `IApplicationAccessor` returned by `CreateApplication`/`PopLaunchRequestedApplication`.

Alternative: Use `ILibraryAppletAccessor::SetOutOfFocusApplicationSuspendingEnabled` (cmd 50) or other mechanisms. This needs further investigation.

### Toggling Flow

```
State: Game has foreground, Album alive in background
User presses Home:
  1. AM sends SAMS msg=2 + AppletMessage 20 to qlaunch
  2. MITM intercepts both, suppresses to qlaunch
  3. MITM calls RequestToGetForeground on real IHomeMenuFunctions
  4. AM transitions foreground: game → qlaunch (which means Album becomes visible)
  5. Game gets FocusStateChanged → Background, suspends

State: Album (Breeze) has foreground, game suspended
User presses Home:
  1. AM sends SAMS msg=2 + AppletMessage 20
  2. MITM intercepts, suppresses to qlaunch
  3. MITM calls RequestForApplicationToGetForeground on the real IApplicationAccessor
  4. AM transitions foreground: qlaunch → game
  5. Game gets FocusStateChanged → InFocus, resumes
  6. Album remains alive but not visible

State: Game has foreground, Album not yet created
User presses Home (first time):
  1. MITM intercepts Home messages
  2. MITM calls CreateLibraryApplet on real ILibraryAppletCreator → Album
  3. MITM calls Start on the accessor
  4. MITM calls RequestToGetForeground
  5. Album launches and becomes visible
```

---

## Flag File Protocol

Same as the SwitchU integration — Breeze communicates via SD card flag files:

| Flag File | Created By | Meaning |
|-----------|------------|---------|
| `sdmc:/config/breeze/breeze_active` | Breeze ("Goto Game" button) | Breeze mode is armed — Home should toggle to Breeze |
| `sdmc:/config/breeze/breeze_goto_game` | Breeze ("Goto Game" button) | After Breeze exits, resume the game |

**Note:** Path changed from `sdmc:/config/SwitchU/` to `sdmc:/config/breeze/` since this sysmodule is independent of SwitchU.

### Flag Lifecycle

1. **Activation:** User launches Album from qlaunch normally → Breeze loads → user presses "Goto Game" → both flags created → Breeze exits → game resumes. `breeze_active` persists; `breeze_goto_game` is consumed by the sysmodule.
2. **Toggling:** Home press → sysmodule checks `breeze_active` → redirects to Breeze. Home from Breeze → sysmodule resumes game.
3. **Deactivation:** In Breeze, press "Exit" → removes `breeze_active` → exits. Next Home press goes to stock qlaunch menu.
4. **Reboot:** Sysmodule cleans up both flags on initialization.

---

## State Machine

```
                         ┌─────────────────┐
                         │   PASSTHROUGH    │ ◄── Default state
                         │  (Breeze mode   │     All IPC forwarded
                         │   inactive)     │     qlaunch works normally
                         └────────┬────────┘
                                  │ breeze_active flag detected
                                  │ on Home press
                                  ▼
                    ┌──────────────────────────┐
                    │   BREEZE_LAUNCHING       │
                    │  Creating Album via      │
                    │  ILibraryAppletCreator   │
                    └────────────┬─────────────┘
                                 │ Album started
                                 ▼
              ┌───────────────────────────────────┐
         Home │   BREEZE_FOREGROUND               │ Home
         from │  Album (Breeze) has foreground    │◄──── from
         Breeze│  Game suspended                   │      game
              │                                   │
              ▼                                   │
     ┌──────────────────────┐                     │
     │  GAME_FOREGROUND     │─────────────────────┘
     │  Game has foreground  │
     │  Album alive in bg    │
     └──────────┬───────────┘
                │ Album exits (Breeze Exit/GotoGame)
                ▼
     ┌──────────────────────┐
     │  CLEANUP             │
     │  Process flags       │
     │  Reset state         │
     └──────────┬───────────┘
                │
                ▼
     breeze_active still set? ──Yes──► PASSTHROUGH (armed, next Home redirects)
                │
                No
                ▼
          PASSTHROUGH (fully inactive)
```

States:
- **PASSTHROUGH**: No interception. All IPC forwarded to/from qlaunch.
- **BREEZE_LAUNCHING**: Creating Album. Suppress qlaunch Home handling.
- **BREEZE_FOREGROUND**: Album visible, game suspended. Suppress qlaunch Home handling.
- **GAME_FOREGROUND**: Game visible, Album alive in background. Suppress qlaunch Home handling.
- **CLEANUP**: Album exited. Process flags, transition back.

---

## qlaunch Compatibility

### Preventing qlaunch Interference

When Breeze mode is active, qlaunch must not process the Home event. We achieve this by:

1. **Suppressing SAMS msg=2** in `PopFromGeneralChannel` — qlaunch never sees the "Home pressed" message
2. **Suppressing AppletMessage 20** in `ReceiveMessage` — qlaunch never sees `DetectShortPressingHomeButton`
3. **Blocking qlaunch's `RequestToGetForeground`** — if qlaunch calls this for any reason while we're managing Breeze, we can suppress it (return success without forwarding). However, this should be unnecessary if both message channels are properly suppressed.

### What About qlaunch's Own Library Applets?

qlaunch can create library applets (e.g., when user opens Album normally, or system dialogs). We need to handle conflicts:

- **If Breeze mode is inactive:** All `CreateLibraryApplet` calls pass through unchanged.
- **If Breeze mode is active AND Album (Breeze) is running:** We must block qlaunch's `CreateLibraryApplet` calls because only one library applet can exist. Return an appropriate error.
- **If Breeze mode is active but Album has exited:** Allow qlaunch's library applet creation (Breeze mode will reactivate on next Home press).

### Transparent Passthrough

The MITM module MUST be invisible when Breeze mode is inactive:
- `ShouldMitm` returns true only for qlaunch — no other process is affected
- All commands not listed in the interface definition auto-forward via Atmosphere's domain forwarding
- Explicitly intercepted commands check Breeze state and forward if inactive
- No additional latency for forwarded commands (single `svc::SendSyncRequest`)

---

## Build System and Integration

### Option A: Add Module to ams_mitm (Recommended)

Add a new module directory alongside existing MITM modules in Atmosphere:

```
stratosphere/ams_mitm/source/
├── bpc_mitm/
├── fs_mitm/
├── ns_mitm/
├── breeze_mitm/          ◄── NEW
│   ├── breeze_mitm_service.hpp
│   ├── breeze_mitm_service.cpp
│   ├── breeze_am_shim.h
│   ├── breeze_am_shim.c
│   ├── breeze_proxy_service.hpp
│   ├── breeze_proxy_service.cpp
│   ├── breeze_home_menu_service.hpp
│   ├── breeze_home_menu_service.cpp
│   ├── breeze_common_state_service.hpp
│   ├── breeze_common_state_service.cpp
│   ├── breeze_la_creator_service.hpp
│   ├── breeze_la_creator_service.cpp
│   ├── breeze_state.hpp
│   ├── breeze_state.cpp
│   └── breezemitm_module.hpp
└── amsmitm_module_management.cpp  ◄── Register breeze module here
```

**Advantages:**
- Leverages existing `ams_mitm` infrastructure (ServerManager, module lifecycle, SD card access)
- Single sysmodule binary — no additional title ID needed
- Built with the Atmosphere build system (already configured for libstratosphere)

**Disadvantages:**
- Requires maintaining a fork of Atmosphere (or patch system)
- Tied to Atmosphere version updates

### Option B: Standalone Sysmodule

Build as a separate `.nsp` with its own title ID, linking against libstratosphere.

**Advantages:**
- Independent distribution (no Atmosphere fork needed)
- Own title ID, can be installed/removed independently

**Disadvantages:**
- More complex build setup (linking libstratosphere externally)
- Need to handle module lifecycle, ServerManager setup from scratch
- Additional process overhead

### Recommendation

**Option A** for development. Once proven, evaluate migrating to Option B for distribution.

---

## File Layout

### Source Files

| File | Purpose |
|------|---------|
| `breeze_mitm_service.hpp/cpp` | Top-level MITM: intercepts `appletAE`, wraps `OpenSystemAppletProxy` |
| `breeze_am_shim.h/c` | C shim functions for forwarding IPC calls to real AM service |
| `breeze_proxy_service.hpp/cpp` | Wraps `ISystemAppletProxy`, intercepts sub-interface getters |
| `breeze_home_menu_service.hpp/cpp` | Wraps `IHomeMenuFunctions`, intercepts `PopFromGeneralChannel` |
| `breeze_common_state_service.hpp/cpp` | Wraps `ICommonStateGetter`, intercepts `ReceiveMessage` |
| `breeze_la_creator_service.hpp/cpp` | Wraps `ILibraryAppletCreator`, manages Album creation |
| `breeze_state.hpp/cpp` | Global state: flags, Album holder, state machine |
| `breezemitm_module.hpp` | Module definition, thread entry |

### Key Shim Functions (breeze_am_shim.c)

```c
// Forward OpenSystemAppletProxy to real appletAE
Result amShimOpenSystemAppletProxy(Service* fwd, Service* out, u64 reserved);

// Forward GetHomeMenuFunctions to real ISystemAppletProxy
Result amShimGetHomeMenuFunctions(Service* proxy, Service* out);

// Forward GetCommonStateGetter to real ISystemAppletProxy
Result amShimGetCommonStateGetter(Service* proxy, Service* out);

// Forward GetLibraryAppletCreator to real ISystemAppletProxy
Result amShimGetLibraryAppletCreator(Service* proxy, Service* out);

// Forward PopFromGeneralChannel to real IHomeMenuFunctions
Result amShimPopFromGeneralChannel(Service* hmf, Service* out_storage);

// Forward ReceiveMessage to real ICommonStateGetter
Result amShimReceiveMessage(Service* csg, u32* out_msg);

// Forward RequestToGetForeground to real IHomeMenuFunctions
Result amShimRequestToGetForeground(Service* hmf);

// Forward CreateLibraryApplet to real ILibraryAppletCreator
Result amShimCreateLibraryApplet(Service* lac, Service* out_accessor, u32 applet_id, u32 mode);

// Forward Start on ILibraryAppletAccessor
Result amShimAccessorStart(Service* accessor);

// Forward Terminate on ILibraryAppletAccessor
Result amShimAccessorTerminate(Service* accessor);

// Forward GetAppletStateChangedEvent on ILibraryAppletAccessor
Result amShimAccessorGetStateChangedEvent(Service* accessor, Event* out);
```

---

## Open Questions and Risks

### Q1: Can Album Stay Alive While Game Has Foreground?

**Risk: Medium.** In SwitchU this works because the daemon directly controls foreground. With stock qlaunch + MITM, AM may enforce that library applets are terminated when the application gets foreground. If AM auto-terminates Album when the game resumes, we lose the persistent session optimization and must recreate Album each Home press.

**Mitigation:** Test empirically. If AM kills Album on game resume, fall back to recreation on each toggle (slower but functional).

### Q2: Does AM Send Home Events Through Both Channels Simultaneously?

**Risk: Low.** Both SAMS msg=2 and AppletMessage 20 are observed in SwitchU. But the exact timing and whether both are always sent needs verification. If only one channel is used in certain firmware versions, our dual-channel suppression is still safe (just a no-op on the unused channel).

### Q3: qlaunch's Internal State After Suppressed Home

**Risk: Medium.** qlaunch may have internal state tracking (e.g., "Home was pressed, waiting for foreground"). If we suppress both message channels but AM has already changed some internal state (focus state, etc.), qlaunch might enter an unexpected state when it next processes events.

**Mitigation:** Monitor qlaunch behavior. We may need to feed qlaunch a "resume normal" message after Breeze exits.

### Q4: Race Conditions on PopFromGeneralChannel

**Risk: Medium.** If qlaunch calls `PopFromGeneralChannel` before our MITM can process the event, qlaunch might see the Home message. The proactive draining strategy (pop messages on event signal, buffer them) mitigates this, but introduces complexity.

**Mitigation:** The MITM intercepts the call itself — qlaunch cannot call `PopFromGeneralChannel` without going through our wrapper. So there is no race: when qlaunch calls `PopFromGeneralChannel`, our wrapper executes, which calls the real service, reads the message, and decides whether to forward or suppress. The MITM is synchronous with qlaunch's call.

**Update on Q4:** After further analysis, this is actually **low risk**. Since the MITM wraps qlaunch's `PopFromGeneralChannel`, every call from qlaunch goes through us. We read the real message, decide, and return. There is no race because we ARE the intermediary.

### Q5: IApplicationAccessor Access for Game Resume

**Risk: High.** To resume the game from Breeze, we need to call `RequestForApplicationToGetForeground` on qlaunch's `IApplicationAccessor`. This requires intercepting `IApplicationCreator` (cmd 22 on the proxy) and tracking the `IApplicationAccessor` returned by `CreateApplication` or `PopLaunchRequestedApplication`.

**Alternative:** Instead of directly resuming the game, we could:
- Let Breeze exit (Album terminates)
- qlaunch's normal Album-exit handling resumes the game
- But this means Breeze can't stay alive for instant toggling

**Another alternative:** Calling `RequestToGetForeground` on IHomeMenuFunctions brings qlaunch to foreground. Then, to give game foreground: does AM have a mechanism where the system applet can transfer foreground to the application without IApplicationAccessor? This needs investigation.

### Q6: LaunchableEvent Requirement

**Risk: Low.** Before creating a library applet, libnx waits on `ISelfController::GetLibraryAppletLaunchableEvent` (cmd 9). Our MITM doesn't have direct access to qlaunch's ISelfController. However, since qlaunch is already running and AM has already signaled this event, it should be in the signaled state. We can skip this wait or also intercept ISelfController.

### Q7: Firmware Compatibility

**Risk: Medium.** IPC command IDs and behaviors may vary across firmware versions. The design is based on current-gen (14.x-18.x) interfaces. Older firmware may not have some commands (e.g., `PerformSystemButtonPressingIfInFocus` is 6.0.0+).

**Mitigation:** Target a minimum firmware version (e.g., 14.0.0+) and document compatibility.

---

## IPC Command Reference

### Commands We Intercept

| Interface | Cmd | Name | Our Action |
|-----------|-----|------|------------|
| IAllSystemAppletProxiesService | 100 | OpenSystemAppletProxy | Wrap returned proxy |
| ISystemAppletProxy | 0 | GetCommonStateGetter | Wrap returned interface |
| ISystemAppletProxy | 11 | GetLibraryAppletCreator | Wrap returned interface |
| ISystemAppletProxy | 20 | GetHomeMenuFunctions | Wrap returned interface |
| ICommonStateGetter | 1 | ReceiveMessage | Filter AppletMessage 20 when Breeze active |
| IHomeMenuFunctions | 10 | RequestToGetForeground | Suppress when MITM managing transition |
| IHomeMenuFunctions | 20 | PopFromGeneralChannel | Filter SAMS msg=2 when Breeze active |
| ILibraryAppletCreator | 0 | CreateLibraryApplet | Block when MITM owns Album |

### Commands We Call on Real Service

| Interface | Cmd | Name | When |
|-----------|-----|------|------|
| IHomeMenuFunctions | 10 | RequestToGetForeground | Initiating foreground transition |
| ILibraryAppletCreator | 0 | CreateLibraryApplet | Creating Album for Breeze |
| ILibraryAppletCreator | 10 | CreateStorage | Preparing Album input data |
| ILibraryAppletAccessor | 0 | GetAppletStateChangedEvent | Monitoring Album lifecycle |
| ILibraryAppletAccessor | 10 | Start | Launching Album |
| ILibraryAppletAccessor | 25 | Terminate | Force-killing Album |
| ILibraryAppletAccessor | 100 | PushInData | Sending LibAppletArgs |

### Commands We Always Forward

Everything else — ISelfController, IAudioController, IDisplayController, IWindowController, IProcessWindingController, IGlobalStateController, IApplicationCreator (initially), IDebugFunctions, and all sub-commands not listed above.
