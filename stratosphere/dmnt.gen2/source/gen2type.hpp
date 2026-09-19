#define max_watch_buffer 0x200
#define max_call_stack 5
#define stack_check_size 100
#define GEN2_VERSION "v0.16"
        typedef struct {
            u64 address:64;
            u32 count:32;
        } NX_PACKED m_from_t;
        typedef struct {
            u32 SP_offset : 7;
            u32 code_offset : 25;
        } NX_PACKED call_stack_t;
        typedef struct {
            u64 address : 64;
            call_stack_t stack[max_call_stack];
        } NX_PACKED m_from_stack_t;
        typedef struct {
            m_from_stack_t from_stack;
            int count;
        } NX_PACKED m_from2_t;    
#define max_watch_buffer2 (int) (max_watch_buffer * sizeof(m_from_t) / sizeof(m_from2_t))
        typedef struct {
            u64 address : 39;                    // address of the instr to watch *** use to set break point
            u32 x30_offset : 25;                 // (x30-main)/4 data capture for this must not be stack alternative
            call_stack_t stack[max_call_stack];  // ([SP+SP_offset*8]-main)/4 == code_offset
            u8 i : 5;                           //  [xi+register_offset]!=target_address *** use to test match if mismatch zero the data until no more
            u8 j : 5;
            u8 k : 5;
            bool two_register:1;
            s16 offset : 16;            //  
        } NX_PACKED m_from3_t;
        typedef struct {
            u64 address : 39;     // address of the instr to watch *** use to set break point
            u32 x30_offset : 25;  // (x30-main)/4 data capture for this must not be stack alternative
            u8 i : 5;             //  [xi+register_offset]!=target_address *** use to test match if mismatch zero the data until no more
            u8 j : 5;
            u8 k : 5;
            bool two_register : 1;
            s16 offset : 16;
        } NX_PACKED m_from1_t;

        enum gen2_command {
            SETW,
            CLEARW,
            DETACH,
            ATTACH,
            ATTACH_CONT,
            CONT,
            INCPC,
            SETB,
            CLEARB,
            STEP,
            STEPOVER,
            SETREGS,
            PAUSE,      // v0.16: stop the game (the overlay's "pauses game"), through gen2's own events
            RESUME      // v0.16: let a game PAUSE stopped run again
        };
        enum x30_catch_type_t {
            OFFSET,
            NONE,
            STACK,
            R_MATCH,
            EXCLUSIVE_SEARCH,
            UNIQUE_SEARCH,
            END_OF_TYPE,
        } NX_PACKED;
        typedef struct {
            bool execute = false, done = false;
            gen2_command command = SETW;
            int count = 0;
            u16 i = 8;
            u8 j = 0, k =0;
            u64 address=0xAA55AA5566,next_address, base = 0, offset = 0;
            const char *module_name = name;
            char name[10] = "undefine";
            bool read, next_read, write, next_write, intercepted = 0;
            x30_catch_type_t x30_catch_type = OFFSET;        
            u64 next_pc=0x55AA55AA;
            union {
                m_from_t from[max_watch_buffer];
                m_from1_t from1[max_watch_buffer];
                m_from2_t from2[max_watch_buffer2];
                m_from3_t from3[max_watch_buffer2];
            } fromU;
            int failed = 0;
            u8 gen2loop_on = false;
            u64 next_pid;
            bool attach_success;
            bool attached;
            bool range_check = false;
            u64 v1,v2;
            int size = 4;
            int vsize = 4;
            char version[10] = GEN2_VERSION;
            u16 x30_match = 0;
            bool check_x30 = false;
            bool two_register = false;
            u16 stack_check_count = 0; //must be not greater than max_call_stack
            u16 exclusive_search_count = 0;  //
            bool exclusive_search_from2 = false;
            u32 exclusive_search_target_trigger = 0;
            u64 target_address;
            u64 main_start, main_end;
            u64 total_trigger = 0;
            u64 max_trigger = 0x10000;
            u16 caller_SP_offset = 0;
            u64 grab_A_address = 0;
            bool grab_A = false;
            bool grab_R = false;
            u8 Register = 14;
            u64 Register_match_value = 0;
            bool bp_hit = false;
            u64 bp_addr = 0;
            u64 bp_thread_id = 0;
            ams::svc::ThreadContext bp_ctx{};
            // Break and Trace: a hit stops the game when pc and lr match; 0 matches any value (v0.16)
            bool bp_match_trigger = false;
            u64 bp_match_pc = 0;
            u64 bp_match_lr = 0;
            u32 bp_original_insn = 0;
            // v0.16 Break and Trace filter, appended after the older layout (16-byte
            // aligned, so it starts where that block ended): a hit breaks only when
            // the row a capture would record for it matches the chosen parts of a
            // captured row. Zeroed when a client sends the older, shorter block.
            alignas(16) u32 bp_filter_flags = 0;  // BP_FILTER_X30 | BP_FILTER_STACK(slot)
            u32 bp_filter_call_from = 0;          // the row's call site: (x30 - main) >> 2
            call_stack_t bp_filter_stack[max_call_stack]{};
        } m_watch_data_t;
#define BP_FILTER_X30 1u
#define BP_FILTER_STACK(slot) (2u << (slot))