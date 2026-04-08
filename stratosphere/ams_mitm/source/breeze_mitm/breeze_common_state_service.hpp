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
 * Wraps ICommonStateGetter for qlaunch.
 *
 * Intercepts:
 *   cmd 1: ReceiveMessage — filters AppletMessage 20 (Home) when Breeze active.
 *          When Home is suppressed, also drains the SAMS general channel to
 *          consume the paired Home SAMS message, then triggers Breeze logic.
 *
 * All other commands forward unchanged via ResultShouldForwardToSession.
 */

#define AMS_BREEZE_COMMON_STATE_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, ReceiveMessage, (sf::Out<u32> out_msg), (out_msg))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::breeze::impl, IBreezeCommonStateInterface, AMS_BREEZE_COMMON_STATE_INTERFACE_INFO, 0xBEEF0004)

namespace ams::mitm::breeze {

    /* AppletMessage value for Home button short press. */
    constexpr u32 AppletMessage_DetectShortPressingHomeButton = 20;

    /* SAMS (General Channel) message types. */
    constexpr u32 SamsMessage_HomeButton = 2;

    /* SAMS header magic. */
    constexpr u32 SamsMagic = 0x534D4153; /* "SAMS" */

    /* SAMS header structure (from general channel IStorage). */
    struct SamsHeader {
        u32 magic;     /* 0x534D4153 */
        u32 version;   /* 1 */
        u32 message;   /* Message type */
        u32 reserved;  /* Padding */
    };
    static_assert(sizeof(SamsHeader) == 0x10);

    class BreezeCommonStateService {
        private:
            Service m_real_csg;  /* Real ICommonStateGetter domain sub-object. */
        public:
            BreezeCommonStateService(Service real_csg) : m_real_csg(real_csg) { }

            ~BreezeCommonStateService() {
                serviceClose(&m_real_csg);
            }
        private:
            /* Drain the SAMS general channel to consume Home messages. */
            void DrainSamsHomeMessages();

            /* Handle intercepted Home press — launch/toggle Breeze. */
            void OnHomePressIntercepted();

        public:
            Result ReceiveMessage(sf::Out<u32> out_msg);
    };
    static_assert(impl::IsIBreezeCommonStateInterface<BreezeCommonStateService>);

}
