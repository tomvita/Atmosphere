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
#include "breeze_proxy_service.hpp"

#define AMS_BREEZE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 100, Result, OpenSystemAppletProxy, (sf::Out<sf::SharedPointer<ams::mitm::breeze::impl::IBreezeProxyInterface>> out, u64 reserved), (out, reserved))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeMitmInterface, AMS_BREEZE_MITM_INTERFACE_INFO, 0xBEEF0001)

namespace ams::mitm::breeze {

    class BreezeMitmService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
        public:
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* Only MITM qlaunch (title 0x0100000000001000). */
                return client_info.program_id == ncm::SystemAppletId::Qlaunch;
            }
        public:
            Result OpenSystemAppletProxy(sf::Out<sf::SharedPointer<impl::IBreezeProxyInterface>> out, u64 reserved);
    };
    static_assert(impl::IsIBreezeMitmInterface<BreezeMitmService>);

}
