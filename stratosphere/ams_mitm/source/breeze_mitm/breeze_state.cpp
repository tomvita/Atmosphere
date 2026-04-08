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
#include "breeze_state.hpp"
#include "../amsmitm_fs_utils.hpp"

namespace ams::mitm::breeze {

    namespace {

        constinit os::SdkMutex g_state_mutex;
        constinit BreezeState g_state = BreezeState::Passthrough;
        constinit bool g_album_active = false;

        /* Service handles — not owned by us for real* handles, owned for album/app accessors. */
        constinit Service g_album_accessor = {};
        constinit bool g_album_accessor_valid = false;

        constinit Service g_app_accessor = {};
        constinit bool g_app_accessor_valid = false;

        constinit os::NativeHandle g_album_state_event_handle = os::InvalidNativeHandle;

        /* Copies of qlaunch's real service domain sub-objects.
         * We store copies (not pointers) because the originals live on
         * the stack in BreezeProxyService methods and go out of scope. */
        constinit Service g_real_home_menu_functions = {};
        constinit bool g_real_hmf_valid = false;

        constinit Service g_real_la_creator = {};
        constinit bool g_real_lac_valid = false;

    }

    /* Flag file helpers — use raw SD filesystem helpers (plain paths, no mount prefix). */
    bool IsBreezeActiveFlag() {
        return mitm::fs::HasSdFile(BreezeActiveFlag);
    }

    bool IsBreezeGotoGameFlag() {
        return mitm::fs::HasSdFile(BreezeGotoGameFlag);
    }

    void RemoveBreezeActiveFlag() {
        mitm::fs::DeleteSdFile(BreezeActiveFlag);
    }

    void RemoveBreezeGotoGameFlag() {
        mitm::fs::DeleteSdFile(BreezeGotoGameFlag);
    }

    void CleanupStaleFlagsOnBoot() {
        /* Ensure directory hierarchy exists. */
        mitm::fs::CreateSdDirectory("/config");
        mitm::fs::CreateSdDirectory("/config/SwitchU");

        /* Remove stale flags from previous session. */
        RemoveBreezeActiveFlag();
        RemoveBreezeGotoGameFlag();
    }

    /* Global state accessors. */
    BreezeState GetState() {
        return g_state;
    }

    void SetState(BreezeState state) {
        g_state = state;
    }

    bool IsAlbumActive() {
        return g_album_active;
    }

    void SetAlbumActive(bool active) {
        g_album_active = active;
    }

    bool IsInterceptingHome() {
        const auto state = GetState();
        return state != BreezeState::Passthrough;
    }

    /* Album accessor. */
    Service *GetAlbumAccessor() {
        return g_album_accessor_valid ? &g_album_accessor : nullptr;
    }

    void SetAlbumAccessor(Service *srv) {
        if (srv != nullptr) {
            g_album_accessor = *srv;
            g_album_accessor_valid = true;
        }
    }

    void CloseAlbumAccessor() {
        if (g_album_accessor_valid) {
            serviceClose(&g_album_accessor);
            g_album_accessor = {};
            g_album_accessor_valid = false;
        }
        g_album_active = false;
        g_album_state_event_handle = os::InvalidNativeHandle;
    }

    /* Application accessor — stored as a copy. */
    Service *GetAppAccessor() {
        return g_app_accessor_valid ? &g_app_accessor : nullptr;
    }

    void SetAppAccessor(const Service *srv) {
        if (srv != nullptr) {
            g_app_accessor = *srv;
            g_app_accessor_valid = true;
        }
    }

    void ClearAppAccessor() {
        g_app_accessor = {};
        g_app_accessor_valid = false;
    }

    /* Album state changed event handle. */
    os::NativeHandle GetAlbumStateChangedEventHandle() {
        return g_album_state_event_handle;
    }

    void SetAlbumStateChangedEventHandle(os::NativeHandle handle) {
        g_album_state_event_handle = handle;
    }

    /* Real service handles — stored as copies. */
    Service *GetRealHomeMenuFunctions() {
        return g_real_hmf_valid ? &g_real_home_menu_functions : nullptr;
    }

    void SetRealHomeMenuFunctions(const Service *srv) {
        if (srv != nullptr) {
            g_real_home_menu_functions = *srv;
            g_real_hmf_valid = true;
        }
    }

    Service *GetRealLibraryAppletCreator() {
        return g_real_lac_valid ? &g_real_la_creator : nullptr;
    }

    void SetRealLibraryAppletCreator(const Service *srv) {
        if (srv != nullptr) {
            g_real_la_creator = *srv;
            g_real_lac_valid = true;
        }
    }

    os::Mutex &GetStateMutex() {
        static os::Mutex s_mutex(false);
        return s_mutex;
    }

}
