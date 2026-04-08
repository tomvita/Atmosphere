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
#include "breeze_app_creator_service.hpp"
#include "breeze_state.hpp"

namespace ams::mitm::breeze {

    Result BreezeAppCreatorService::PopLaunchRequestedApplication(sf::Out<sf::SharedPointer<impl::IBreezeAppAccessorInterface>> out) {
        /*
         * Intercept PopLaunchRequestedApplication to capture the returned
         * IApplicationAccessor. We call the real service, store a copy of
         * the accessor handle in global state (for ResumeGame to use),
         * then wrap it in a transparent pass-through service for qlaunch.
         *
         * The target_object_id preserves the real domain object ID so that
         * commands we don't handle are forwarded correctly by the SF framework.
         */
        Service real_accessor = {};
        R_TRY(breezeAmShimAcPopLaunchRequestedApplication(&m_real_ac, &real_accessor));

        const sf::cmif::DomainObjectId target_object_id{serviceGetObjectId(&real_accessor)};

        /* Store a copy in global state for ResumeGame().
         * IMPORTANT: This copy shares the same domain object ID as the one
         * owned by BreezeAppAccessorService. The wrapper must outlive any
         * use of the global copy. When the game exits, qlaunch will release
         * the accessor and we should clear the global. */
        SetAppAccessor(&real_accessor);

        /* Wrap in transparent pass-through service. */
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IBreezeAppAccessorInterface, BreezeAppAccessorService>(real_accessor), target_object_id);
        R_SUCCEED();
    }

}
