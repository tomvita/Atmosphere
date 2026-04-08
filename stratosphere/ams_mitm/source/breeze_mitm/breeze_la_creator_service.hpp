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
#include "breeze_am_shim.h"

/*
 * Wraps ILibraryAppletCreator for qlaunch.
 *
 * Intercepts:
 *   cmd 0: CreateLibraryApplet — blocks when MITM owns an active Album session,
 *          to prevent qlaunch from creating a conflicting library applet.
 *
 * All other commands forward unchanged.
 *
 * Note: We don't intercept CreateStorage (cmd 10) — the MITM calls it
 * directly on the real service when setting up Album.
 */

#define AMS_BREEZE_LA_CREATOR_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, CreateLibraryApplet, (u32 applet_id, u32 mode), (applet_id, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeLaCreatorInterface, AMS_BREEZE_LA_CREATOR_INTERFACE_INFO, 0xBEEF0005)

namespace ams::mitm::breeze {

    class BreezeLaCreatorService {
        private:
            Service m_real_lac;  /* Real ILibraryAppletCreator domain sub-object. */
        public:
            BreezeLaCreatorService(Service real_lac) : m_real_lac(real_lac) { }

            ~BreezeLaCreatorService() {
                serviceClose(&m_real_lac);
            }

            /* Expose for Album launch logic. */
            Service *GetRealService() { return &m_real_lac; }

        public:
            Result CreateLibraryApplet(u32 applet_id, u32 mode);
    };
    static_assert(impl::IsIBreezeLaCreatorInterface<BreezeLaCreatorService>);

}
