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
#include "dmnt2_hardware_breakpoint.hpp"
#include "dmnt2_debug_process.hpp"
#include "dmnt2_debug_log.hpp"

namespace ams::dmnt {

    namespace {

        constinit auto g_last_bp_register      = -1;
        constinit auto g_first_bp_ctx_register = -1;
        constinit auto g_last_bp_ctx_register  = -1;
        constinit auto g_last_wp_register      = -1;

        constinit os::SdkMutex g_multicore_lock;
        constinit bool g_multicore_started = false;

        alignas(os::ThreadStackAlignment) constinit u8 g_multicore_thread_stack[16_KB];
        constinit os::ThreadType g_multicore_thread;
        constinit os::MessageQueueType g_multicore_request_queue;
        constinit os::MessageQueueType g_multicore_response_queue;
        constinit uintptr_t g_multicore_request_storage[2];
        constinit uintptr_t g_multicore_response_storage[2];

        /* Trace of every debug-register write we perform, so `monitor dbgregs`
         * can answer "did anything touch this register after I armed it, and
         * did the write actually land on the core it was meant for?".
         *
         * The ARM debug registers are per-core and the kernel neither saves
         * nor restores them, so a watchpoint that silently stops firing can
         * only be explained by (a) a later write to the same register or to
         * the context comparator it is linked to, or (b) the original write
         * never reaching that core. This buffer distinguishes the two.
         * See WATCHPOINT_BUG.md. */
        constinit os::SdkMutex g_dbg_trace_lock;
        constinit DebugRegisterTraceEntry g_dbg_trace[DebugRegisterTraceCount] = {};
        constinit u64 g_dbg_trace_total = 0;

        void RecordDebugRegisterWrite(s32 requested_core, s32 observed_core, u32 spins, u64 reg, u64 dbgbcr, u64 value, u32 result) {
            std::scoped_lock lk(g_dbg_trace_lock);

            auto &entry = g_dbg_trace[g_dbg_trace_total % DebugRegisterTraceCount];

            entry.tick           = os::GetSystemTick().GetInt64Value();
            entry.dbgbcr         = dbgbcr;
            entry.value          = value;
            entry.reg            = static_cast<u32>(reg);
            entry.result         = result;
            entry.requested_core = static_cast<s16>(requested_core);
            entry.observed_core  = static_cast<s16>(observed_core);
            entry.migrate_spins  = static_cast<u16>(spins);

            ++g_dbg_trace_total;
        }

        /* Number of times we re-check the processor number before giving up on
         * a migration. Each spin is a yield, so this costs nothing when the
         * migration is immediate (the common case). */
        constexpr s32 MaxMigrateSpins = 64;

