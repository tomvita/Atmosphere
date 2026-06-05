#pragma once
#include <stratosphere.hpp>

namespace ams::dmnt::dbg {

    Result AttachDmnt(os::ProcessId process_id);
    void DetachDmnt();

    Result AttachGen2(os::ProcessId process_id);
    void DetachGen2();

    os::NativeHandle GetSharedDebugHandle();
    os::ProcessId GetSharedProcessId();
    void SetSharedProcessName(const char *name);
    void GetSharedProcessName(char *out_name);

}

