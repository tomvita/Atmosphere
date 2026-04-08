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
 * Wraps IHomeMenuFunctions for qlaunch.
 *
 * We do NOT intercept PopFromGeneralChannel (cmd 20) because it returns
 * a domain sub-object (IStorage) which is complex to handle in MITM.
 * Instead, when we suppress a Home AppletMessage via ICommonStateGetter,
 * we proactively drain the SAMS channel to consume the paired Home
 * SAMS message.
 *
 * What we intercept:
 *   cmd 10: RequestToGetForeground — we may need to suppress when MITM
 *           is managing the foreground transition itself.
 *
 * All other commands (including cmd 20, 21) forward unchanged.
 */

#define AMS_BREEZE_HOME_MENU_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 10, Result, RequestToGetForeground, (), ())

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeHomeMenuInterface, AMS_BREEZE_HOME_MENU_INTERFACE_INFO, 0xBEEF0003)

namespace ams::mitm::breeze {

    class BreezeHomeMenuService {
        private:
            Service m_real_hmf;  /* Real IHomeMenuFunctions domain sub-object. */
        public:
            BreezeHomeMenuService(Service real_hmf) : m_real_hmf(real_hmf) { }

            ~BreezeHomeMenuService() {
                serviceClose(&m_real_hmf);
            }

            /* Expose for breeze_state to call RequestToGetForeground on real service. */
            Service *GetRealService() { return &m_real_hmf; }

        public:
            Result RequestToGetForeground();
    };
    static_assert(impl::IsIBreezeHomeMenuInterface<BreezeHomeMenuService>);

}
