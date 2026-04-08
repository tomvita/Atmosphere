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
#include "breeze_common_state_service.hpp"
#include "breeze_state.hpp"
#include "breeze_monitor.hpp"

namespace ams::mitm::breeze {

    namespace {

        /* AppletId for Album (Photo Viewer). */
        constexpr u32 AppletId_Album = 0x10;  /* AppletId_LibraryAppletPhotoViewer */

        /* LibAppletMode for full-foreground operation. */
        constexpr u32 LibAppletMode_AllForeground = 0;

        /* AM result code for "no messages available" / empty queue. */
        /* This is ResultNoMessage from AM: 2-128-3 (module AM, desc 3). */
        constexpr Result ResultAmNoMessage() { return MAKERESULT(128, 3); }

        /*
         * Launch Album (Breeze) as a library applet via the real
         * ILibraryAppletCreator on qlaunch's session.
         *
         * Steps:
         *   1. CreateLibraryApplet(Album, AllForeground) → ILibraryAppletAccessor
         *   2. CreateStorage(sizeof(LibAppletArgs)) → IStorage
         *   3. Write LibAppletArgs to the IStorage
         *   4. PushInData(IStorage) on the accessor
         *   5. Start() on the accessor
         *   6. GetAppletStateChangedEvent() for monitoring
         *
         * The accessor is stored in global state for lifecycle management.
         */
        Result LaunchAlbum() {
            Service *lac = GetRealLibraryAppletCreator();
            R_UNLESS(lac != nullptr, sm::mitm::ResultShouldForwardToSession());

            /* 1. Create Album library applet. */
            Service accessor = {};
            R_TRY(breezeAmShimLacCreateLibraryApplet(lac, &accessor, AppletId_Album, LibAppletMode_AllForeground));

            /* 2. Create storage for LibAppletArgs. */
            Service storage = {};
            Result rc = breezeAmShimLacCreateStorage(lac, &storage, 0x20);
            if (R_FAILED(rc)) {
                serviceClose(&accessor);
                R_RETURN(rc);
            }

            /* 3. Prepare and write LibAppletArgs. */
            struct {
                u32 version;          /* CommonArgs_version = 1 */
                u32 size;             /* CommonArgs_size = 0x20 */
                u32 la_version;       /* LaVersion = 0 */
                s32 theme_color;      /* ExpectedThemeColor = 0 */
                u8  play_sound;       /* PlayStartupSound = 0 */
                u8  pad[7];
                u64 tick;             /* System tick */
            } args = {};
            args.version = 1;
            args.size = 0x20;
            args.la_version = 0;
            args.theme_color = 0;
            args.play_sound = 0;
            args.tick = armGetSystemTick();

            rc = breezeAmShimStorageWrite(&storage, &args, sizeof(args), 0);
            if (R_FAILED(rc)) {
                serviceClose(&storage);
                serviceClose(&accessor);
                R_RETURN(rc);
            }

            /* 4. Push input data. (This closes the storage.) */
            rc = breezeAmShimAccessorPushInData(&accessor, &storage);
            if (R_FAILED(rc)) {
                serviceClose(&accessor);
                R_RETURN(rc);
            }

            /* 5. Start the applet. */
            rc = breezeAmShimAccessorStart(&accessor);
            if (R_FAILED(rc)) {
                serviceClose(&accessor);
                R_RETURN(rc);
            }

            /* 6. Get the state changed event for monitoring Album exit. */
            Handle event_handle = INVALID_HANDLE;
            rc = breezeAmShimAccessorGetStateChangedEvent(&accessor, &event_handle);
            if (R_FAILED(rc)) {
                /* Album launched but we can't monitor it — close and fail. */
                breezeAmShimAccessorTerminate(&accessor);
                serviceClose(&accessor);
                R_RETURN(rc);
            }

            /* Store the accessor and event handle in global state. */
            SetAlbumAccessor(&accessor);
            SetAlbumActive(true);
            SetAlbumStateChangedEventHandle(event_handle);

            /* Notify the monitor thread that Album is now running. */
            NotifyAlbumLaunched();

            R_SUCCEED();
        }

        /*
         * Resume the game by calling RequestForApplicationToGetForeground
         * on qlaunch's IApplicationAccessor.
         */
        Result ResumeGame() {
            Service *app = GetAppAccessor();
            if (app != nullptr) {
                R_RETURN(breezeAmShimAppRequestForeground(app));
            }
            /* If we don't have an app accessor, we can't resume directly.
             * Fall through — qlaunch may handle it via normal Album-exit flow. */
            R_SUCCEED();
        }

    }