        void MultiCoreThread(void *) {
            /* Service requests. */
            while (true) {
                /* Wait for a request to come in. */
                uintptr_t request_v;
                os::ReceiveMessageQueue(std::addressof(request_v), std::addressof(g_multicore_request_queue));

                /* Process the request. */
                const uintptr_t *request = reinterpret_cast<const uintptr_t *>(request_v);
                bool success = true;
                if (request[0] == 1) {
                    /* Set on each core. */
                    for (s32 core = 0; core < 4; ++core) {
                        /* Switch to the desired core. */
                        R_ABORT_UNLESS(svc::SetThreadCoreMask(svc::PseudoHandle::CurrentThread, core, (1 << core)));

                        /* Wait until we are actually executing on that core.
                         *
                         * svc::GetThreadCoreMask (which this code used to call
                         * here, and then discard the result of) reports the
                         * thread's *ideal* core - the value we just set - so it
                         * proves nothing. GetCurrentProcessorNumber reports the
                         * core we are running on right now, which is what
                         * decides where SetHardwareBreakPoint's write lands.
                         *
                         * SetThreadCoreMask requests a migration but does not
                         * guarantee it has happened by the time the SVC returns;
                         * if we write while still on the old core, the target
                         * core silently never gets the breakpoint. */
                        s32 observed = svc::GetCurrentProcessorNumber();
                        s32 spins    = 0;
                        while (observed != core && spins < MaxMigrateSpins) {
                            /* A yield is enough when the scheduler simply has
                             * not run yet, but it returns immediately if
                             * nothing at our priority (2) is runnable - which
                             * is the normal case. Fall back to a short sleep so
                             * we are actually descheduled and the target core
                             * can pick us up. Worst case is a few ms. */
                            if (spins < 8) {
                                os::YieldThread();
                            } else {
                                os::SleepThread(TimeSpan::FromMicroSeconds(100));
                            }
                            observed = svc::GetCurrentProcessorNumber();
                            ++spins;
                        }

                        /* Set the breakpoint. */
                        const Result result = svc::SetHardwareBreakPoint(static_cast<svc::HardwareBreakPointRegisterName>(request[1]), request[2], request[3]);

                        RecordDebugRegisterWrite(core, observed, spins, request[1], request[2], request[3], result.GetValue());

                        if (observed != core) {
                            /* We never reached the core, so this core did not
                             * get the breakpoint. Report failure rather than
                             * leaving the caller believing it is armed
                             * everywhere - a watchpoint armed on three cores
                             * out of four is exactly the silent half-failure
                             * this whole change exists to surface.
                             *
                             * Keep going through the remaining cores: arming
                             * what we can is strictly better than stopping, and
                             * the caller is told either way. */
                            success = false;
                            AMS_DMNT2_GDB_LOG_ERROR("SetHardwareBreakPoint MIGRATE FAIL, want core=%d, on core=%d, reg=%lu\n", core, observed, request[1]);
                            continue;
                        }

                        if (R_FAILED(result)) {
                            success = false;
                            AMS_DMNT2_GDB_LOG_ERROR("SetHardwareBreakPoint FAIL 0x%08x, core=%d, reg=%lu, ctrl=%lx, val=%lx\n", result.GetValue(), core, request[1], request[2], request[3]);
                            break;
                        }
                    }
                }

                os::SendMessageQueue(std::addressof(g_multicore_response_queue), static_cast<uintptr_t>(success));
            }
        }

        void EnsureMultiCoreStarted() {
            std::scoped_lock lk(g_multicore_lock);
            if (!g_multicore_started) {
                os::InitializeMessageQueue(std::addressof(g_multicore_request_queue), g_multicore_request_storage, util::size(g_multicore_request_storage));
                os::InitializeMessageQueue(std::addressof(g_multicore_response_queue), g_multicore_response_storage, util::size(g_multicore_response_storage));

                R_ABORT_UNLESS(os::CreateThread(std::addressof(g_multicore_thread), MultiCoreThread, nullptr, g_multicore_thread_stack, sizeof(g_multicore_thread_stack), os::HighestThreadPriority - 1));
                os::StartThread(std::addressof(g_multicore_thread));

                g_multicore_started = true;
            }
        }

    }

    Result HardwareBreakPoint::Clear(DebugProcess *debug_process) {
        AMS_UNUSED(debug_process);

        Result result = svc::ResultInvalidArgument();
        if (m_in_use) {
            AMS_DMNT2_GDB_LOG_DEBUG("HardwareBreakPoint::Clear %p 0x%lx\n", this, m_address);
            result = HardwareBreakPointManager::SetExecutionBreakPoint(m_reg, m_ctx, 0);
            this->Reset();
        }
        R_RETURN(result);
    }

    Result HardwareBreakPoint::Set(DebugProcess *debug_process, uintptr_t address, size_t size, bool is_step) {
        /* Set fields. */
        m_is_step = is_step;
        m_address = address;
        m_size    = size;

        /* Set context breakpoint. */
        R_TRY(HardwareBreakPointManager::SetContextBreakPoint(m_ctx, debug_process));

        /* Set execution breakpoint. */
        R_TRY(HardwareBreakPointManager::SetExecutionBreakPoint(m_reg, m_ctx, address));

        /* Set as in-use. */
        m_in_use = true;
        R_SUCCEED();
    }

