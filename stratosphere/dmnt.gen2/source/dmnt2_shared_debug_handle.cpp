#include <stratosphere.hpp>
#include "dmnt2_shared_debug_handle.hpp"

namespace ams::dmnt::dbg {

    namespace {

        os::NativeHandle g_shared_debug_handle = os::InvalidNativeHandle;
        os::ProcessId g_shared_process_id = os::InvalidProcessId;
        char g_shared_process_name[12] = {0};
        bool g_attach_dmnt = false;
        bool g_attach_gen2 = false;
        constinit os::SdkMutex g_shared_handle_lock;

    }

    Result AttachDmnt(os::ProcessId process_id) {
        std::scoped_lock lk(g_shared_handle_lock);

        if (g_shared_debug_handle != os::InvalidNativeHandle && g_shared_process_id != process_id) {
            svc::CloseHandle(g_shared_debug_handle);
            g_shared_debug_handle = os::InvalidNativeHandle;
            g_shared_process_id   = os::InvalidProcessId;
            g_shared_process_name[0] = '\0';
        }

        if (g_shared_debug_handle == os::InvalidNativeHandle) {
            R_TRY(svc::DebugActiveProcess(std::addressof(g_shared_debug_handle), process_id.value));
            g_shared_process_id = process_id;
        }

        g_attach_dmnt = true;
        R_SUCCEED();
    }

    void DetachDmnt() {
        std::scoped_lock lk(g_shared_handle_lock);

        g_attach_dmnt = false;

        if (!g_attach_dmnt && !g_attach_gen2) {
            if (g_shared_debug_handle != os::InvalidNativeHandle) {
                svc::CloseHandle(g_shared_debug_handle);
                g_shared_debug_handle = os::InvalidNativeHandle;
                g_shared_process_id   = os::InvalidProcessId;
                g_shared_process_name[0] = '\0';
            }
        }
    }

    Result AttachGen2(os::ProcessId process_id) {
        std::scoped_lock lk(g_shared_handle_lock);

        /* Join a handle that is already open for this process rather than
         * re-opening it, exactly as AttachDmnt does.
         *
         * This used to re-open whenever gen2 itself was not yet attached (the
         * condition also tested `!g_attach_gen2`), which closed the handle out
         * from under the cheat engine even when it was already open for the
         * very process we wanted. That was the sole reason
         * DebugProcess::Attach() had to ForceCloseCheatProcess() first -- and
         * closing the cheat process wipes the cheat list, the toggles and the
         * whole frozen-address map, then re-reads every cheat off the SD card
         * on the way back. The handle is shared; there is no kernel reason to
         * churn it.
         *
         * A different process is still a real re-open: the shared handle is a
         * single slot, so the cheat engine has to be told to let go, which the
         * caller does with ForceCloseCheatProcess. */
        if (g_shared_debug_handle != os::InvalidNativeHandle && g_shared_process_id != process_id) {
            svc::CloseHandle(g_shared_debug_handle);
            g_shared_debug_handle = os::InvalidNativeHandle;
            g_shared_process_id   = os::InvalidProcessId;
            g_shared_process_name[0] = '\0';
        }

        if (g_shared_debug_handle == os::InvalidNativeHandle) {
            R_TRY(svc::DebugActiveProcess(std::addressof(g_shared_debug_handle), process_id.value));
            g_shared_process_id = process_id;
        }

        g_attach_gen2 = true;
        R_SUCCEED();
    }

    void DetachGen2() {
        std::scoped_lock lk(g_shared_handle_lock);

        g_attach_gen2 = false;

        if (!g_attach_dmnt && !g_attach_gen2) {
            if (g_shared_debug_handle != os::InvalidNativeHandle) {
                svc::CloseHandle(g_shared_debug_handle);
                g_shared_debug_handle = os::InvalidNativeHandle;
                g_shared_process_id   = os::InvalidProcessId;
                g_shared_process_name[0] = '\0';
            }
        }
    }

    os::NativeHandle GetSharedDebugHandle() {
        std::scoped_lock lk(g_shared_handle_lock);
        return g_shared_debug_handle;
    }

    os::ProcessId GetSharedProcessId() {
        std::scoped_lock lk(g_shared_handle_lock);
        return g_shared_process_id;
    }

    void SetSharedProcessName(const char *name) {
        std::scoped_lock lk(g_shared_handle_lock);
        std::strncpy(g_shared_process_name, name, sizeof(g_shared_process_name) - 1);
        g_shared_process_name[sizeof(g_shared_process_name) - 1] = '\0';
    }

    void GetSharedProcessName(char *out_name) {
        std::scoped_lock lk(g_shared_handle_lock);
        std::strncpy(out_name, g_shared_process_name, 12);
    }

}