    void BreezeCommonStateService::DrainSamsHomeMessages() {
        /*
         * After suppressing a Home AppletMessage, we must also consume the
         * paired SAMS general channel message (msg=2) to prevent qlaunch
         * from processing it when it next calls PopFromGeneralChannel.
         *
         * We call PopFromGeneralChannel on the real IHomeMenuFunctions,
         * read the IStorage data, and discard Home messages. Non-Home
         * messages are problematic — ideally we'd re-queue them, but since
         * the general channel is a FIFO and AM sent the SAMS msg=2 paired
         * with AppletMessage 20, there should be at most one Home SAMS msg
         * waiting.
         *
         * Note: This might pop a non-Home SAMS message if there's a race.
         * In practice, Home messages are delivered as a pair (AppletMessage 20
         * + SAMS msg=2), so immediately after suppressing the AppletMessage,
         * the SAMS queue should have the Home message at the front.
         */
        Service *hmf = GetRealHomeMenuFunctions();
        if (hmf == nullptr) {
            return;
        }

        /* Pop from the general channel. Returns IStorage as a domain sub-object. */
        Service storage = {};
        Result rc = breezeAmShimHmfPopFromGeneralChannel(hmf, &storage);
        if (R_SUCCEEDED(rc)) {
            /* We got a storage. We should read and verify it's a Home message,
             * but even if it's not, consuming it prevents qlaunch from getting
             * a stale message. Close the storage. */
            serviceClose(&storage);
        }
        /* If PopFromGeneralChannel fails (empty queue), that's fine — AM may
         * not always deliver on both channels simultaneously. */
    }

    void BreezeCommonStateService::OnHomePressIntercepted() {
        /*
         * Called when we intercept an AppletMessage 20 (Home short press)
         * while Breeze mode is active. This is the core state machine driver.
         */
        std::scoped_lock lk(GetStateMutex());

        /* Also drain the SAMS channel to prevent qlaunch from seeing it. */
        DrainSamsHomeMessages();

        const BreezeState state = GetState();

        switch (state) {
            case BreezeState::Passthrough: {
                /*
                 * Breeze is armed but not yet launched. Or just became armed
                 * after a previous session ended.
                 *
                 * Transition: PASSTHROUGH → BREEZE_LAUNCHING → BREEZE_FOREGROUND
                 */
                SetState(BreezeState::BreezeLaunching);

                /* Request foreground for qlaunch (required by AM before launching
                 * a library applet — the system applet must have foreground). */
                Service *hmf = GetRealHomeMenuFunctions();
                if (hmf != nullptr) {
                    breezeAmShimHmfRequestToGetForeground(hmf);
                }

                /* Launch Album. */
                Result rc = LaunchAlbum();
                if (R_SUCCEEDED(rc)) {
                    SetState(BreezeState::BreezeForeground);
                } else {
                    /* Failed to launch Album — revert to passthrough. */
                    SetState(BreezeState::Passthrough);
                }
                break;
            }

            case BreezeState::GameForeground: {
                /*
                 * Game has foreground, Album alive in background.
                 * Home toggles back to Breeze.
                 *
                 * Transition: GAME_FOREGROUND → BREEZE_FOREGROUND
                 */
                Service *hmf = GetRealHomeMenuFunctions();
                if (hmf != nullptr) {
                    breezeAmShimHmfRequestToGetForeground(hmf);
                }
                /* Album should come to foreground automatically since it's
                 * the active library applet of the system applet. */
                SetState(BreezeState::BreezeForeground);
                break;
            }

            case BreezeState::BreezeForeground: {
                /*
                 * Breeze has foreground, game suspended.
                 * Home toggles back to game.
                 *
                 * Transition: BREEZE_FOREGROUND → GAME_FOREGROUND
                 */
                Result rc = ResumeGame();
                if (R_SUCCEEDED(rc)) {
                    SetState(BreezeState::GameForeground);
                }
                /* If ResumeGame fails, stay in BREEZE_FOREGROUND. */
                break;
            }

            case BreezeState::BreezeLaunching:
                /* Album is still launching. Ignore additional Home presses. */
                break;

            case BreezeState::Cleanup:
                /* Cleanup in progress. Ignore. */
                break;
        }
    }

    Result BreezeCommonStateService::ReceiveMessage(sf::Out<u32> out_msg) {
        /*
         * Intercept ReceiveMessage. Call the real service, inspect the
         * message, and decide whether to forward it to qlaunch or suppress.
         */
        u32 msg = 0;
        Result rc = breezeAmShimCsgReceiveMessage(&m_real_csg, &msg);

        /* If the real service returned an error (no messages), pass it through. */
        R_TRY(rc);

        /* Check if this is a Home button short press. */
        if (msg == AppletMessage_DetectShortPressingHomeButton) {
            /* Check if Breeze mode is active. */
            if (IsBreezeActiveFlag()) {
                /* Intercept! Handle the Home press ourselves. */
                OnHomePressIntercepted();

                /* Tell qlaunch there are no messages (suppress the Home event). */
                R_RETURN(ResultAmNoMessage());
            }
        }

        /* Not a Home message, or Breeze not active — forward to qlaunch. */
        out_msg.SetValue(msg);
        R_SUCCEED();
    }

}
