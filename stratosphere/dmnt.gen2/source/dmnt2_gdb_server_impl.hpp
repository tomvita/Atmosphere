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
#include "dmnt2_gdb_packet_io.hpp"
#include "dmnt2_gdb_signal.hpp"
#include "dmnt2_debug_process.hpp"

namespace ams::dmnt {
#include "gen2type.hpp"

    class GdbServerImpl {
        private:
            enum class State {
                Initial,
                Running,
                Exited,
                Destroyed,
            };
        private:
            int m_socket;
            TransportSession m_session;
            GdbPacketIo m_packet_io;
            char *m_receive_packet{nullptr};
            char *m_reply_cur{nullptr};
            char *m_reply_end{nullptr};
            char m_buffer[GdbPacketBufferSize / 2];
            bool m_killed{false};
            os::ThreadType m_events_thread;
            State m_state;
            DebugProcess m_debug_process;
            os::ProcessId m_process_id{os::InvalidProcessId};
            os::Event m_event;
            os::ProcessId m_wait_process_id{os::InvalidProcessId};
            bool m_gen2_watch_active{false};
            u64 m_gen2_watch_address{0};
            u64 m_gen2_watch_size{0};
            bool m_gen2_watch_read{false};
            bool m_gen2_watch_write{false};
            u64 m_step_original_pc{0};
            bool m_step_pending{false};
            bool m_watchpoint_rearm_pending{false};
            bool m_instruction_rearm_pending{false};
            bool m_continue_after_step{false};
            /* FIELD_WATCH: the whole of [m_fw_lo, m_fw_hi) is covered by up to
             * four aligned power-of-two watchpoints. A hit lifts the block that
             * fired (m_fw_lifted, a mask over m_fw_blocks), lets the access
             * through to a hardware breakpoint at m_fw_step_pc and puts the
             * block back there. */
            struct FieldWatchBlock { u64 address; u64 size; };
            enum class FieldWatchState : u8 {
                None,
                Step,  /* only the thread that hit runs, to m_fw_step_pc */
                Wait,  /* every thread runs; m_fw_step_pc or a timeout ends it */
            };
            enum class FieldWatchResume : u8 { All, Thread };
            bool m_fw_active{false};
            bool m_fw_read{false};
            bool m_fw_write{false};
            bool m_fw_excl{false};
            bool m_fw_dirty{false};
            /* FIELD_SWEEP: the blocks are exact pieces of what is still unseen,
             * four of them from piece m_fw_sweep_cursor on, moved on by the tick. */
            bool m_fw_sweep{false};
            u32 m_fw_sweep_cursor{0};
            os::Tick m_fw_sweep_start{};
            u8 m_fw_excl_rs{0};
            u8 m_fw_lifted{0};
            FieldWatchState m_fw_state{FieldWatchState::None};
            u32 m_fw_block_count{0};
            FieldWatchBlock m_fw_blocks[4]{};
            u64 m_fw_lo{0};
            u64 m_fw_hi{0};
            u64 m_fw_step_pc{0};
            os::Tick m_fw_state_start{};
            void FieldWatchSet();
            u32 FieldWatchBuildCover(FieldWatchBlock *out) const;
            u32 FieldWatchBuildSweep(FieldWatchBlock *out) const;
            /* METHOD_WATCH. The table is this object's own: m_watch_data is
             * the client's to overwrite, and a break instruction left in the
             * game with nobody to answer for it crashes the game. It outlives
             * the watch (m_mw_active) so a hit that was already on its way
             * when the watch ended is still recognised. */
            struct MethodWatchEntry { u32 method; u32 original; u32 caller; u8 state; u64 self; };
            static constexpr size_t MethodWatchMax = 384;
            MethodWatchEntry m_mw[MethodWatchMax]{};
            u32 m_mw_count{0};
            u64 m_mw_main{0};
            bool m_mw_active{false};
            u64 m_mw_pid{0};
            u64 m_cw_pid{0};
            void WatchTablesAfterAttach();
            void DetachWithWatchBreaksOff();
            void MethodWatchSet();
            void MethodWatchClear();
            void MethodWatchSync();
            bool MethodWatchOnBreak(u64 thread_id, u64 address);
            /* CODE_WATCH: the same one-shot breaks for a whole module. The
             * list, with each original instruction, is a file on the SD card;
             * kept here are the first function of every chunk of it, one chunk,
             * and the newest hits. A hit reads its chunk unless it is the one
             * already here. The file stays open until the detach, so a hit
             * that was on its way when the breaks came off is still known. */
            static constexpr u32 CodeWatchChunk = 256;
            u32 m_cw_index[max_code_watch / CodeWatchChunk]{};
            m_code_entry_t m_cw_chunk[CodeWatchChunk]{};
            u32 m_cw_chunk_at{0xFFFFFFFF};
            m_code_hit_t m_cw_ring[max_code_ring]{};
            u32 m_cw_ring_count{0};
            u32 m_cw_hits{0};
            u32 m_cw_count{0};
            u64 m_cw_main{0};
            bool m_cw_active{false};
            bool m_cw_file_open{false};
            fs::FileHandle m_cw_file{};
            void CodeWatchArm();
            void CodeWatchDisarm();
            void CodeWatchClose();
            void CodeWatchSpill();
            bool CodeWatchChunkLoad(u32 chunk);
            bool CodeWatchOnBreak(u64 thread_id, u64 address);
            void FieldWatchClear();
            void FieldWatchLift(u64 address);
            void FieldWatchRearm();
            void FieldWatchTick();
            bool FieldWatchCovers(u64 address) const;
            FieldWatchResume FieldWatchOnAccess(u64 thread_id, u64 address);
            void FieldWatchOnStep(u64 thread_id);
        public:
            GdbServerImpl(int socket, void *thread_stack, size_t stack_size);
            ~GdbServerImpl();

