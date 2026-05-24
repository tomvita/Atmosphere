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
#include "dmnt_cheat_service.hpp"
#include "impl/dmnt_cheat_api.hpp"
#include "../dmnt2_debug_log.hpp"
#include "../gen2type.hpp"

namespace ams::dmnt {
    extern m_watch_data_t m_watch_data;
    /* Mutex protecting m_watch_data against the gen2_loop poll thread,
     * the GDB events thread (capture-time updates), and the dmnt:cht
     * IPC handlers below. See gen2fork_design_and_review.md Opt 3. */
    extern os::SdkMutex g_watch_data_lock;
    /* Signaled to wake gen2_loop's polling thread as soon as a client
     * sets execute=true, so commands are processed sub-millisecond
     * instead of waiting for the next 50 ms tick. See Opt 1. */
    extern os::Event g_gen2_request_event;
}

namespace ams::dmnt::cheat {

    /* ========================================================================================= */
    /* ====================================  Meta Commands  ==================================== */
    /* ========================================================================================= */

    void CheatService::HasCheatProcess(sf::Out<bool> out) {
        bool has_process = dmnt::cheat::impl::GetHasActiveCheatProcess();
        AMS_DMNT2_DEBUG_LOG("HasCheatProcess() -> %d\n", has_process);
        out.SetValue(has_process);
    }

    void CheatService::GetCheatProcessEvent(sf::OutCopyHandle out_event) {
        AMS_DMNT2_DEBUG_LOG("GetCheatProcessEvent()\n");
        out_event.SetValue(dmnt::cheat::impl::GetCheatProcessEventHandle(), false);
    }

