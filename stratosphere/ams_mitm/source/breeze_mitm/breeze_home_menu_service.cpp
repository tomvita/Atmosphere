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
#include "breeze_home_menu_service.hpp"
#include "breeze_state.hpp"

namespace ams::mitm::breeze {

    Result BreezeHomeMenuService::RequestToGetForeground() {
        /*
         * qlaunch calls RequestToGetForeground when it wants to become the
         * active foreground applet (e.g., after receiving a Home event).
         *
         * When Breeze is managing the foreground transition, suppress this
         * to prevent qlaunch from interfering. Otherwise, forward normally.
         */
        if (IsInterceptingHome()) {
            /* Suppress — return success without forwarding. qlaunch thinks
             * it got foreground, but AM didn't actually do anything. */
            R_SUCCEED();
        }

        /* Forward to real service. */
        R_RETURN(breezeAmShimHmfRequestToGetForeground(&m_real_hmf));
    }

}
