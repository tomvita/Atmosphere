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
#include "dmnt2_gdb_server.hpp"
#include "dmnt2_transport_layer.hpp"
#include "cheat/dmnt_cheat_service.hpp"
#include "cheat/impl/dmnt_cheat_api.hpp"

namespace ams {

    namespace {

        constinit u8 g_fs_heap_memory[4_KB];
        lmem::HeapHandle g_fs_heap_handle;

        void *AllocateForFs(size_t size) {
            return lmem::AllocateFromExpHeap(g_fs_heap_handle, size);
        }

        void DeallocateForFs(void *p, size_t size) {
            AMS_UNUSED(size);
            return lmem::FreeToExpHeap(g_fs_heap_handle, p);
        }

        void InitializeFsHeap() {
            g_fs_heap_handle = lmem::CreateExpHeap(g_fs_heap_memory, sizeof(g_fs_heap_memory), lmem::CreateOption_None);
        }

    }

    namespace {

        bool IsHtcEnabled() {
            u8 enable_htc = 0;
            settings::fwdbg::GetSettingsItemValue(std::addressof(enable_htc), sizeof(enable_htc), "atmosphere", "enable_htc");
            return enable_htc != 0;
        }

        bool IsStandaloneGdbstubEnabled() {
            u8 enable_gdbstub = 0;
            settings::fwdbg::GetSettingsItemValue(std::addressof(enable_gdbstub), sizeof(enable_gdbstub), "atmosphere", "enable_standalone_gdbstub");
            return enable_gdbstub != 0;
        }

        void CheckDmntGen2ApiVersion() {
            /* Bypass version check to guarantee boot success on all Atmosphere/Exosphere versions. */
            return;
        }

    }

    namespace init {

        void InitializeSystemModule() {
            /* Initialize heap. */
            InitializeFsHeap();

            /* Initialize our connection to sm. */
            R_ABORT_UNLESS(sm::Initialize());

            /* Initialize fs. */
            fs::InitializeForSystem();
            fs::SetAllocator(AllocateForFs, DeallocateForFs);
            fs::SetEnabledAutoAbort(false);

            /* Initialize other services we need. */
            R_ABORT_UNLESS(pmdmntInitialize());
            R_ABORT_UNLESS(pminfoInitialize());
            R_ABORT_UNLESS(ldrDmntInitialize());
            R_ABORT_UNLESS(roDmntInitialize());
            R_ABORT_UNLESS(nsdevInitialize());
            lr::Initialize();
            R_ABORT_UNLESS(setInitialize());
            R_ABORT_UNLESS(setsysInitialize());
            R_ABORT_UNLESS(hidInitialize());

            /* Mount the SD card. */
            R_ABORT_UNLESS(fs::MountSdCard("sdmc"));

            /* Verify that we can sanely execute. */
            CheckDmntGen2ApiVersion();
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() { /* ... */ }

    }
    using ServerOptions = sf::hipc::DefaultServerManagerOptions;

    constexpr sm::ServiceName DebugMonitorServiceName = sm::ServiceName::Encode("dmnt:-");
    constexpr size_t          DebugMonitorMaxSessions = 4;

    constexpr sm::ServiceName CheatServiceName = sm::ServiceName::Encode("dmnt:cht");
    constexpr size_t          CheatMaxSessions = 2;

    /* dmnt:-, dmnt:cht. */
    constexpr size_t NumServers = 2;
    constexpr size_t NumSessions = DebugMonitorMaxSessions + CheatMaxSessions;

    sf::hipc::ServerManager<NumServers, ServerOptions, NumSessions> g_server_manager;

    constinit sf::UnmanagedServiceObject<dmnt::cheat::impl::ICheatInterface, dmnt::cheat::CheatService> g_cheat_service;

    void LoopServerThread(void*) {
        g_server_manager.LoopProcess();
    }