    Result CheatService::GetCheatProcessMetadata(sf::Out<CheatProcessMetadata> out_metadata) {
        Result rc = dmnt::cheat::impl::GetCheatProcessMetadata(out_metadata.GetPointer());
        AMS_DMNT2_DEBUG_LOG("GetCheatProcessMetadata() -> result: 0x%08x\n", rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::ForceOpenCheatProcess() {
        Result rc = dmnt::cheat::impl::ForceOpenCheatProcess();
        AMS_DMNT2_DEBUG_LOG("ForceOpenCheatProcess() -> result: 0x%08x\n", rc.GetValue());
        R_UNLESS(R_SUCCEEDED(rc), dmnt::cheat::ResultCheatNotAttached());
        R_SUCCEED();
    }

    Result CheatService::PauseCheatProcess() {
        Result rc = dmnt::cheat::impl::PauseCheatProcess();
        AMS_DMNT2_DEBUG_LOG("PauseCheatProcess() -> result: 0x%08x\n", rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::ResumeCheatProcess() {
        Result rc = dmnt::cheat::impl::ResumeCheatProcess();
        AMS_DMNT2_DEBUG_LOG("ResumeCheatProcess() -> result: 0x%08x\n", rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::ForceCloseCheatProcess() {
        Result rc = dmnt::cheat::impl::ForceCloseCheatProcess();
        AMS_DMNT2_DEBUG_LOG("ForceCloseCheatProcess() -> result: 0x%08x\n", rc.GetValue());
        R_RETURN(rc);
    }

    /* ========================================================================================= */
    /* ===================================  Memory Commands  =================================== */
    /* ========================================================================================= */

    Result CheatService::GetCheatProcessMappingCount(sf::Out<u64> out_count) {
        Result rc = dmnt::cheat::impl::GetCheatProcessMappingCount(out_count.GetPointer());
        AMS_DMNT2_DEBUG_LOG("GetCheatProcessMappingCount() -> result: 0x%08x\n", rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::GetCheatProcessMappings(const sf::OutArray<svc::MemoryInfo> &mappings, sf::Out<u64> out_count, u64 offset) {
        Result rc = dmnt::cheat::impl::GetCheatProcessMappings(mappings.GetPointer(), mappings.GetSize(), out_count.GetPointer(), offset);
        AMS_DMNT2_DEBUG_LOG("GetCheatProcessMappings(offset: %lu) -> result: 0x%08x\n", offset, rc.GetValue());
        R_UNLESS(mappings.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        R_RETURN(rc);
    }

    Result CheatService::ReadCheatProcessMemory(const sf::OutBuffer &buffer, u64 address, u64 out_size) {
        Result rc = dmnt::cheat::impl::ReadCheatProcessMemory(address, buffer.GetPointer(), std::min(out_size, buffer.GetSize()));
        // AMS_DMNT2_DEBUG_LOG("ReadCheatProcessMemory(addr: 0x%lx, size: %lu) -> result: 0x%08x\n", address, out_size, rc.GetValue());
        R_UNLESS(buffer.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        R_RETURN(rc);
    }

    Result CheatService::WriteCheatProcessMemory(const sf::InBuffer &buffer, u64 address, u64 in_size) {
        Result rc = dmnt::cheat::impl::WriteCheatProcessMemory(address, buffer.GetPointer(), std::min(in_size, buffer.GetSize()));
        AMS_DMNT2_DEBUG_LOG("WriteCheatProcessMemory(addr: 0x%lx, size: %lu) -> result: 0x%08x\n", address, in_size, rc.GetValue());
        R_UNLESS(buffer.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        R_RETURN(rc);
    }

    Result CheatService::QueryCheatProcessMemory(sf::Out<svc::MemoryInfo> mapping, u64 address) {
        Result rc = dmnt::cheat::impl::QueryCheatProcessMemory(mapping.GetPointer(), address);
        // AMS_DMNT2_DEBUG_LOG("QueryCheatProcessMemory(addr: 0x%lx) -> result: 0x%08x\n", address, rc.GetValue());
        R_RETURN(rc);
    }

    /* ========================================================================================= */
    /* ===================================  Cheat Commands  ==================================== */
    /* ========================================================================================= */

    Result CheatService::GetCheatCount(sf::Out<u64> out_count) {
        Result rc = dmnt::cheat::impl::GetCheatCount(out_count.GetPointer());
        AMS_DMNT2_DEBUG_LOG("GetCheatCount() -> result: 0x%08x, count: %lu\n", rc.GetValue(), out_count.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::GetCheats(const sf::OutArray<CheatEntry> &cheats, sf::Out<u64> out_count, u64 offset) {
        Result rc = dmnt::cheat::impl::GetCheats(cheats.GetPointer(), cheats.GetSize(), out_count.GetPointer(), offset);
        AMS_DMNT2_DEBUG_LOG("GetCheats(offset: %lu) -> result: 0x%08x, count: %lu\n", offset, rc.GetValue(), out_count.GetValue());
        R_UNLESS(cheats.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        R_RETURN(rc);
    }

    Result CheatService::GetCheatById(sf::Out<CheatEntry> cheat, u32 cheat_id) {
        Result rc = dmnt::cheat::impl::GetCheatById(cheat.GetPointer(), cheat_id);
        AMS_DMNT2_DEBUG_LOG("GetCheatById(id: %u) -> result: 0x%08x\n", cheat_id, rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::ToggleCheat(u32 cheat_id) {
        Result rc = dmnt::cheat::impl::ToggleCheat(cheat_id);
        AMS_DMNT2_DEBUG_LOG("ToggleCheat(id: %u) -> result: 0x%08x\n", cheat_id, rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::AddCheat(const CheatDefinition &cheat, sf::Out<u32> out_cheat_id, bool enabled) {
        Result rc = dmnt::cheat::impl::AddCheat(out_cheat_id.GetPointer(), cheat, enabled);
        AMS_DMNT2_DEBUG_LOG("AddCheat() -> result: 0x%08x, id: %u\n", rc.GetValue(), out_cheat_id.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::RemoveCheat(u32 cheat_id) {
        Result rc = dmnt::cheat::impl::RemoveCheat(cheat_id);
        AMS_DMNT2_DEBUG_LOG("RemoveCheat(id: %u) -> result: 0x%08x\n", cheat_id, rc.GetValue());
        R_RETURN(rc);
    }

    Result CheatService::ReadStaticRegister(sf::Out<u64> out, u8 which) {
        R_RETURN(dmnt::cheat::impl::ReadStaticRegister(out.GetPointer(), which));
    }

    Result CheatService::WriteStaticRegister(u8 which, u64 value) {
        R_RETURN(dmnt::cheat::impl::WriteStaticRegister(which, value));
    }

    Result CheatService::ResetStaticRegisters() {
        R_RETURN(dmnt::cheat::impl::ResetStaticRegisters());
    }

    Result CheatService::SetMasterCheat(const CheatDefinition &cheat) {
        R_RETURN(dmnt::cheat::impl::SetMasterCheat(cheat));
    }

    Result CheatService::SetMemoryBreakpoint(u64 address) {
        R_RETURN(dmnt::cheat::impl::SetMemoryBreakpoint(address));
    }

    /* ========================================================================================= */
    /* ===================================  Address Commands  ================================== */
    /* ========================================================================================= */

    Result CheatService::GetFrozenAddressCount(sf::Out<u64> out_count) {
        R_RETURN(dmnt::cheat::impl::GetFrozenAddressCount(out_count.GetPointer()));
    }

    Result CheatService::GetFrozenAddresses(const sf::OutArray<FrozenAddressEntry> &addresses, sf::Out<u64> out_count, u64 offset) {
        R_UNLESS(addresses.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        R_RETURN(dmnt::cheat::impl::GetFrozenAddresses(addresses.GetPointer(), addresses.GetSize(), out_count.GetPointer(), offset));
    }

    Result CheatService::GetFrozenAddress(sf::Out<FrozenAddressEntry> entry, u64 address) {
        R_RETURN(dmnt::cheat::impl::GetFrozenAddress(entry.GetPointer(), address));
    }

    Result CheatService::EnableFrozenAddress(sf::Out<u64> out_value, u64 address, u64 width) {
        /* Width needs to be a power of two <= 8. */
        R_UNLESS(width > 0,                  dmnt::cheat::ResultFrozenAddressInvalidWidth());
        R_UNLESS(width <= sizeof(u64),       dmnt::cheat::ResultFrozenAddressInvalidWidth());
        R_UNLESS((width & (width - 1)) == 0, dmnt::cheat::ResultFrozenAddressInvalidWidth());
        R_RETURN(dmnt::cheat::impl::EnableFrozenAddress(out_value.GetPointer(), address, width));
    }

    Result CheatService::DisableFrozenAddress(u64 address) {
        R_RETURN(dmnt::cheat::impl::DisableFrozenAddress(address));
    }

    Result CheatService::GetGen2WatchData(const sf::OutBuffer &buffer) {
        R_UNLESS(buffer.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        const size_t copy_size = std::min(sizeof(ams::dmnt::m_watch_data), buffer.GetSize());
        {
            std::scoped_lock lk(ams::dmnt::g_watch_data_lock);
            std::memcpy(buffer.GetPointer(), std::addressof(ams::dmnt::m_watch_data), copy_size);
        }
        R_SUCCEED();
    }

    Result CheatService::SetGen2WatchData(const sf::InBuffer &buffer) {
        R_UNLESS(buffer.GetPointer() != nullptr, dmnt::cheat::ResultCheatNullBuffer());
        const size_t copy_size = std::min(sizeof(ams::dmnt::m_watch_data), buffer.GetSize());
        bool wake_loop;
        {
            std::scoped_lock lk(ams::dmnt::g_watch_data_lock);
            std::memcpy(std::addressof(ams::dmnt::m_watch_data), buffer.GetPointer(), copy_size);
            /* Wake the gen2_loop polling thread immediately if the client
             * is requesting an action. This is the Opt 1 latency fix:
             * sub-millisecond command dispatch instead of waiting up to
             * 50 ms for the next periodic tick. */
            wake_loop = ams::dmnt::m_watch_data.execute;
        }
        if (wake_loop) {
            ams::dmnt::g_gen2_request_event.Signal();
        }
        R_SUCCEED();
    }

}
