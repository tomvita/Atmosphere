/**
 * @file breeze_am_shim.h
 * @brief AM (Applet Manager) IPC wrapper for breeze.mitm.
 * @copyright Atmosphère-NX Authors
 *
 * C shim functions for forwarding IPC calls to the real AM service
 * through MITM'd domain sessions.
 */
#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Proxy-level forwarders (on the MITM forward service → real appletAE) ===== */

/**
 * Forward OpenSystemAppletProxy (cmd 100) to real appletAE.
 * @param s        The MITM forward service handle (real appletAE session).
 * @param out      Output: ISystemAppletProxy domain sub-object.
 * @param reserved Must be 0.
 */
Result breezeAmShimOpenSystemAppletProxy(Service* s, Service* out, u64 reserved);

/* ===== ISystemAppletProxy sub-interface forwarders ===== */

/**
 * Forward GetCommonStateGetter (cmd 0) on ISystemAppletProxy.
 * @param proxy  The real ISystemAppletProxy domain sub-object.
 * @param out    Output: ICommonStateGetter domain sub-object.
 */
Result breezeAmShimGetCommonStateGetter(Service* proxy, Service* out);

/**
 * Forward GetLibraryAppletCreator (cmd 11) on ISystemAppletProxy.
 * @param proxy  The real ISystemAppletProxy domain sub-object.
 * @param out    Output: ILibraryAppletCreator domain sub-object.
 */
Result breezeAmShimGetLibraryAppletCreator(Service* proxy, Service* out);

/**
 * Forward GetHomeMenuFunctions (cmd 20) on ISystemAppletProxy.
 * @param proxy  The real ISystemAppletProxy domain sub-object.
 * @param out    Output: IHomeMenuFunctions domain sub-object.
 */
Result breezeAmShimGetHomeMenuFunctions(Service* proxy, Service* out);

/**
 * Forward GetApplicationCreator (cmd 22) on ISystemAppletProxy.
 * @param proxy  The real ISystemAppletProxy domain sub-object.
 * @param out    Output: IApplicationCreator domain sub-object.
 */
Result breezeAmShimGetApplicationCreator(Service* proxy, Service* out);

/* ===== ICommonStateGetter commands ===== */

/**
 * Forward GetEventHandle (cmd 0) on ICommonStateGetter.
 * Returns the event handle that signals when AppletMessages are available.
 */
Result breezeAmShimCsgGetEventHandle(Service* csg, Handle* out_handle);

/**
 * Forward ReceiveMessage (cmd 1) on ICommonStateGetter.
 * @param out_msg  Output: AppletMessage value (u32).
 */
Result breezeAmShimCsgReceiveMessage(Service* csg, u32* out_msg);

/* ===== IHomeMenuFunctions commands ===== */

/**
 * Forward RequestToGetForeground (cmd 10) on IHomeMenuFunctions.
 */
Result breezeAmShimHmfRequestToGetForeground(Service* hmf);

/**
 * Forward PopFromGeneralChannel (cmd 20) on IHomeMenuFunctions.
 * @param out  Output: IStorage domain sub-object.
 */
Result breezeAmShimHmfPopFromGeneralChannel(Service* hmf, Service* out);

/**
 * Forward GetPopFromGeneralChannelEvent (cmd 21) on IHomeMenuFunctions.
 */
Result breezeAmShimHmfGetPopFromGeneralChannelEvent(Service* hmf, Handle* out_handle);

/* ===== ILibraryAppletCreator commands ===== */

/**
 * Forward CreateLibraryApplet (cmd 0) on ILibraryAppletCreator.
 * @param applet_id  AppletId (e.g., AppletId_LibraryAppletPhotoViewer = 0x10)
 * @param mode       LibAppletMode (e.g., LibAppletMode_AllForeground = 0)
 * @param out        Output: ILibraryAppletAccessor domain sub-object.
 */
Result breezeAmShimLacCreateLibraryApplet(Service* lac, Service* out, u32 applet_id, u32 mode);

/**
 * Forward CreateStorage (cmd 10) on ILibraryAppletCreator.
 * @param size  Size of the storage to create.
 * @param out   Output: IStorage domain sub-object.
 */
Result breezeAmShimLacCreateStorage(Service* lac, Service* out, s64 size);

/* ===== ILibraryAppletAccessor commands ===== */

/**
 * Forward GetAppletStateChangedEvent (cmd 0) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorGetStateChangedEvent(Service* accessor, Handle* out_handle);

/**
 * Forward Start (cmd 10) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorStart(Service* accessor);

/**
 * Forward RequestExit (cmd 20) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorRequestExit(Service* accessor);

/**
 * Forward Terminate (cmd 25) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorTerminate(Service* accessor);

/**
 * Forward GetResult (cmd 30) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorGetResult(Service* accessor);

/**
 * Forward PushInData (cmd 100) on ILibraryAppletAccessor.
 * @param storage  The IStorage domain sub-object to push. Closed after use.
 */
Result breezeAmShimAccessorPushInData(Service* accessor, Service* storage);

/**
 * Forward RequestForAppletToGetForeground (cmd 150) on ILibraryAppletAccessor.
 */
Result breezeAmShimAccessorRequestForeground(Service* accessor);

/* ===== IStorage commands ===== */

/**
 * Open IStorage (cmd 0) to get IStorageAccessor, write data via
 * IStorageAccessor::Write (cmd 10), then close the accessor.
 * This matches the libnx appletStorageWrite() pattern.
 *
 * @param storage  The IStorage domain sub-object.
 * @param data     Data buffer to write.
 * @param size     Number of bytes.
 * @param offset   Offset in storage.
 */
Result breezeAmShimStorageWrite(Service* storage, const void* data, size_t size, s64 offset);

/* ===== IApplicationCreator commands ===== */

/**
 * Forward PopLaunchRequestedApplication (cmd 1) on IApplicationCreator.
 * @param out  Output: IApplicationAccessor domain sub-object.
 */
Result breezeAmShimAcPopLaunchRequestedApplication(Service* ac, Service* out);

/**
 * Forward CreateApplication (cmd 0) on IApplicationCreator.
 * @param out      Output: IApplicationAccessor domain sub-object.
 * @param app_id   Application title ID.
 */
Result breezeAmShimAcCreateApplication(Service* ac, Service* out, u64 app_id);

/* ===== IApplicationAccessor commands ===== */

/**
 * Forward RequestForApplicationToGetForeground (cmd 101) on IApplicationAccessor.
 */
Result breezeAmShimAppRequestForeground(Service* app);

#ifdef __cplusplus
}
#endif