    HardwareBreakPointManager::HardwareBreakPointManager(DebugProcess *debug_process) : BreakPointManager(debug_process) {
        /* Determine the number of breakpoint registers. */
        CountBreakPointRegisters();

        /* Initialize all breakpoints. */
        for (size_t i = 0; i < util::size(m_breakpoints); ++i) {
            m_breakpoints[i].Initialize(static_cast<svc::HardwareBreakPointRegisterName>(svc::HardwareBreakPointRegisterName_I0 + i), static_cast<svc::HardwareBreakPointRegisterName>(g_first_bp_ctx_register));
        }
    }

    size_t HardwareBreakPointManager::GetUsableBreakPointCount() {
        CountBreakPointRegisters();

        /* Execution breakpoints may only use the registers below the
         * context-aware ones.
         *
         * The context-aware comparators are architecturally the highest
         * numbered breakpoint registers (I4/I5 on the Switch's Cortex-A57),
         * and we permanently reserve both: g_first_bp_ctx_register links
         * execution breakpoints to the debugged process, and
         * g_first_bp_ctx_register + 1 does the same for watchpoints.
         *
         * Before this bound existed, GetFreeBreakPoint() walked all
         * BreakPointCountMax (0x10) slots, so a 5th simultaneous hardware
         * execution breakpoint was handed I4 and a 6th was handed I5 - and
         * writing I5 as a plain execution breakpoint destroys the comparator
         * every armed watchpoint is linked to (the kernel sets WT=1 and we put
         * ctx in LBN). Every watchpoint then stops firing, silently and with
         * no error, until something calls WatchPoint::Set again. See
         * WATCHPOINT_BUG.md, Finding B. */
        if (g_first_bp_ctx_register <= 0) {
            return 0;
        }

        return std::min<size_t>(static_cast<size_t>(g_first_bp_ctx_register), BreakPointCountMax);
    }

    BreakPointBase *HardwareBreakPointManager::GetBreakPoint(size_t index) {
        if (index < GetUsableBreakPointCount()) {
            return m_breakpoints + index;
        } else {
            return nullptr;
        }
    }

    size_t HardwareBreakPointManager::GetDebugRegisterTrace(size_t index, DebugRegisterTraceEntry *out, u64 *out_total) {
        std::scoped_lock lk(g_dbg_trace_lock);

        if (out_total != nullptr) {
            *out_total = g_dbg_trace_total;
        }

        /* Report the most recent entries, oldest first. */
        const size_t count = std::min<size_t>(g_dbg_trace_total, DebugRegisterTraceCount);
        if (index >= count) {
            return 0;
        }

        *out = g_dbg_trace[(g_dbg_trace_total - count + index) % DebugRegisterTraceCount];
        return count;
    }

    void HardwareBreakPointManager::GetRegisterExtents(int *out_last_bp, int *out_first_ctx, int *out_last_ctx, int *out_last_wp) {
        CountBreakPointRegisters();

        *out_last_bp   = g_last_bp_register;
        *out_first_ctx = g_first_bp_ctx_register;
        *out_last_ctx  = g_last_bp_ctx_register;
        *out_last_wp   = g_last_wp_register;
    }

    Result HardwareBreakPointManager::SetHardwareBreakPoint(u32 r, u64 dbgbcr, u64 value) {
        /* Send request. */
        const uintptr_t request[4] = {
            1,
            r,
            dbgbcr,
            value
        };
        R_UNLESS(SendMultiCoreRequest(request), dmnt::ResultUnknown());

        R_SUCCEED();
    }

    Result HardwareBreakPointManager::SetContextBreakPoint(svc::HardwareBreakPointRegisterName ctx, DebugProcess *debug_process) {
        /* Encode the register. */
        const u64 dbgbcr = (0x3 << 20) | (0 << 16) | (0xF << 5) | 1;

        const Result result = SetHardwareBreakPoint(ctx, dbgbcr, debug_process->GetHandle());
        if (R_FAILED(result)) {
            AMS_DMNT2_GDB_LOG_ERROR("SetContextBreakPoint FAIL 0x%08x ctx=%d\n", result.GetValue(), ctx);
        }

        R_RETURN(result);
    }

