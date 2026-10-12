#define max_watch_buffer 0x200
#define max_call_stack 5
#define stack_check_size 100
#define GEN2_VERSION "v0.20"
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
            RESUME,     // v0.16: let a game PAUSE stopped run again
            GETTHREADS  // v0.16: fill the thread list below, and refresh bp_ctx for bp_thread_id
        };
#define max_thread_list 32
        enum x30_catch_type_t {
            OFFSET,
            NONE,
            STACK,
            R_MATCH,
            EXCLUSIVE_SEARCH,
            UNIQUE_SEARCH,
            END_OF_TYPE,
            // v0.17, after END_OF_TYPE so the call-site source cycle never lands on it:
            // watch every byte of [next_address, next_address + size) and record
            // which were accessed, one bit per byte in fromU (count stays 0).
            FIELD_WATCH,
            // The same, for the bytes the bitmap sent with it does not mark yet:
            // four exact watchpoints at a time, moved round the rest. The game
            // runs at speed, and an access outside the four of the moment is missed.
            FIELD_SWEEP,
            // Which methods are ever called: fromU holds `size` m_method_t entries.
            // A break instruction goes on each method's first instruction and
            // comes off at its first hit, so a method stops the game once.
            METHOD_WATCH,
            // v0.19: METHOD_WATCH for a whole module. The list is a file (see
            // CODE_WATCH_LIST below), since neither this block nor the fork's
            // memory holds a hundred thousand functions.
            CODE_WATCH,
        } NX_PACKED;
        typedef struct {
            u32 method;       // the method's first instruction, as an offset from main_start
            u32 caller : 28;  // (return address - main_start) >> 2 at its first call; 0 until then, or outside main
            u32 state : 4;    // METHOD_*
            u64 self;         // x0 at that call: for an instance method, the object it was called on
        } NX_PACKED m_method_t;
        enum {
            METHOD_ARMED,        // waiting: not called so far
            METHOD_CALLED,
            METHOD_OVERWRITTEN,  // something else rewrote the instruction (a cheat's hook): unknown
            METHOD_NOT_SET,      // the break could not be put there
        };
#define max_method_watch ((int)(max_watch_buffer * sizeof(m_from_t) / sizeof(m_method_t)))
        // CODE_WATCH. Before the SETW the client writes CODE_WATCH_LIST, `v1`
        // records of m_code_entry_t ascending by method with original 0, and
        // leaves CODE_WATCH_HITS empty. The fork fills in each original as it
        // puts the break there. Each first call is an m_code_hit_t: the newest
        // are in fromU (`count` of them), the ones before were appended to
        // CODE_WATCH_HITS, and total_trigger counts both. CLEARW takes the
        // breaks off and appends the rest, so the file is then the whole run.
        // Out of the SETW: v1 = functions, v2 = those no break could be put on;
        // failed = 6 the list could not be read, 7 the hits could not be written.
        typedef struct {
            u32 method;    // the function's first instruction, as an offset from main_start
            u32 original;  // the instruction the break replaced; the break itself where none was put
        } NX_PACKED m_code_entry_t;
        typedef struct {
            u32 method;
            u32 caller;    // (return address - main_start) >> 2 at the first call, 0 outside main
        } NX_PACKED m_code_hit_t;
#define CODE_WATCH_LIST "sdmc:/switch/breeze/code_watch.bin"
#define CODE_WATCH_HITS "sdmc:/switch/breeze/code_watch_hits.bin"
#define max_code_watch 131072
#define max_code_ring ((int)(max_watch_buffer * sizeof(m_from_t) / sizeof(m_code_hit_t)))
// The largest instance a FIELD_WATCH bitmap holds, in bytes.
#define field_watch_max_size (max_watch_buffer * sizeof(m_from_t) * 8)
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
            // v0.16 thread list (GETTHREADS): every thread of the game and where
            // it stands. A stopped game holds them all; a paused one has each
            // thread wherever it happened to be, so there is no single PC.
            u32 thread_count = 0;
            struct {
                u64 id;
                u64 pc;
                u64 lr;
                u64 sp;
                char name[32];
            } threads[max_thread_list]{};
        } m_watch_data_t;
#define BP_FILTER_X30 1u
#define BP_FILTER_STACK(slot) (2u << (slot))
// Break only when register `Register` (0-30) holds `Register_match_value`: at a
// method's first instruction X0 is the object it was called on, so this is a
// break on one instance.
#define BP_FILTER_REGISTER 0x100u
/* The block as it was before the Break and Trace fields. Breeze stores exactly
 * this much in its saved results, so the fields after it must not move. */
#define GEN2_SAVED_SIZE 7232