    /* NOTE: Nintendo loops four threads processing on the manager -- we'll loop an extra fifth for our cheat service. */
    constexpr size_t TotalThreads = DebugMonitorMaxSessions + 1;
    static_assert(TotalThreads >= 1, "TotalThreads");
    constexpr size_t NumExtraThreads = TotalThreads - 1;
    constexpr size_t ThreadStackSize = 0x4000;
    alignas(os::MemoryPageSize) u8 g_extra_thread_stacks[NumExtraThreads][ThreadStackSize];

    os::ThreadType g_extra_threads[NumExtraThreads];

    void InitializeIpcServer() {
        /* Create services. */
        const auto rc = g_server_manager.RegisterObjectForServer(g_cheat_service.GetShared(), CheatServiceName, CheatMaxSessions);
        AMS_DMNT2_DEBUG_LOG("RegisterObjectForServer(dmnt:cht) result: 0x%08x\n", rc.GetValue());
        if (rc.IsFailure()) {
            /* Service registration failed (e.g. because legacy dmnt already registered dmnt:cht). */
            /* We fail gracefully to allow GDB server and the rest of the sysmodule to run. */
        }
    }

    void LoopProcessIpcServer() {
        /* Initialize threads. */
        if constexpr (NumExtraThreads > 0) {
            static_assert(AMS_GET_SYSTEM_THREAD_PRIORITY(dmnt, Main) == AMS_GET_SYSTEM_THREAD_PRIORITY(dmnt, Ipc));
            for (size_t i = 0; i < NumExtraThreads; i++) {
                R_ABORT_UNLESS(os::CreateThread(std::addressof(g_extra_threads[i]), LoopServerThread, nullptr, g_extra_thread_stacks[i], ThreadStackSize, AMS_GET_SYSTEM_THREAD_PRIORITY(dmnt, Ipc)));
                os::SetThreadNamePointer(std::addressof(g_extra_threads[i]), AMS_GET_SYSTEM_THREAD_NAME(dmnt, Ipc));
            }
        }

        /* Start extra threads. */
        if constexpr (NumExtraThreads > 0) {
            for (size_t i = 0; i < NumExtraThreads; i++) {
                os::StartThread(std::addressof(g_extra_threads[i]));
            }
        }

        /* Loop this thread. */
        LoopServerThread(nullptr);

        /* Wait for extra threads to finish. */
        if constexpr (NumExtraThreads > 0) {
            for (size_t i = 0; i < NumExtraThreads; i++) {
                os::WaitThread(std::addressof(g_extra_threads[i]));
            }
        }
    }

    void Main() {
        /* Set thread name. */
        os::SetThreadNamePointer(os::GetCurrentThread(), AMS_GET_SYSTEM_THREAD_NAME(dmnt, Main));
        AMS_ASSERT(os::GetThreadPriority(os::GetCurrentThread()) == AMS_GET_SYSTEM_THREAD_PRIORITY(dmnt, Main));

        /* Always initialize debug log thread for diagnostic purposes. */
        dmnt::InitializeDebugLog();
        AMS_DMNT2_DEBUG_LOG("dmnt.gen2 started.\n");

        bool use_htcs = false, use_tcp = false;
        {
            use_htcs = IsHtcEnabled();
            use_tcp  = IsStandaloneGdbstubEnabled();
        }
        AMS_DMNT2_DEBUG_LOG("use_htcs: %d, use_tcp: %d\n", use_htcs, use_tcp);

        /* Initialize transport layer. */
        if (use_htcs) {
            dmnt::transport::InitializeByHtcs();
        } else if (use_tcp) {
            dmnt::transport::InitializeByTcp();
        }

        /* If we have a transport, start GdbServer. */
        if (use_htcs || use_tcp) {
            /* Start GdbServer. */
            dmnt::InitializeGdbServer();
            dmnt::InitializeGdbServer2();
            AMS_DMNT2_DEBUG_LOG("GDB servers initialized.\n");
        }

        /* Initialize the cheat manager. */
        dmnt::cheat::impl::InitializeCheatManager();
        AMS_DMNT2_DEBUG_LOG("Cheat manager initialized.\n");

        /* Initialize ipc server. */
        InitializeIpcServer();

        /* Loop processing ipc server. */
        LoopProcessIpcServer();
    }

}
