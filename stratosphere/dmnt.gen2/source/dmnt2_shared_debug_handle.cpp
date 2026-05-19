#include <stratosphere.hpp>
#include "dmnt2_shared_debug_handle.hpp"

namespace ams::dmnt::dbg {

    namespace {

        os::NativeHandle g_shared_debug_handle = os::InvalidNativeHandle;
        os::ProcessId g_shared_process_id = os::InvalidProcessId;
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
            }
        }
    }

    Result AttachGen2(os::ProcessId process_id) {
        std::scoped_lock lk(g_shared_handle_lock);

        if (g_shared_debug_handle != os::InvalidNativeHandle && g_shared_process_id != process_id) {
            svc::CloseHandle(g_shared_debug_handle);
            g_shared_debug_handle = os::InvalidNativeHandle;
            g_shared_process_id   = os::InvalidProcessId;
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
            }
        }
    }

    os::NativeHandle GetSharedDebugHandle() {
        std::scoped_lock lk(g_shared_handle_lock);
        return g_shared_debug_handle;
    }

}
