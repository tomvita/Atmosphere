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
#pragma once
#include <stratosphere.hpp>
#include "dmnt2_breakpoint_manager.hpp"

namespace ams::dmnt {

    /* One recorded debug-register write, surfaced by `monitor dbgregs`.
     * The ARM debug registers are per-core and the kernel neither saves nor
     * restores them, so this is the only way to tell a watchpoint that was
     * overwritten from one whose write never reached the core that mattered.
     * See WATCHPOINT_BUG.md. */
    struct DebugRegisterTraceEntry {
        u64 tick;
        u64 dbgbcr;
        u64 value;
        u32 reg;
        u32 result;
        s16 requested_core;
        s16 observed_core;
        u16 migrate_spins;
        u16 reserved;
    };

    constexpr size_t DebugRegisterTraceCount = 64;

    struct HardwareBreakPoint : public BreakPoint {
        svc::HardwareBreakPointRegisterName m_reg;
        svc::HardwareBreakPointRegisterName m_ctx;

        void Initialize(svc::HardwareBreakPointRegisterName r, svc::HardwareBreakPointRegisterName c) {
            m_reg = r;
            m_ctx = c;
        }

        virtual Result Clear(DebugProcess *debug_process) override;
        virtual Result Set(DebugProcess *debug_process, uintptr_t address, size_t size, bool is_step) override;
    };

    class HardwareBreakPointManager : public BreakPointManager {
        public:
            static constexpr size_t BreakPointCountMax = 0x10;
        private:
            HardwareBreakPoint m_breakpoints[BreakPointCountMax];
        public:
            static Result SetHardwareBreakPoint(u32 r, u64 dbgbcr, u64 value);
            static Result SetContextBreakPoint(svc::HardwareBreakPointRegisterName ctx, DebugProcess *debug_process);
            static svc::HardwareBreakPointRegisterName GetWatchPointContextRegister();
            static Result SetExecutionBreakPoint(svc::HardwareBreakPointRegisterName reg, svc::HardwareBreakPointRegisterName ctx, u64 address);

            /* Number of execution-breakpoint slots we may actually hand out:
             * the registers below the reserved context-aware comparators. */
            static size_t GetUsableBreakPointCount();

            /* Copy trace entry `index` (oldest first) into `out`; returns the
             * number of entries available, or 0 if `index` is past the end.
             * `out_total` receives the lifetime write count. */
            static size_t GetDebugRegisterTrace(size_t index, DebugRegisterTraceEntry *out, u64 *out_total);

            static void GetRegisterExtents(int *out_last_bp, int *out_first_ctx, int *out_last_ctx, int *out_last_wp);
        public:
            explicit HardwareBreakPointManager(DebugProcess *debug_process);
        private:
            virtual BreakPointBase *GetBreakPoint(size_t index) override;
        private:
            static void CountBreakPointRegisters();

            static bool SendMultiCoreRequest(const void *request);
    };

}