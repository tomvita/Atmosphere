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
#include <string.h>
#include <switch.h>
#include "breeze_am_shim.h"

/* Helper: get a domain sub-object with no input data. */
static Result _shimGetSession(Service* s, Service* out, u32 cmd_id) {
    serviceAssumeDomain(s);
    return serviceDispatch(s, cmd_id,
        .out_num_objects = 1,
        .out_objects = out,
    );
}

/* Helper: get a copy handle output with no data. */
static Result _shimGetHandle(Service* s, Handle* out, u32 cmd_id) {
    serviceAssumeDomain(s);
    return serviceDispatch(s, cmd_id,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = out,
    );
}

/* Helper: no-IO command. */
static Result _shimNoIO(Service* s, u32 cmd_id) {
    serviceAssumeDomain(s);
    return serviceDispatch(s, cmd_id);
}

/* ===== Proxy-level: OpenSystemAppletProxy (cmd 100 on appletAE) ===== */

Result breezeAmShimOpenSystemAppletProxy(Service* s, Service* out, u64 reserved) {
    /*
     * Note: OpenSystemAppletProxy normally requires in_send_pid=true and a
     * process handle. But when forwarding through a MITM, the original
     * client's call has already been dispatched with its PID/handle.
     * We're calling this on the MITM's forward service, so the session
     * already belongs to the correct process (qlaunch).
     *
     * We send reserved=0, PID (via in_send_pid), and CUR_PROCESS_HANDLE.
     * However, since this is called from a MITM context and the forward
     * service is a domain session to the real appletAE, the PID is
     * the MITM's PID. The MITM framework handles the initial proxy open
     * differently — we're forwarding the original client request.
     *
     * Actually, for the MITM pattern used in Atmosphere, the top-level
     * MITM intercepts the command and can forward using the m_forward_service
     * which is a pre-opened session to the real service. We need to dispatch
     * cmd 100 with the same parameters the client sent.
     */
    serviceAssumeDomain(s);
    return serviceDispatchIn(s, 100, reserved,
        .in_send_pid = true,
        .in_num_handles = 1,
        .in_handles = { CUR_PROCESS_HANDLE },
        .out_num_objects = 1,
        .out_objects = out,
    );
}

/* ===== ISystemAppletProxy sub-interface getters ===== */

Result breezeAmShimGetCommonStateGetter(Service* proxy, Service* out) {
    return _shimGetSession(proxy, out, 0);
}

Result breezeAmShimGetLibraryAppletCreator(Service* proxy, Service* out) {
    return _shimGetSession(proxy, out, 11);
}

Result breezeAmShimGetHomeMenuFunctions(Service* proxy, Service* out) {
    return _shimGetSession(proxy, out, 20);
}

Result breezeAmShimGetApplicationCreator(Service* proxy, Service* out) {
    return _shimGetSession(proxy, out, 22);
}

/* ===== ICommonStateGetter commands ===== */

Result breezeAmShimCsgGetEventHandle(Service* csg, Handle* out_handle) {
    return _shimGetHandle(csg, out_handle, 0);
}

Result breezeAmShimCsgReceiveMessage(Service* csg, u32* out_msg) {
    serviceAssumeDomain(csg);
    return serviceDispatchOut(csg, 1, *out_msg);
}

/* ===== IHomeMenuFunctions commands ===== */

Result breezeAmShimHmfRequestToGetForeground(Service* hmf) {
    return _shimNoIO(hmf, 10);
}

Result breezeAmShimHmfPopFromGeneralChannel(Service* hmf, Service* out) {
    return _shimGetSession(hmf, out, 20);
}

Result breezeAmShimHmfGetPopFromGeneralChannelEvent(Service* hmf, Handle* out_handle) {
    return _shimGetHandle(hmf, out_handle, 21);
}

/* ===== ILibraryAppletCreator commands ===== */

Result breezeAmShimLacCreateLibraryApplet(Service* lac, Service* out, u32 applet_id, u32 mode) {
    const struct {
        u32 id;
        u32 mode;
    } in = { applet_id, mode };

    serviceAssumeDomain(lac);
    return serviceDispatchIn(lac, 0, in,
        .out_num_objects = 1,
        .out_objects = out,
    );
}

Result breezeAmShimLacCreateStorage(Service* lac, Service* out, s64 size) {
    serviceAssumeDomain(lac);
    return serviceDispatchIn(lac, 10, size,
        .out_num_objects = 1,
        .out_objects = out,
    );
}

/* ===== ILibraryAppletAccessor commands ===== */

Result breezeAmShimAccessorGetStateChangedEvent(Service* accessor, Handle* out_handle) {
    return _shimGetHandle(accessor, out_handle, 0);
}

Result breezeAmShimAccessorStart(Service* accessor) {
    return _shimNoIO(accessor, 10);
}

Result breezeAmShimAccessorRequestExit(Service* accessor) {
    return _shimNoIO(accessor, 20);
}

Result breezeAmShimAccessorTerminate(Service* accessor) {
    return _shimNoIO(accessor, 25);
}

Result breezeAmShimAccessorGetResult(Service* accessor) {
    return _shimNoIO(accessor, 30);
}

Result breezeAmShimAccessorPushInData(Service* accessor, Service* storage) {
    serviceAssumeDomain(accessor);
    Result rc = serviceDispatch(accessor, 100,
        .in_num_objects = 1,
        .in_objects = { storage },
    );
    /* Close the storage after pushing, matching libnx behavior. */
    serviceClose(storage);
    return rc;
}

Result breezeAmShimAccessorRequestForeground(Service* accessor) {
    return _shimNoIO(accessor, 150);
}

/* ===== IStorage commands ===== */

Result breezeAmShimStorageWrite(Service* storage, const void* data, size_t size, s64 offset) {
    /*
     * libnx pattern: IStorage::Open (cmd 0) → IStorageAccessor,
     * then IStorageAccessor::Write (cmd 10), then close accessor.
     */
    Service accessor = {};
    serviceAssumeDomain(storage);
    Result rc = serviceDispatch(storage, 0,
        .out_num_objects = 1,
        .out_objects = &accessor,
    );
    if (R_FAILED(rc)) {
        return rc;
    }

    /* IStorageAccessor::Write (cmd 10): input s64 offset + buffer. */
    serviceAssumeDomain(&accessor);
    rc = serviceDispatchIn(&accessor, 10, offset,
        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
        .buffers = { { data, size } },
    );

    serviceClose(&accessor);
    return rc;
}

/* ===== IApplicationCreator commands ===== */

Result breezeAmShimAcPopLaunchRequestedApplication(Service* ac, Service* out) {
    return _shimGetSession(ac, out, 1);
}

Result breezeAmShimAcCreateApplication(Service* ac, Service* out, u64 app_id) {
    serviceAssumeDomain(ac);
    return serviceDispatchIn(ac, 0, app_id,
        .out_num_objects = 1,
        .out_objects = out,
    );
}

/* ===== IApplicationAccessor commands ===== */

Result breezeAmShimAppRequestForeground(Service* app) {
    return _shimNoIO(app, 101);
}
