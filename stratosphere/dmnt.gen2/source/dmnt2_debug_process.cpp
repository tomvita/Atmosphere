/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include "dmnt2_debug_log.hpp"
#include "dmnt2_debug_process.hpp"
#include "dmnt2_shared_debug_handle.hpp"
#include "cheat/impl/dmnt_cheat_api.hpp"

namespace ams::dmnt {

    namespace {

        s32 SignExtend(u32 value, u32 bits) {
            return static_cast<s32>(value << (32 - bits)) >> (32 - bits);
        }

    }

    Result DebugProcess::Attach(os::ProcessId process_id, bool start_process) {
        /* Can we join the cheat engine's debug handle instead of taking the
         * process away from it?
         *
         * dmnt and dmnt.gen2 are one sysmodule now and share a single debug
         * handle (dmnt2_shared_debug_handle.cpp), so the "only one debugger per
         * process" restriction that forced a detach no longer applies. When the
         * handle is already open for the process we want, we join it: the cheat
         * VM, its cheat list, its toggles and its frozen addresses all survive a
         * gen2 attach untouched, and there is no svcDebugActiveProcess to come
         * back ResultBusy while the kernel finishes an asynchronous detach
         * (which is what "attach, fail, attach again" was).
         *
         * Not joinable when we have to start the process ourselves (there are
         * real initial debug events to consume in that case), or when the
         * handle is open for some other process - the shared handle is a single
         * slot, so the cheat engine genuinely has to let go. */
        const bool can_join = !start_process
                           && dmnt::dbg::GetSharedDebugHandle() != os::InvalidNativeHandle
                           && dmnt::dbg::GetSharedProcessId() == process_id;

#if !defined(DMNT_GEN2_NO_CHEATVM)
        if (can_join) {
            /* Take over debug-event handling, but leave the cheat process
             * attached. Wait for the events thread to actually park: setting
             * the flag alone leaves a window in which it can still wake on an
             * event and continue it out from under us. */
            ams::dmnt::cheat::impl::SuspendDebugEvents(true);
            if (!ams::dmnt::cheat::impl::WaitDebugEventsParked(ams::TimeSpan::FromMilliSeconds(200))) {
                AMS_DMNT2_GDB_LOG_WARN("DebugProcess::Attach: cheat debug-events thread did not park\n");
            }
        } else if (dmnt::dbg::GetSharedDebugHandle() != os::InvalidNativeHandle) {
            /* Cleanly detach the cheat process if it was attached. */
            ams::dmnt::cheat::impl::ForceCloseCheatProcess();
            /* Sleep briefly to allow the kernel asynchronously to detach. */
            os::SleepThread(ams::TimeSpan::FromMilliSeconds(100));
        }

        /* Every early return below this point used to leave the cheat process
         * closed and its debug-events thread suspended.
         *
         * That combination is worse than it looks: ForceCloseCheatProcess()
         * above already tore the cheat engine down, and the only re-open used
         * to be at the very end of this function, on the success path. A client
         * (Breeze, bookmark.ovl) that then called ForceOpenCheatProcess() to
         * recover would re-attach with svcDebugActiveProcess -- which suspends
         * the game -- and the cheat DebugEventsThread, still gated by
         * g_suspend_debug_events, would never call ContinueCheatProcess. The
         * game stays frozen until the console is slept and woken.
         *
         * Restore both on any failure, in that order: un-suspend first so the
         * events thread is live before the re-attach hands it a suspended
         * process. On the join path there is nothing to re-open, so only the
         * suspend is undone. */
        auto cheat_guard = SCOPE_GUARD {
            ams::dmnt::cheat::impl::SuspendDebugEvents(false);
            if (!can_join) {
                ams::dmnt::cheat::impl::ForceOpenCheatProcess();
            }
        };
#endif

        /* Attach Gen2. */
        R_TRY(dmnt::dbg::AttachGen2(process_id));
        m_debug_handle = dmnt::dbg::GetSharedDebugHandle();

#if !defined(DMNT_GEN2_NO_CHEATVM)
        ams::dmnt::cheat::impl::SuspendDebugEvents(true);
#endif

        /* If necessary, start the process. */
        if (start_process) {
            R_ABORT_UNLESS(pm::dmnt::StartProcess(process_id));
        }

        /* Collect initial information.
         *
         * Start() replays the initial debug events (CreateProcess, one
         * CreateThread per thread, DebuggerAttached) and leaves the process
         * halted. Those events were consumed by the cheat engine long ago on
         * the join path, so there StartShared() reconstructs the same state
         * from svcGetThreadList and leaves the process running. */
        if (can_join) {
            R_TRY(this->StartShared(process_id));
        } else {
            R_TRY(this->Start());
        }

        /* Get the attached modules. */
        R_TRY(this->CollectModules());

        /* Get our process id. */
        u64 pid_value = 0;
        svc::GetProcessId(std::addressof(pid_value), m_debug_handle);

        m_process_id = { pid_value };

        /* Get process info. */
        this->CollectProcessInfo();

#if !defined(DMNT_GEN2_NO_CHEATVM)
        if (!can_join) {
            /* Re-open the cheat process now that our own initial debug events
             * have been consumed. AttachDmnt() sees the shared handle already
             * open for this same process, so it joins it rather than re-opening
             * -- which would steal the handle back and invalidate our session.
             * Without this the cheat VM stays detached for the life of the GDB
             * session and does not come back afterwards, because
             * DetectLaunchThread only re-attaches on a new application launch.
             *
             * On the join path we never closed it, so there is nothing to do:
             * the cheat process, its VM state and its frozen addresses are
             * still exactly as the user left them. */
            ams::dmnt::cheat::impl::ForceOpenCheatProcess();
        }

        cheat_guard.Cancel();
#endif

        R_SUCCEED();
    }

