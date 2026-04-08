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
#include "breeze_app_accessor_service.hpp"

/*
 * Wraps IApplicationCreator for qlaunch.
 *
 * Intercepts:
 *   cmd 1: PopLaunchRequestedApplication — calls real service, captures the
 *          returned IApplicationAccessor in global state, wraps in a
 *          transparent pass-through service, and returns to qlaunch.
 *
 * All other commands forward unchanged (unhandled cmd IDs are forwarded
 * automatically by the SF MITM framework via ForwardRequest).
 */

#define AMS_BREEZE_APP_CREATOR_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, PopLaunchRequestedApplication, (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeAppAccessorInterface>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeAppCreatorInterface, AMS_BREEZE_APP_CREATOR_INTERFACE_INFO, 0xBEEF0006)

namespace ams::mitm::breeze {

    class BreezeAppCreatorService {
        private:
            Service m_real_ac;  /* Real IApplicationCreator domain sub-object. */
        public:
            BreezeAppCreatorService(Service real_ac) : m_real_ac(real_ac) { }

            ~BreezeAppCreatorService() {
                serviceClose(&m_real_ac);
            }

        public:
            Result PopLaunchRequestedApplication(sf::Out<sf::SharedPointer<impl::IBreezeAppAccessorInterface>> out);
    };
    static_assert(impl::IsIBreezeAppCreatorInterface<BreezeAppCreatorService>);

}