    svc::HardwareBreakPointRegisterName HardwareBreakPointManager::GetWatchPointContextRegister() {
        CountBreakPointRegisters();
        return static_cast<svc::HardwareBreakPointRegisterName>(g_first_bp_ctx_register + 1);
    }

    Result HardwareBreakPointManager::SetExecutionBreakPoint(svc::HardwareBreakPointRegisterName reg, svc::HardwareBreakPointRegisterName ctx, u64 address) {
        /* Encode the register. */
        const u64 dbgbcr = (0x1 << 20) | (ctx << 16) | (0xF << 5) | ((address != 0) ? 1 : 0);

        const Result result = SetHardwareBreakPoint(reg, dbgbcr, address);
        if (R_FAILED(result)) {
            AMS_DMNT2_GDB_LOG_ERROR("SetContextBreakPoint FAIL 0x%08x reg=%d, ctx=%d, address=%lx\n", result.GetValue(), reg, ctx, address);
        }

        R_RETURN(result);
    }

    void HardwareBreakPointManager::CountBreakPointRegisters() {
        /* Determine the valid breakpoint extents. */
        if (g_last_bp_ctx_register == -1) {
            /* Keep setting until we see a failure. */
            for (int i = svc::HardwareBreakPointRegisterName_I0; i <= static_cast<int>(svc::HardwareBreakPointRegisterName_I15); ++i) {
                if (R_FAILED(svc::SetHardwareBreakPoint(static_cast<svc::HardwareBreakPointRegisterName>(i), 0, 0))) {
                    break;
                }
                g_last_bp_register = i;
            }
            AMS_DMNT2_GDB_LOG_DEBUG("Last valid breakpoint=%d\n", g_last_bp_register);

            /* Determine the context register range. */
            const u64 dbgbcr = (0x3 << 20) | (0x0 << 16) | (0xF << 5) | 1;

            g_last_bp_ctx_register = g_last_bp_register;
            for (int i = g_last_bp_ctx_register; i >= static_cast<int>(svc::HardwareBreakPointRegisterName_I0); --i) {
                const Result result = svc::SetHardwareBreakPoint(static_cast<svc::HardwareBreakPointRegisterName>(i), dbgbcr, svc::PseudoHandle::CurrentProcess);
                svc::SetHardwareBreakPoint(static_cast<svc::HardwareBreakPointRegisterName>(i), 0, 0);

                if (R_FAILED(result)) {
                    if (!svc::ResultInvalidHandle::Includes(result)) {
                        break;
                    }
                }

                g_first_bp_ctx_register = i;
            }
            AMS_DMNT2_GDB_LOG_DEBUG("Context BreakPoints = %d-%d\n", g_first_bp_ctx_register, g_last_bp_ctx_register);

            /* Determine valid watchpoint registers. */
            for (int i = svc::HardwareBreakPointRegisterName_D0; i <= static_cast<int>(svc::HardwareBreakPointRegisterName_D15); ++i) {
                if (R_FAILED(svc::SetHardwareBreakPoint(static_cast<svc::HardwareBreakPointRegisterName>(i), 0, 0))) {
                    break;
                }
                g_last_wp_register = i - svc::HardwareBreakPointRegisterName_D0;
            }
            AMS_DMNT2_GDB_LOG_DEBUG("Last valid watchpoint=%d\n", g_last_wp_register);
        }
    }

    bool HardwareBreakPointManager::SendMultiCoreRequest(const void *request) {
        /* Ensure the multi core thread is active. */
        EnsureMultiCoreStarted();

        /* Send the request. */
        os::SendMessageQueue(std::addressof(g_multicore_request_queue), reinterpret_cast<uintptr_t>(request));

        /* Get the response. */
        uintptr_t response;
        os::ReceiveMessageQueue(std::addressof(response), std::addressof(g_multicore_response_queue));

        return static_cast<bool>(response);
    }

}