            void LoopProcess();
            bool gen2_loop();
            u8 gen2_server_on = 0;
            void Gen2Attach();
            m_from_stack_t get_from_stack(svc::ThreadContext &thread_context, bool get_address);
            bool BreakFilterMatches(svc::ThreadContext &thread_context);
            void set_next_watch_for_exclusive_search();
            bool HasDebugProcess() const { return m_debug_process.IsValid(); }
            os::NativeHandle GetDebugHandle() const { return m_debug_process.GetHandle(); }

           private:
            void ProcessPacket(char *receive, char *reply);

            void SendPacket(bool *out_break, const char *src) { return m_packet_io.SendPacket(out_break, src, std::addressof(m_session)); }
            char *ReceivePacket(bool *out_break, char *dst, size_t size) { return m_packet_io.ReceivePacket(out_break, dst, size, std::addressof(m_session)); }
        private:
            bool Is64Bit() const { return m_debug_process.Is64Bit(); }
            bool Is64BitAddressSpace() const { return m_debug_process.Is64BitAddressSpace(); }
        private:
            static void DebugEventsThreadEntry(void *arg) { static_cast<GdbServerImpl *>(arg)->DebugEventsThread(); }
            void DebugEventsThread();
            void ProcessDebugEvents();
            void AppendStopReplyPacket(GdbSignal signal);
        private:
            void D();

            void G();

            void H();
            void Hg();

            void M();

            void P();

            void Q();

            void QStartNoAckMode();

            void T();

            void Z();

            void c();

            bool g();

            void k();

            void m();

            void p();

            void v();

            void vAttach();
            void vCont();

            void q();

            void qAttached();
            void qC();
            void qRcmd();
            void qSupported();
            void qXfer();
            void qXferFeaturesRead();
            void qXferLibrariesRead();
            void qXferOsdataRead();
            bool qXferThreadsRead();
            void get_region(u64 address);
            void setw();
            void clearw();

            void z();

            void QuestionMark();
        private:
            Result ParseVCont(char * const token, u64 *thread_ids, u8 *continue_modes, s32 num_threads, DebugProcess::ContinueMode &default_continue_mode);
    };

}