    void DebugProcess::Detach() {
        m_hardware_breakpoints.ClearAll();
        m_hardware_watchpoints.ClearAll();

        if (m_is_valid) {
            m_software_breakpoints.ClearAll();

            /* Always try to resume, rather than only when m_status says we are
             * halted.
             *
             * m_status is not a reliable answer to "is the process stopped".
             * Continue(thread_id) resumes a single thread but sets the status
             * to Running for the whole process, so after a single-thread
             * continue the remaining threads can still be stopped while
             * m_status reads Running - and we would walk away leaving the game
             * frozen. A Continue with no pending debug event simply fails, so
             * attempting it unconditionally costs nothing.
             *
             * The cheat engine's events thread also resumes the process once
             * SuspendDebugEvents(false) below un-parks it, but only when it is
             * attached; this has to be right on its own for
             * DMNT_GEN2_NO_CHEATVM builds and for processes it does not hold. */
            this->Continue();

            dmnt::dbg::DetachGen2();
            m_debug_handle = svc::InvalidHandle;

#if !defined(DMNT_GEN2_NO_CHEATVM)
            ams::dmnt::cheat::impl::SuspendDebugEvents(false);
#endif
        }

        m_is_valid = false;
        m_thread_count = 0;
        std::memset(m_thread_valid, 0, sizeof(m_thread_valid));
        std::memset(m_thread_ids, 0, sizeof(m_thread_ids));
        std::memset(m_thread_infos, 0, sizeof(m_thread_infos));
    }

