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
#include "breeze_mitm_service.hpp"
#include "breeze_am_shim.h"

namespace ams::mitm::breeze {

    Result BreezeMitmService::OpenSystemAppletProxy(sf::Out<sf::SharedPointer<impl::IBreezeProxyInterface>> out, u64 reserved) {
        /*
         * DEBUG: Auto-forward this command to preserve qlaunch's PID and
         * process handle.  OpenSystemAppletProxy sends in_send_pid=true and
         * CUR_PROCESS_HANDLE, which would be replaced by ams_mitm's PID/handle
         * if we intercepted and re-dispatched via the C shim.
         *
         * ResultShouldForwardToSession tells the SF framework to forward the
         * original TLS buffer unchanged.
         */
        AMS_UNUSED(out, reserved);
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

}
