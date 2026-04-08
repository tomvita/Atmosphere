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
#include "breeze_monitor.hpp"
#include "breeze_state.hpp"
#include "breeze_am_shim.h"

namespace ams::mitm::breeze {

    namespace {

        /* Intra-process event to wake the monitor when Album is launched. */
        constinit os::EventType g_album_launched_event;
        constinit bool g_event_initialized = false;

        /* Monitor thread. */
        constexpr size_t MonitorThreadStackSize = 0x2000;
        constinit os::ThreadType g_monitor_thread;
        alignas(os::ThreadStackAlignment) constinit u8 g_monitor_thread_stack[MonitorThreadStackSize];

        /*
         * Process Album exit: check flag files and transition state.
         *
         * Called when the album state changed event fires, indicating
         * Album has exited or changed state.
         */
        void OnAlbumStateChanged() {
            std::scoped_lock lk(GetStateMutex());

            Service *accessor = GetAlbumAccessor();
            if (accessor == nullptr) {
                return;
            }

            /* Check if Album actually exited by calling GetResult.
             * GetResult succeeds (returns R_SUCCEEDED) only after the applet
             * has exited. If the applet is still running, GetResult returns
             * an error (e.g., ResultAppletNotExited). */
            Result rc = breezeAmShimAccessorGetResult(accessor);
            if (R_FAILED(rc)) {
                /* Album hasn't exited yet. The state changed event fired for
                 * a non-exit reason (e.g., interactive dialog, state change).
                 * Ignore and keep waiting. */
                return;
            }

            /* GetResult succeeded — Album has exited. */
            const BreezeState state = GetState();
            if (state != BreezeState::BreezeForeground &&
                state != BreezeState::GameForeground &&
                state != BreezeState::BreezeLaunching) {
                /* Not in a state where Album should be active. */
                return;
            }

            /* Album exited. Transition to Cleanup. */
            SetState(BreezeState::Cleanup);

            /* Check flags to determine what to do. */
            const bool goto_game = IsBreezeGotoGameFlag();
            const bool still_active = IsBreezeActiveFlag();

            /* Clean up goto_game flag regardless. */
            if (goto_game) {
                RemoveBreezeGotoGameFlag();
            }

            /* Close the Album accessor. */
            CloseAlbumAccessor();

            if (goto_game && still_active) {
                /*
                 * Breeze "Goto Game" was pressed. Breeze wants to stay armed
                 * (breeze_active flag present) but hand foreground to the game.
                 *
                 * Resume the game and go back to Passthrough (waiting for
                 * next Home press to re-launch Album).
                 */
                Service *app = GetAppAccessor();
                if (app != nullptr) {
                    breezeAmShimAppRequestForeground(app);
                }

                SetState(BreezeState::Passthrough);
            } else {
                /*
                 * Breeze "Exit" was pressed (breeze_active removed) or Album
                 * exited abnormally. Clean up and return to normal operation.
                 */
                if (still_active) {
                    /* Abnormal exit with flag still present — clean up. */
                    RemoveBreezeActiveFlag();
                }

                SetState(BreezeState::Passthrough);
            }
        }

        void MonitorThreadFunc(void *) {
            while (true) {
                /* Wait for notification that Album was launched. */
                os::WaitEvent(std::addressof(g_album_launched_event));
                os::ClearEvent(std::addressof(g_album_launched_event));

                /* Get the album state changed event handle. */
                os::NativeHandle handle = GetAlbumStateChangedEventHandle();
                if (handle == os::InvalidNativeHandle) {
                    continue;
                }

                /* Attach the native handle to a system event for waiting. */
                os::SystemEventType album_event;
                os::AttachReadableHandleToSystemEvent(
                    std::addressof(album_event),
                    handle,
                    false,  /* Don't manage (auto-close) the handle — state owns it. */
                    os::EventClearMode_ManualClear
                );

                /*
                 * Wait for the album state changed event.
                 * This blocks until Album's state changes (e.g., exit).
                 *
                 * We loop because the event may fire for state changes
                 * other than exit (e.g., interactive state transitions).
                 */
                while (true) {
                    os::WaitSystemEvent(std::addressof(album_event));
                    os::ClearSystemEvent(std::addressof(album_event));

                    /* Process the state change. */
                    OnAlbumStateChanged();

                    /* If we've cleaned up (no longer in an Album-active state),
                     * break out and wait for the next Album launch. */
                    const BreezeState state = GetState();
                    if (state == BreezeState::Passthrough || state == BreezeState::Cleanup) {
                        break;
                    }
                }

                /* Detach the event (doesn't close the handle since manage=false). */
                os::DestroySystemEvent(std::addressof(album_event));
            }
        }

    }

    void StartAlbumMonitor() {
        /* Initialize the launch notification event. */
        os::InitializeEvent(std::addressof(g_album_launched_event), false, os::EventClearMode_ManualClear);
        g_event_initialized = true;

        /* Create and start the monitor thread. */
        R_ABORT_UNLESS(os::CreateThread(
            std::addressof(g_monitor_thread),
            MonitorThreadFunc,
            nullptr,
            g_monitor_thread_stack,
            sizeof(g_monitor_thread_stack),
            /* Use a lower priority than the MITM server thread (21).
             * 22 = slightly lower priority, fine for a monitor. */
            22
        ));
        os::SetThreadNamePointer(std::addressof(g_monitor_thread), "breeze.AlbumMonitor");
        os::StartThread(std::addressof(g_monitor_thread));
    }

    void NotifyAlbumLaunched() {
        if (g_event_initialized) {
            os::SignalEvent(std::addressof(g_album_launched_event));
        }
    }

}
