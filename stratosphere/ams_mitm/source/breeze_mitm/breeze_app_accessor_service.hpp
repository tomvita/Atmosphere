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
 * Transparent pass-through wrapper for IApplicationAccessor.
 *
 * Exists solely so we can intercept PopLaunchRequestedApplication
 * on IApplicationCreator (which returns an IApplicationAccessor),
 * capture the accessor handle for our own use, and still return
 * a valid SF object to qlaunch.
 *
 * All commands forward unchanged via ResultShouldForwardToSession.
 * We intercept cmd 101 (RequestForApplicationToGetForeground)
 * with a no-op so that when WE call it on the stored copy, it's
 * fine, and when qlaunch calls it through here, it also forwards.
 *
 * Actually, this is purely transparent — we don't need to intercept
 * any command on IApplicationAccessor. We just need it to exist as
 * an SF interface so the return type of PopLaunchRequestedApplication
 * is valid in the framework.
 *
 * We use a dummy command ID (65535) that forwards to session, making
 * the entire interface transparent.
 */

/* Dummy interface — no intercepted commands.
 * We define a single dummy method that always returns ForwardToSession.
 * The SF framework will forward all unhandled command IDs automatically
 * when the service is part of a domain object returned by another
 * intercepted command. */
#define AMS_BREEZE_APP_ACCESSOR_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 101, Result, RequestForApplicationToGetForeground, (), ())

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeAppAccessorInterface, AMS_BREEZE_APP_ACCESSOR_INTERFACE_INFO, 0xBEEF0007)

namespace ams::mitm::breeze {

    class BreezeAppAccessorService {
        private:
            Service m_real_accessor;  /* Real IApplicationAccessor domain sub-object. */
        public:
            BreezeAppAccessorService(Service real_accessor)
                : m_real_accessor(real_accessor) { }

            ~BreezeAppAccessorService() {
                serviceClose(&m_real_accessor);
            }

        public:
            Result RequestForApplicationToGetForeground() {
                /* Forward to real service. */
                return breezeAmShimAppRequestForeground(&m_real_accessor);
            }
    };
    static_assert(impl::IsIBreezeAppAccessorInterface<BreezeAppAccessorService>);

}