    Result DebugProcess::Start() {
        /* Process the initial debug events. */
        s32 num_threads = 0;
        bool attached = false;
        while (num_threads == 0 || !attached) {
            /* Wait for debug events to be available. */
            s32 dummy_index;
            R_ABORT_UNLESS(svc::WaitSynchronization(std::addressof(dummy_index), std::addressof(m_debug_handle), 1, svc::WaitInfinite));

            /* Get debug event. */
            svc::DebugEventInfo d;
            R_ABORT_UNLESS(svc::GetDebugEvent(std::addressof(d), m_debug_handle));

            /* Handle the debug event. */
            switch (d.type) {
                case svc::DebugEvent_CreateProcess:
                    {
                        /* Set our create process info. */
                        m_create_process_info = d.info.create_process;

                        /* Cache our bools. */
                        m_is_64_bit               = (m_create_process_info.flags & svc::CreateProcessFlag_Is64Bit);
                        m_is_64_bit_address_space = (m_create_process_info.flags & svc::CreateProcessFlag_AddressSpaceMask) == svc::CreateProcessFlag_AddressSpace64Bit;
                    }
                    break;
                case svc::DebugEvent_CreateThread:
                    {
                        ++num_threads;

                        if (const s32 index = this->ThreadCreate(d.thread_id); index >= 0) {
                            const Result result = osdbg::InitializeThreadInfo(std::addressof(m_thread_infos[index]), m_debug_handle, std::addressof(m_create_process_info), std::addressof(d.info.create_thread));

                            if (R_FAILED(result)) {
                                AMS_DMNT2_GDB_LOG_WARN("DebugProcess::Start: InitializeThreadInfo(%lx) failed: %08x\n", d.thread_id, result.GetValue());
                            }
                        }
                    }
                    break;
                case svc::DebugEvent_ExitThread:
                    {
                        --num_threads;
                        this->ThreadExit(d.thread_id);
                    }
                    break;
                case svc::DebugEvent_Exception:
                    {
                        if (d.info.exception.type == svc::DebugException_DebuggerAttached) {
                            attached = true;
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        /* Set ourselves as valid. */
        m_is_valid = true;
        this->SetDebugBreaked();

        R_SUCCEED();
    }

    Result DebugProcess::StartShared(os::ProcessId process_id) {
        m_process_id = process_id;

        /* Halt the process before we query anything about it, and consume the
         * resulting event ourselves, so that Attach() returns with the same
         * contract as Start(): valid, stopped, nothing in flight.
         *
         * This has to happen first, not last. We joined a handle whose initial
         * events the cheat engine drained long ago, so the process is running
         * when we get here -- and svc::GetDebugThreadContext fails on a running
         * thread. Building the thread table before breaking left every thread
         * with no TLS address and garbage thread info, which is what broke GDB
         * breakpoints on the join path (and only on the join path, i.e. only
         * once Breeze had launched and left the cheat engine attached).
         *
         * Two more reasons it cannot be left to the caller: vAttach's stop
         * reply is built from GetThreadContext, which is meaningless while the
         * process runs; and a Break() from that side lets the DebuggerBreak
         * event reach ProcessDebugEvents, which answers it with a second
         * asynchronous stop reply on top of vAttach's own. Consuming it here
         * keeps it off the wire.
         *
         * Safe because the cheat engine's events thread is parked by now
         * (Attach waited for it), so nothing else can take the event first.
         *
         * Callers that want the game running continue explicitly, exactly as
         * they already do on the Start() path -- gen2's ATTACH_CONT does. */
        if (R_SUCCEEDED(svc::BreakDebugProcess(m_debug_handle))) {
            const auto start  = os::GetSystemTick();
            const auto budget = TimeSpan::FromSeconds(1);
            while (os::ConvertToTimeSpan(os::GetSystemTick() - start) < budget) {
                s32 dummy_index;
                svc::Handle handle = m_debug_handle;
                if (R_FAILED(svc::WaitSynchronization(std::addressof(dummy_index), std::addressof(handle), 1, TimeSpan::FromMilliSeconds(20).GetNanoSeconds()))) {
                    continue;
                }

                svc::DebugEventInfo d;
                if (R_FAILED(this->GetProcessDebugEvent(std::addressof(d)))) {
                    continue;
                }

                if (d.type == svc::DebugEvent_Exception && d.info.exception.type == svc::DebugException_DebuggerBreak) {
                    this->SetLastThreadId(d.thread_id);
                    break;
                }
            }
        } else {
            AMS_DMNT2_GDB_LOG_WARN("DebugProcess::StartShared: BreakDebugProcess failed\n");
        }

        /* Query memory extents to infer address space flags. */
        this->CollectProcessInfo();

        const bool is_64_bit = (m_process_aslr_size > 0xFFFFFFFFULL) || (m_process_alias_size > 0xFFFFFFFFULL);
        if (is_64_bit) {
            m_create_process_info.flags = svc::CreateProcessFlag_Is64Bit | svc::CreateProcessFlag_AddressSpace64Bit;
        } else {
            m_create_process_info.flags = svc::CreateProcessFlag_AddressSpace32Bit;
        }

        m_is_64_bit               = (m_create_process_info.flags & svc::CreateProcessFlag_Is64Bit);
        m_is_64_bit_address_space = (m_create_process_info.flags & svc::CreateProcessFlag_AddressSpaceMask) == svc::CreateProcessFlag_AddressSpace64Bit;

        /* Populate process info. */
        m_create_process_info.process_id = process_id.value;

        /* Get program id from info. */
        R_TRY(svc::GetInfo(std::addressof(m_create_process_info.program_id), svc::InfoType_ProgramId, m_debug_handle, 0));

        /* Copy default process name. */
        std::strncpy(m_create_process_info.name, "Application", sizeof(m_create_process_info.name));
        m_create_process_info.name[sizeof(m_create_process_info.name) - 1] = '\0';

        /* Rebuild the thread table from scratch: draining the break above can
         * have consumed a queued CreateThread event and already added an entry,
         * and ThreadCreate() does not deduplicate. */
        m_thread_count = 0;
        std::memset(m_thread_valid, 0, sizeof(m_thread_valid));
        std::memset(m_thread_ids, 0, sizeof(m_thread_ids));

        /* Retrieve active thread list from the OS using GetThreadList. */
        s32 num_threads = 0;
        u64 thread_ids[ThreadCountMax];
        R_TRY(svc::GetThreadList(std::addressof(num_threads), thread_ids, ThreadCountMax, m_debug_handle));

        /* Initialize m_thread_valid and thread info for each active thread. */
        for (s32 i = 0; i < num_threads; ++i) {
            const u64 thread_id = thread_ids[i];
            const s32 index = this->ThreadCreate(thread_id);
            if (index >= 0) {
                /* Get thread context to retrieve TPIDR (TLS pointer). */
                svc::ThreadContext ctx = {};
                u32 flags = svc::ThreadContextFlag_General | svc::ThreadContextFlag_Control;

                svc::DebugInfoCreateThread create_thread = {};
                create_thread.thread_id = thread_id;

                if (R_SUCCEEDED(svc::GetDebugThreadContext(std::addressof(ctx), m_debug_handle, thread_id, flags))) {
                    create_thread.tls_address = ctx.tpidr;
                }

                const Result result = osdbg::InitializeThreadInfo(std::addressof(m_thread_infos[index]), m_debug_handle, std::addressof(m_create_process_info), std::addressof(create_thread));
                if (R_FAILED(result)) {
                    AMS_DMNT2_GDB_LOG_WARN("DebugProcess::StartShared: InitializeThreadInfo(%lx) failed: %08x\n", thread_id, result.GetValue());
                }
            }
        }

        /* Set ourselves as valid. */
        m_is_valid = true;
        this->SetDebugBreaked();

        R_SUCCEED();
    }

    s32 DebugProcess::ThreadCreate(u64 thread_id) {
        for (size_t i = 0; i < ThreadCountMax; ++i) {
            if (!m_thread_valid[i]) {
                m_thread_valid[i] = true;
                m_thread_ids[i]   = thread_id;

                this->SetLastThreadId(thread_id);
                this->SetLastSignal(GdbSignal_BreakpointTrap);

                ++m_thread_count;

                return i;
            }
        }

        return -1;
    }

    void DebugProcess::ThreadExit(u64 thread_id) {
        for (size_t i = 0; i < ThreadCountMax; ++i) {
            if (m_thread_valid[i] && m_thread_ids[i] == thread_id) {
                m_thread_valid[i] = false;
                m_thread_ids[i]   = 0;

                this->SetLastThreadId(thread_id);
                this->SetLastSignal(GdbSignal_BreakpointTrap);

                --m_thread_count;
                break;
            }
        }
    }

    Result DebugProcess::CollectModules() {
        /* Reset our module count. */
        m_module_count = 0;

        /* Traverse the address space, looking for modules. */
        uintptr_t address = 0;

        while (true) {
            /* Query the current address. */
            svc::MemoryInfo memory_info;
            svc::PageInfo page_info;
            if (R_SUCCEEDED(svc::QueryDebugProcessMemory(std::addressof(memory_info), std::addressof(page_info), m_debug_handle, address))) {
                if (memory_info.permission == svc::MemoryPermission_ReadExecute && (memory_info.state == svc::MemoryState_Code || memory_info.state == svc::MemoryState_AliasCode)) {
                    /* Check that we can add the module. */
                    AMS_ABORT_UNLESS(m_module_count < ModuleCountMax);

                    /* Get module definition. */
                    auto &module = m_module_definitions[m_module_count++];

                    /* Set module address/size. */
                    module.SetAddressSize(memory_info.base_address, memory_info.size);

                    /* Get module name buffer. */
                    char *module_name = module.GetNameBuffer();
                    std::memset(module_name, 0, ModuleDefinition::PathLengthMax);

                    /* Read module path. */
                    struct {
                        u32  zero;
                        s32  path_length;
                        char path[ModuleDefinition::PathLengthMax];
                        u32  dummy[3];
                    } module_path;
                    if (R_SUCCEEDED(this->ReadMemory(std::addressof(module_path), memory_info.base_address + memory_info.size, sizeof(module_path)))) {
                        if (module_path.zero == 0 && module_path.path_length > 0) {
                            std::memcpy(module_name, module_path.path, std::min<size_t>(ModuleDefinition::PathLengthMax, module_path.path_length));
                        } else if (module_path.zero == 1) {
                            struct {
                                u32 zero;
                                u32 dummy[3];
                                s32 path_length;
                                char path[ModuleDefinition::PathLengthMax];
                            } *alt_module_path = reinterpret_cast<decltype(alt_module_path)>(&module_path);

                            if (alt_module_path->path_length > 0) {
                                std::memcpy(module_name, alt_module_path->path, std::min<size_t>(ModuleDefinition::PathLengthMax, alt_module_path->path_length));
                            }
                            module_path.path_length = alt_module_path->path_length;
                        }
                    } else {
                        module_path.path_length = 0;
                    }

                    /* Truncate module name. */
                    module_name[ModuleDefinition::PathLengthMax - 1] = 0;

                    /* Set default module name start. */
                    module.SetNameStart(0);

                    /* Ignore leading directories. */
                    for (size_t i = 0; i < std::min<size_t>(ModuleDefinition::PathLengthMax, module_path.path_length) && module_name[i] != 0; ++i) {
                        if (module_name[i] == '/' || module_name[i] == '\\') {
                            module.SetNameStart(i + 1);
                        }
                    }
                }
            }

            /* Check if we're done. */
            const uintptr_t next_address = memory_info.base_address + memory_info.size;
            if (memory_info.state == svc::MemoryState_Inaccessible) {
                break;
            }
            if (next_address <= address) {
                break;
            }

            address = next_address;
        }

        R_SUCCEED();
    }

    void DebugProcess::CollectProcessInfo() {
        /* Define helper for getting process info. */
        auto CollectProcessInfoImpl = [&](os::NativeHandle handle) -> Result {
            /* Collect all values. */
            R_TRY(svc::GetInfo(std::addressof(m_process_alias_address), svc::InfoType_AliasRegionAddress, handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_alias_size),    svc::InfoType_AliasRegionSize,    handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_heap_address),  svc::InfoType_HeapRegionAddress,  handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_heap_size),     svc::InfoType_HeapRegionSize,     handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_aslr_address),  svc::InfoType_AslrRegionAddress,  handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_aslr_size),     svc::InfoType_AslrRegionSize,     handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_stack_address), svc::InfoType_StackRegionAddress, handle, 0));
            R_TRY(svc::GetInfo(std::addressof(m_process_stack_size),    svc::InfoType_StackRegionSize,    handle, 0));

