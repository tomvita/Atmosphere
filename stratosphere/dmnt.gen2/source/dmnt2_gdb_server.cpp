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
#include "dmnt2_transport_layer.hpp"
#include "dmnt2_gdb_server.hpp"
#include "dmnt2_gdb_server_impl.hpp"

namespace ams::dmnt {
    /* See dmnt2_gdb_server_impl.cpp for definitions. */
    extern os::Event g_gen2_request_event;
    extern u8        g_gen2_server_on;
// bool gen2_loop(u32 count);
    namespace {

        constexpr size_t ServerThreadStackSize = util::AlignUp(4 * GdbPacketBufferSize + os::MemoryPageSize, os::ThreadStackAlignment);
        constexpr size_t ServerThreadStackSize2 = util::AlignUp(os::MemoryPageSize, os::ThreadStackAlignment);

        alignas(os::ThreadStackAlignment) constinit u8 g_server_thread_stack[ServerThreadStackSize];
        alignas(os::ThreadStackAlignment) constinit u8 g_events_thread_stack[util::AlignUp(2 * GdbPacketBufferSize + os::MemoryPageSize, os::ThreadStackAlignment)];
        alignas(os::ThreadStackAlignment) constinit u8 g_server_thread_stack2[ServerThreadStackSize2];

        constinit os::ThreadType g_server_thread, g_server_thread2;

        constinit util::TypedStorage<GdbServerImpl> g_gdb_server;
        std::atomic<bool> g_gdb_server_constructed = false;
        constinit os::SdkMutex g_gdb_server_lock;

        /* Wakes when a dmnt:cht client signals g_gen2_request_event. We
         * still use a periodic timeout so the `attached / gen2loop_on`
         * mirror fields in m_watch_data get refreshed even when no
         * client is poking us. See design doc Opt 1.
         */
        constexpr inline TimeSpan Gen2LoopRefreshInterval = TimeSpan::FromMilliSeconds(500);

        void GdbServerThreadFunction2(void *) {
            while (true) {
                /* Block until a client signals a new request, or until the
                 * refresh interval elapses so we can keep the attached/
                 * gen2loop_on mirror fields current. Auto-clear event,
                 * so multiple signals between two ticks collapse to one
                 * iteration. */
                g_gen2_request_event.TimedWait(Gen2LoopRefreshInterval);
                {
                    std::scoped_lock lk(g_gdb_server_lock);
                    if (g_gdb_server_constructed.load(std::memory_order_acquire)) {
                        util::GetReference(g_gdb_server).gen2_loop();
                    }
                }
            };
        }
        void GdbServerThreadFunction(void *) {
            /* Loop forever, servicing our gdb server. */
            while (true) {
                /* Get a socket. */
                int fd;
                while ((fd = transport::Socket()) == -1) {
                    os::SleepThread(TimeSpan::FromSeconds(1));
                }

                /* Ensure we cleanup the socket when we're done with it. */
                ON_SCOPE_EXIT {
                    transport::Close(fd);
                    os::SleepThread(TimeSpan::FromSeconds(1));
                };

                /* Bind. */
                if (transport::Bind(fd, transport::PortName_GdbServer) == -1) {
                    continue;
                }

                /* Listen on our port. */
                bool accept_failed = false;
                while (transport::Listen(fd, 0) == 0) {
                    /* Continue accepting clients, so long as we can. */
                    int client_fd;
                    while (true) {
                        /* Construct a "placeholder" GdbServerImpl with a
                         * sentinel fd before calling Accept.
                         *
                         * Why this exists: Gen2Attach() (called from
                         * gen2_loop when a dmnt:cht client requests an
                         * ATTACH) signals g_event_request_cv. Only a
                         * live DebugEventsThread can receive that
                         * signal; the events thread is owned by
                         * GdbServerImpl. Without this placeholder, no
                         * events thread exists until a real GDB client
                         * connects, so a cold-boot ATTACH from Breeze
                         * silently times out.
                         *
                         * The sentinel TransportSession is a no-op
                         * (Recv on the sentinel fd fails immediately
                         * and the receive thread exits). All we want
                         * is the DebugEventsThread.
                         *
                         * Previously this used fd=500 which is a real
                         * fd value and could collide with a future
                         * socket allocation. We use -1 as an
                         * unambiguous sentinel. The receive thread
                         * will see Recv fail with EBADF on most
                         * transports and exit cleanly.
                         *
                         * See gen2fork_design_and_review.md Bug 5 /
                         * Addendum on the ATTACH regression.
                         */
                        constexpr int SentinelFd = -1;
                        {
                            std::scoped_lock lk(g_gdb_server_lock);
                            util::ConstructAt(g_gdb_server, SentinelFd, g_events_thread_stack, sizeof(g_events_thread_stack));
                            g_gdb_server_constructed.store(true, std::memory_order_release);
                        }
                        g_gen2_server_on = 2;

                        /* Try to accept a client. */
                        const int temp_fd = transport::Accept(fd);

                        {
                            std::scoped_lock lk(g_gdb_server_lock);
                            g_gdb_server_constructed.store(false, std::memory_order_release);
                            g_gen2_server_on = 0;
                            util::DestroyAt(g_gdb_server);
                        }

                        if (temp_fd < 0) {
                            accept_failed = true;
                            break;
                        }
                        client_fd = temp_fd;
                        {
                            /* Create gdb server for the socket. */
                            {
                                std::scoped_lock lk(g_gdb_server_lock);
                                util::ConstructAt(g_gdb_server, client_fd, g_events_thread_stack, sizeof(g_events_thread_stack));
                                g_gdb_server_constructed.store(true, std::memory_order_release);
                            }

                            ON_SCOPE_EXIT {
                                std::scoped_lock lk(g_gdb_server_lock);
                                g_gdb_server_constructed.store(false, std::memory_order_release);
                                util::DestroyAt(g_gdb_server);
                            };

                            /* Process for the server. */
                            util::GetReference(g_gdb_server).LoopProcess();
                        }

                        /* Close the client socket. */
                        transport::Close(client_fd);
                    }
                    if (accept_failed) {
                        break;
                    }
                }
            }
        }

    }

    void InitializeGdbServer() {
        /* Create and start gdb server threads. */
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_server_thread), GdbServerThreadFunction, nullptr, g_server_thread_stack, sizeof(g_server_thread_stack), os::HighestThreadPriority - 1));
        os::StartThread(std::addressof(g_server_thread));
    }

    void InitializeGdbServer2() {
        /* Create and start gdb server threads. */
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_server_thread2), GdbServerThreadFunction2, nullptr, g_server_thread_stack2, sizeof(g_server_thread_stack2), os::HighestThreadPriority - 1));
        os::StartThread(std::addressof(g_server_thread2));
    }

    os::NativeHandle GetGdbDebugHandle() {
        std::scoped_lock lk(g_gdb_server_lock);
        if (g_gdb_server_constructed.load(std::memory_order_acquire)) {
            auto &server = util::GetReference(g_gdb_server);
            if (server.HasDebugProcess()) {
                return server.GetDebugHandle();
            }
        }
        return os::InvalidNativeHandle;
    }
}
