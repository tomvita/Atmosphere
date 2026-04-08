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
#include "breeze_home_menu_service.hpp"
#include "breeze_common_state_service.hpp"
#include "breeze_la_creator_service.hpp"
#include "breeze_app_creator_service.hpp"

/*
 * Wraps ISystemAppletProxy (returned by OpenSystemAppletProxy cmd 100).
 * Intercepts sub-interface getters (cmds 0, 11, 20, 22) to wrap them with
 * our Breeze-aware service objects. All other commands forward unchanged.
 */

#define AMS_BREEZE_PROXY_INTERFACE_INFO(C, H)                                                                                                                                                     \
    AMS_SF_METHOD_INFO(C, H,  0, Result, GetCommonStateGetter,    (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeCommonStateInterface>> out),     (out))                              \
    AMS_SF_METHOD_INFO(C, H, 11, Result, GetLibraryAppletCreator, (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeLaCreatorInterface>> out),       (out))                              \
    AMS_SF_METHOD_INFO(C, H, 20, Result, GetHomeMenuFunctions,    (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeHomeMenuInterface>> out),        (out))                              \
    AMS_SF_METHOD_INFO(C, H, 22, Result, GetApplicationCreator,   (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeAppCreatorInterface>> out),      (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeProxyInterface, AMS_BREEZE_PROXY_INTERFACE_INFO, 0xBEEF0002)

namespace ams::mitm::breeze {

    class BreezeProxyService {
        private:
            sm::MitmProcessInfo m_client_info;
            Service m_real_proxy;  /* The real ISystemAppletProxy domain sub-object. */
        public:
            BreezeProxyService(const sm::MitmProcessInfo &cl, Service real_proxy)
                : m_client_info(cl), m_real_proxy(real_proxy) { }

            ~BreezeProxyService() {
                serviceClose(&m_real_proxy);
            }
        public:
            Result GetCommonStateGetter(sf::Out<sf::SharedPointer<impl::IBreezeCommonStateInterface>> out);
            Result GetLibraryAppletCreator(sf::Out<sf::SharedPointer<impl::IBreezeLaCreatorInterface>> out);
            Result GetHomeMenuFunctions(sf::Out<sf::SharedPointer<impl::IBreezeHomeMenuInterface>> out);
            Result GetApplicationCreator(sf::Out<sf::SharedPointer<impl::IBreezeAppCreatorInterface>> out);
    };
    static_assert(impl::IsIBreezeProxyInterface<BreezeProxyService>);

}
