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
#include "breeze_la_creator_service.hpp"
#include "breeze_state.hpp"

namespace ams::mitm::breeze {

    Result BreezeLaCreatorService::CreateLibraryApplet(u32 applet_id, u32 mode) {
        AMS_UNUSED(applet_id, mode);
        /*
         * Intercept CreateLibraryApplet from qlaunch.
         *
         * When Breeze is active and we own an Album session, block qlaunch
         * from creating any library applet (only one can exist at a time).
         * This prevents qlaunch from accidentally destroying our Album.
         *
         * When Breeze is not active or Album is not running, forward normally.
         */
        if (IsAlbumActive()) {
            /* Return an error to indicate the library applet slot is busy.
             * AM error 2-128-2 (ResultBusy). */
            R_RETURN(MAKERESULT(128, 2));
        }

        /* Forward to real service. */
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

}