            if (m_program_location.program_id == ncm::InvalidProgramId) {
                R_TRY(svc::GetInfo(std::addressof(m_program_location.program_id.value), svc::InfoType_ProgramId, handle, 0));
            }

            u64 value;
            R_TRY(svc::GetInfo(std::addressof(value), svc::InfoType_IsApplication, handle, 0));
            m_is_application = value != 0;

            R_SUCCEED();
        };

        /* Get process info/status. */
        os::NativeHandle process_handle;
        if (R_FAILED(pm::dmnt::AtmosphereGetProcessInfo(std::addressof(process_handle), std::addressof(m_program_location), std::addressof(m_process_override_status), m_process_id))) {
            process_handle            = os::InvalidNativeHandle;
            m_program_location        = { ncm::InvalidProgramId, };
            m_process_override_status = {};
        }
        ON_SCOPE_EXIT { os::CloseNativeHandle(process_handle); };

        /* Try collecting from our debug handle, then the process handle. */
        if (R_FAILED(CollectProcessInfoImpl(m_debug_handle)) && R_FAILED(CollectProcessInfoImpl(process_handle))) {
            m_process_alias_address = 0;
            m_process_alias_size    = 0;
            m_process_heap_address  = 0;
            m_process_heap_size     = 0;
            m_process_aslr_address  = 0;
            m_process_aslr_size     = 0;
            m_process_stack_address = 0;
            m_process_stack_size    = 0;
            m_is_application        = false;
        }
    }

    Result DebugProcess::GetThreadContext(svc::ThreadContext *out, u64 thread_id, u32 flags) {
        R_RETURN(svc::GetDebugThreadContext(out, m_debug_handle, thread_id, flags));
    }

    Result DebugProcess::SetThreadContext(const svc::ThreadContext *ctx, u64 thread_id, u32 flags) {
        R_RETURN(svc::SetDebugThreadContext(m_debug_handle, thread_id, ctx, flags));
    }

    Result DebugProcess::ReadMemory(void *dst, uintptr_t address, size_t size) {
        R_RETURN(svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(dst), m_debug_handle, address, size));
    }

    Result DebugProcess::WriteMemory(const void *src, uintptr_t address, size_t size) {
        R_RETURN(svc::WriteDebugProcessMemory(m_debug_handle, reinterpret_cast<uintptr_t>(src), address, size));
    }

    Result DebugProcess::QueryMemory(svc::MemoryInfo *out, uintptr_t address) {
        svc::PageInfo dummy;
        R_RETURN(svc::QueryDebugProcessMemory(out, std::addressof(dummy), m_debug_handle, address));
    }

    Result DebugProcess::Continue() {
        AMS_DMNT2_GDB_LOG_DEBUG("DebugProcess::Continue() all\n");

        u64 thread_ids[] = { 0 };
        R_TRY(svc::ContinueDebugEvent(m_debug_handle, svc::ContinueFlag_ExceptionHandled | svc::ContinueFlag_EnableExceptionEvent | svc::ContinueFlag_ContinueAll, thread_ids, util::size(thread_ids)));

        m_continue_thread_id = 0;
        m_status             = ProcessStatus_Running;

        this->SetLastThreadId(0);
        this->SetLastSignal(GdbSignal_Signal0);

        R_SUCCEED();
    }

    Result DebugProcess::Continue(u64 thread_id) {
        AMS_DMNT2_GDB_LOG_DEBUG("DebugProcess::Continue() thread_id=%lx\n", thread_id);

        u64 thread_ids[] = { thread_id };
        R_TRY(svc::ContinueDebugEvent(m_debug_handle, svc::ContinueFlag_ExceptionHandled | svc::ContinueFlag_EnableExceptionEvent, thread_ids, util::size(thread_ids)));

        m_continue_thread_id = thread_id;
        m_status             = ProcessStatus_Running;

        this->SetLastThreadId(0);
        this->SetLastSignal(GdbSignal_Signal0);

        R_SUCCEED();
    }

    Result DebugProcess::Step() {
        AMS_DMNT2_GDB_LOG_DEBUG("DebugProcess::Step() all\n");
        R_RETURN(this->Step(this->GetLastThreadId()));
    }

    Result DebugProcess::Step(u64 thread_id) {
        AMS_DMNT2_GDB_LOG_DEBUG("DebugProcess::Step() thread_id=%lx\n", thread_id);

        /* Get the thread context. */
        svc::ThreadContext ctx;
        R_TRY(this->GetThreadContext(std::addressof(ctx), thread_id, svc::ThreadContextFlag_Control));

        /* Note that we're stepping. */
        m_stepping = true;

        if (m_use_hardware_single_step) {
            /* Set thread single step. */
            R_TRY(this->SetThreadContext(std::addressof(ctx), thread_id, svc::ThreadContextFlag_SetSingleStep));
        } else {
            /* Determine where we're stepping to. */
            u64 current_pc  = ctx.pc;
            u64 step_target = 0;
            this->GetBranchTarget(ctx, thread_id, current_pc, step_target);

            /* Ensure we end with valid breakpoints. */
            auto bp_guard = SCOPE_GUARD { this->ClearStep(); };

            /* Set step breakpoint on current pc. */
            /* TODO: aarch32 breakpoints. */
            if (current_pc) {
                R_TRY(m_step_breakpoints.SetBreakPoint(current_pc, sizeof(u32), true));
            }

            if (step_target) {
                R_TRY(m_step_breakpoints.SetBreakPoint(step_target, sizeof(u32), true));
            }

            bp_guard.Cancel();
        }

        R_SUCCEED();
    }

    void DebugProcess::ClearStep() {
        /* If we should, clear our step breakpoints. */
        if (m_stepping) {
            m_step_breakpoints.ClearStep();
            m_stepping = false;
        }
    }

    Result DebugProcess::Break() {
        if (this->GetStatus() == ProcessStatus_Running) {
            AMS_DMNT2_GDB_LOG_DEBUG("DebugProcess::Break\n");
            R_RETURN(svc::BreakDebugProcess(m_debug_handle));
        } else {
            AMS_DMNT2_GDB_LOG_ERROR("DebugProcess::Break called on non-running process!\n");
            R_SUCCEED();
        }
    }

    Result DebugProcess::Terminate() {
        if (this->IsValid()) {
            R_ABORT_UNLESS(svc::TerminateDebugProcess(m_debug_handle));
            this->Detach();
        }

        R_SUCCEED();
    }

    void DebugProcess::GetBranchTarget(svc::ThreadContext &ctx, u64 thread_id, u64 &current_pc, u64 &target) {
        /* Save pc, in case we modify it. */
        const u64 pc = current_pc;

        /* Clear the target. */
        target = 0;

        /* By default, we advance by four. */
        current_pc += 4;

        /* Get the instruction where we were. */
        u32 insn = 0;
        this->ReadMemory(std::addressof(insn), pc, sizeof(insn));

        /* Handle by architecture. */
        bool is_call = false;
        if (this->Is64Bit()) {
            if ((insn & 0x7C000000) == 0x14000000) {
                /* Unconditional branch (b/bl) */
                if (insn != 0x14000001) {
                    is_call    = (insn & 0x80000000) == 0x80000000;
                    current_pc = 0;
                    target     = SignExtend(((insn & 0x03FFFFFF) << 2), 28) + pc;
                }
            } else if ((insn & 0x7E000000) == 0x34000000) {
                /* Compare/Branch (cbz/cbnz) */
                target = SignExtend(((insn & 0x00FFFFE0) >> 3), 21) + pc;
            } else if ((insn & 0x7E000000) == 0x36000000) {
                /* Test and branch (tbz/tbnz) */
                target = SignExtend(((insn & 0x0007FFE0) >> 3), 16) + pc;
            } else if ((insn & 0xFF000010) == 0x54000000) {
                /* Conditional branch (b.*) */
                if ((insn & 0xF) == 0xE) {
                    /* Unconditional. */
                    current_pc = 0;
                }
                target = SignExtend(((insn & 0x00FFFFE0) >> 3), 21) + pc;
            } else if ((insn & 0xFF8FFC1F) == 0xD60F0000) {
                /* Unconditional branch */
                is_call = (insn & 0x00F00000) == 0x00300000;
                if (!is_call) {
                    current_pc = 0;
                }

                /* Get the register. */
                svc::ThreadContext new_ctx;
                if (R_SUCCEEDED(this->GetThreadContext(std::addressof(new_ctx), thread_id, svc::ThreadContextFlag_Control | svc::ThreadContextFlag_General))) {
                    const int reg = (insn & 0x03E0) >> 5;
                    if (reg < 29) {
                        target = new_ctx.r[reg];
                    } else if (reg == 29) {
                        target = new_ctx.fp;
                    } else if (reg == 30) {
                        target = new_ctx.lr;
                    } else if (reg == 31) {
                        target = new_ctx.sp;
                    }
                }
            }
        } else {
            /* TODO aarch32 branch decoding */
            AMS_UNUSED(ctx);
        }
    }

    Result DebugProcess::SetBreakPoint(uintptr_t address, size_t size, bool is_step) {
        R_RETURN(m_software_breakpoints.SetBreakPoint(address, size, is_step));
    }

    Result DebugProcess::ClearBreakPoint(uintptr_t address, size_t size) {
        m_software_breakpoints.ClearBreakPoint(address, size);
        R_SUCCEED();
    }

    Result DebugProcess::SetHardwareBreakPoint(uintptr_t address, size_t size, bool is_step) {
        R_RETURN(m_hardware_breakpoints.SetBreakPoint(address, size, is_step));
    }

    Result DebugProcess::ClearHardwareBreakPoint(uintptr_t address, size_t size) {
        m_hardware_breakpoints.ClearBreakPoint(address, size);
        R_SUCCEED();
    }

    Result DebugProcess::SetWatchPoint(u64 address, u64 size, bool read, bool write) {
        R_RETURN(m_hardware_watchpoints.SetWatchPoint(address, size, read, write));
    }

    Result DebugProcess::ClearWatchPoint(u64 address, u64 size) {
        R_RETURN(m_hardware_watchpoints.ClearBreakPoint(address, size));
    }

    Result DebugProcess::GetWatchPointInfo(u64 address, bool &read, bool &write) {
        R_RETURN(m_hardware_watchpoints.GetWatchPointInfo(address, read, write));
    }

    bool DebugProcess::IsValidWatchPoint(u64 address, u64 size) {
        return HardwareWatchPointManager::IsValidWatchPoint(address, size);
    }

    Result DebugProcess::GetThreadCurrentCore(u32 *out, u64 thread_id) {
        u64 dummy_value;
        u32 val32 = 0;

        R_TRY(svc::GetDebugThreadParam(std::addressof(dummy_value), std::addressof(val32), m_debug_handle, thread_id, svc::DebugThreadParam_CurrentCore));

        *out = val32;
        R_SUCCEED();
    }

    Result DebugProcess::GetProcessDebugEvent(svc::DebugEventInfo *out) {
        /* Get the event. */
        R_TRY(svc::GetDebugEvent(out, m_debug_handle));

        /* Process the event. */
        switch (out->type) {
            case svc::DebugEvent_CreateProcess:
                {
                    /* Set our create process info. */
                    m_create_process_info = out->info.create_process;

                    /* Cache our bools. */
                    m_is_64_bit               = (m_create_process_info.flags & svc::CreateProcessFlag_Is64Bit);
                    m_is_64_bit_address_space = (m_create_process_info.flags & svc::CreateProcessFlag_AddressSpaceMask) == svc::CreateProcessFlag_AddressSpace64Bit;
                }
                break;
            case svc::DebugEvent_CreateThread:
                {
                    if (const s32 index = this->ThreadCreate(out->thread_id); index >= 0) {
                        const Result result = osdbg::InitializeThreadInfo(std::addressof(m_thread_infos[index]), m_debug_handle, std::addressof(m_create_process_info), std::addressof(out->info.create_thread));

                        if (R_FAILED(result)) {
                            AMS_DMNT2_GDB_LOG_WARN("DebugProcess::GetProcessDebugEvent: InitializeThreadInfo(%lx) failed: %08x\n", out->thread_id, result.GetValue());
                        }
                    }
                }
                break;
            case svc::DebugEvent_ExitThread:
                {
                    this->ThreadExit(out->thread_id);
                }
                break;
            default:
                break;
        }

        if (out->flags & svc::DebugEventFlag_Stopped) {
            this->SetDebugBreaked();
        }

        R_SUCCEED();
    }

    u64 DebugProcess::GetLastThreadId() {
        /* Select our first valid thread id. */
        if (m_last_thread_id == 0) {
            for (size_t i = 0; i < ThreadCountMax; ++i) {
                if (m_thread_valid[i]) {
                    SetLastThreadId(m_thread_ids[i]);
                    break;
                }
            }
        }

        return m_last_thread_id;
    }

    Result DebugProcess::GetThreadList(s32 *out_count, u64 *out_thread_ids, size_t max_count) {
        s32 count = 0;
        for (size_t i = 0; i < ThreadCountMax; ++i) {
            if (m_thread_valid[i]) {
                if (count < static_cast<s32>(max_count)) {
                    out_thread_ids[count++] = m_thread_ids[i];
                }
            }
        }

        *out_count = count;
        R_SUCCEED();
    }

    Result DebugProcess::GetThreadInfoList(s32 *out_count, osdbg::ThreadInfo **out_infos, size_t max_count) {
        s32 count = 0;
        for (size_t i = 0; i < ThreadCountMax; ++i) {
            if (m_thread_valid[i]) {
                if (count < static_cast<s32>(max_count)) {
                    out_infos[count++] = std::addressof(m_thread_infos[i]);
                }
            }
        }

        *out_count = count;
        R_SUCCEED();
    }

    void DebugProcess::GetThreadName(char *dst, u64 thread_id) const {
        for (size_t i = 0; i < ThreadCountMax; ++i) {
            if (m_thread_valid[i] && m_thread_ids[i] == thread_id) {
                if (R_FAILED(osdbg::GetThreadName(dst, std::addressof(m_thread_infos[i])))) {
                    if (m_thread_infos[i]._thread_type != 0) {
                        if (m_thread_infos[i]._thread_type_type == osdbg::ThreadTypeType_Libnx) {
                            util::TSNPrintf(dst, os::ThreadNameLengthMax, "libnx Thread_0x%010lx", reinterpret_cast<uintptr_t>(m_thread_infos[i]._thread_type));
                        } else {
                            util::TSNPrintf(dst, os::ThreadNameLengthMax, "Thread_0x%010lx", reinterpret_cast<uintptr_t>(m_thread_infos[i]._thread_type));
                        }
                    } else {
                        break;
                    }
                }
                return;
            }
        }

        util::TSNPrintf(dst, os::ThreadNameLengthMax, "Thread_ID=%lu", thread_id);
    }

}