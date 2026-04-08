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
#include "breeze_proxy_service.hpp"
#include "breeze_am_shim.h"
#include "breeze_state.hpp"

namespace ams::mitm::breeze {

    Result BreezeProxyService::GetCommonStateGetter(sf::Out<sf::SharedPointer<impl::IBreezeCommonStateInterface>> out) {
        /* Forward to real proxy to get the real ICommonStateGetter. */
        Service real_csg;
        R_TRY(breezeAmShimGetCommonStateGetter(&m_real_proxy, &real_csg));

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(&real_csg)};

        /* Wrap in our Breeze-aware service. */
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IBreezeCommonStateInterface, BreezeCommonStateService>(real_csg), target_object_id);
        R_SUCCEED();
    }

    Result BreezeProxyService::GetLibraryAppletCreator(sf::Out<sf::SharedPointer<impl::IBreezeLaCreatorInterface>> out) {
        /* Forward to real proxy to get the real ILibraryAppletCreator. */
        Service real_lac;
        R_TRY(breezeAmShimGetLibraryAppletCreator(&m_real_proxy, &real_lac));

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(&real_lac)};

        /* Store a copy of the real LA creator in global state so we can
         * create Album ourselves later.
         * IMPORTANT: This copy shares the same domain object ID as the
         * one owned by BreezeLaCreatorService. The wrapper must outlive
         * any use of the global copy (it does, since both live for the
         * entire qlaunch session). */
        SetRealLibraryAppletCreator(&real_lac);

        /* Wrap in our Breeze-aware service. */
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IBreezeLaCreatorInterface, BreezeLaCreatorService>(real_lac), target_object_id);
        R_SUCCEED();
    }

    Result BreezeProxyService::GetHomeMenuFunctions(sf::Out<sf::SharedPointer<impl::IBreezeHomeMenuInterface>> out) {
        /* Forward to real proxy to get the real IHomeMenuFunctions. */
        Service real_hmf;
        R_TRY(breezeAmShimGetHomeMenuFunctions(&m_real_proxy, &real_hmf));

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(&real_hmf)};

        /* Store a copy of the real HMF in global state so we can
         * call RequestToGetForeground ourselves.
         * Same lifetime note as SetRealLibraryAppletCreator above. */
        SetRealHomeMenuFunctions(&real_hmf);

        /* Wrap in our Breeze-aware service. */
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IBreezeHomeMenuInterface, BreezeHomeMenuService>(real_hmf), target_object_id);
        R_SUCCEED();
    }

    Result BreezeProxyService::GetApplicationCreator(sf::Out<sf::SharedPointer<impl::IBreezeAppCreatorInterface>> out) {
        /* Forward to real proxy to get the real IApplicationCreator. */
        Service real_ac;
        R_TRY(breezeAmShimGetApplicationCreator(&m_real_proxy, &real_ac));

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(&real_ac)};

        /* Wrap in our Breeze-aware service that intercepts
         * PopLaunchRequestedApplication to capture the IApplicationAccessor. */
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IBreezeAppCreatorInterface, BreezeAppCreatorService>(real_ac), target_object_id);
        R_SUCCEED();
    }

}
