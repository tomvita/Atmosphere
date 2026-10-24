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

namespace ams::dmnt::cheat::impl {

    void InitializeCheatManager();

    os::NativeHandle GetSharedDebugHandle();
    void SuspendDebugEvents(bool suspend);

    /* Block until the cheat engine's debug-events thread is definitely not
     * holding a debug event on the shared handle. Call after
     * SuspendDebugEvents(true) before taking over event handling.
     * Returns false on timeout. */
    bool WaitDebugEventsParked(TimeSpan timeout);

    /* gen2 taking the next application launch (v0.21).
     *
     * pm keeps one hook on "an application was created", and the cheat
     * engine's launch thread holds it for good. So a gen2 client that wants a
     * game from its first instruction (Breeze's block, or GDB's
     * `monitor wait application`) registers here instead. On the next launch
     * the launch thread leaves the process created but not started and hands
     * its pid to that client, which attaches and starts it itself. Nobody
     * taking it within a few seconds gets it started the usual way, so a
     * launch can never be left hanging. */
    enum Gen2LaunchWaiter : u8 {
        Gen2LaunchWaiter_None  = 0,
        Gen2LaunchWaiter_Block = 1,
        Gen2LaunchWaiter_Gdb   = 2,
    };
    /* False when another waiter already holds the next launch. */
    bool SetGen2LaunchWait(Gen2LaunchWaiter waiter);
    /* Stops waiting, only if `waiter` is the one waiting. */
    void CancelGen2LaunchWait(Gen2LaunchWaiter waiter);
    Gen2LaunchWaiter GetGen2LaunchWait();
    /* The created, not yet started process handed to `waiter`, or 0. Taking
     * it makes starting it the taker's job. */
    u64 TakeGen2LaunchedProcess(Gen2LaunchWaiter waiter);

    bool GetHasActiveCheatProcess();
    os::NativeHandle GetCheatProcessEventHandle();
    Result GetCheatProcessMetadata(CheatProcessMetadata *out);
    Result ForceOpenCheatProcess();
    Result PauseCheatProcess();
    Result ResumeCheatProcess();
    Result ForceCloseCheatProcess();

    Result ReadCheatProcessMemoryUnsafe(u64 process_addr, void *out_data, size_t size);
    Result WriteCheatProcessMemoryUnsafe(u64 process_addr, void *data, size_t size);

    Result PauseCheatProcessUnsafe();
    Result ResumeCheatProcessUnsafe();

    Result GetCheatProcessMappingCount(u64 *out_count);
    Result GetCheatProcessMappings(svc::MemoryInfo *mappings, size_t max_count, u64 *out_count, u64 offset);
    Result ReadCheatProcessMemory(u64 proc_addr, void *out_data, size_t size);
    Result WriteCheatProcessMemory(u64 proc_addr, const void *data, size_t size);
    Result QueryCheatProcessMemory(svc::MemoryInfo *mapping, u64 address);

    Result GetCheatCount(u64 *out_count);
    Result GetCheats(CheatEntry *cheats, size_t max_count, u64 *out_count, u64 offset);
    Result GetCheatById(CheatEntry *out_cheat, u32 cheat_id);
    Result ToggleCheat(u32 cheat_id);
    Result AddCheat(u32 *out_id, const CheatDefinition &def, bool enabled);
    Result RemoveCheat(u32 cheat_id);
    Result SetMasterCheat(const CheatDefinition &def);
    Result SetMemoryBreakpoint(u64 address);
    Result ReadStaticRegister(u64 *out, size_t which);
    Result WriteStaticRegister(size_t which, u64 value);
    Result ResetStaticRegisters();

    Result GetFrozenAddressCount(u64 *out_count);
    Result GetFrozenAddresses(FrozenAddressEntry *frz_addrs, size_t max_count, u64 *out_count, u64 offset);
    Result GetFrozenAddress(FrozenAddressEntry *frz_addr, u64 address);
    Result EnableFrozenAddress(u64 *out_value, u64 address, u64 width);
    Result DisableFrozenAddress(u64 address);

}
