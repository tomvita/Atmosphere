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

namespace ams::mitm::breeze {

    /* State machine for Breeze MITM. */
    enum class BreezeState {
        /* No interception. All IPC forwarded to qlaunch. */
        Passthrough,

        /* Creating Album via ILibraryAppletCreator. Home events suppressed. */
        BreezeLaunching,

        /* Album (Breeze) has foreground, game suspended. Home events suppressed. */
        BreezeForeground,

        /* Game has foreground, Album alive in background. Home events suppressed. */
        GameForeground,

        /* Album exited. Processing flags, transitioning back. */
        Cleanup,
    };

    /* Flag file paths on SD card (plain paths for fsFsGetEntryType etc.).
     * Must match the paths Breeze NRO uses (sdmc:/config/SwitchU/...). */
    constexpr const char BreezeActiveFlag[]   = "/config/SwitchU/breeze_active";
    constexpr const char BreezeGotoGameFlag[] = "/config/SwitchU/breeze_goto_game";

    /* Flag file helpers. */
    bool IsBreezeActiveFlag();
    bool IsBreezeGotoGameFlag();
    void RemoveBreezeActiveFlag();
    void RemoveBreezeGotoGameFlag();
    void CleanupStaleFlagsOnBoot();

    /* Global state accessors. */
    BreezeState GetState();
    void SetState(BreezeState state);

    bool IsAlbumActive();
    void SetAlbumActive(bool active);

    bool IsInterceptingHome();

    /* Album accessor service handle — held by MITM to manage Album lifecycle. */
    Service *GetAlbumAccessor();
    void SetAlbumAccessor(Service *srv);
    void CloseAlbumAccessor();

    /* Application accessor service handle — captured from qlaunch's IApplicationCreator.
     * Stored as a copy of the Service struct. */
    Service *GetAppAccessor();
    void SetAppAccessor(const Service *srv);
    void ClearAppAccessor();

    /* Album state changed event. */
    os::NativeHandle GetAlbumStateChangedEventHandle();
    void SetAlbumStateChangedEventHandle(os::NativeHandle handle);

    /* Real service handles captured from qlaunch's proxy session.
     * These are copies of the domain sub-objects, stored in global state
     * so the MITM can use them independently (e.g., launching Album,
     * calling RequestToGetForeground). */
    Service *GetRealHomeMenuFunctions();
    void SetRealHomeMenuFunctions(const Service *srv);

    Service *GetRealLibraryAppletCreator();
    void SetRealLibraryAppletCreator(const Service *srv);

    /* Mutex for thread-safe state access. */
    os::Mutex &GetStateMutex();

}
