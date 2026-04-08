#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>
#include <vector>
#include <queue>
#include <deque>

#define DEFAULT_K0 1
#define DEFAULT_K1 2
#define DEFAULT_K2 3
#define DEFAULT_R 8
#define DEFAULT_F 4

typedef struct _proc_inst_t
{
    uint32_t instruction_address;
    int32_t op_code;
    int32_t src_reg[2];
    int32_t dest_reg;
    
    // Additional fields for tracking
    uint64_t tag;                    // Instruction tag/ID
    uint64_t fetch_cycle;            // Cycle when fetched
    uint64_t dispatch_cycle;         // Cycle when dispatched
    uint64_t rs_entry_cycle;         // Cycle when entered RS
    uint64_t schedule_cycle;         // Cycle when fired (for output)
    uint64_t fire_cycle;             // Cycle when fired to FU
    uint64_t execute_cycle;          // Cycle when execution completes
    uint64_t state_update_cycle;     // Cycle when state updated
    uint64_t complete_cycle;         // Cycle when execution completes
    uint64_t delete_cycle;           // Cycle when deleted from RS
    
    bool src_ready[2];               // Source operand ready bits
    bool fired;                      // Has been fired to FU
    int32_t fu_type;                 // Actual FU type (handling -1 case)
    bool completed;                  // Execution completed
    bool fu_freed;                   // FU has been freed
    bool fu_to_free;                 // FU should be freed in second half
    int32_t fu_index;                // Which FU is executing this
    
    uint64_t src_writer_tag[2];      // Tag of last writer for each source register
    
} proc_inst_t;

typedef struct _proc_stats_t
{
    double avg_inst_retired;
    double avg_inst_fired;
    double avg_disp_size;
    unsigned long max_disp_size;
    unsigned long retired_instruction;
    unsigned long cycle_count;
    unsigned long total_disp_size;  // Total dispatch queue size sum
    unsigned long total_fired;      // Total instructions fired
} proc_stats_t;

bool read_instruction(proc_inst_t* p_inst);

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f);
void run_proc(proc_stats_t* p_stats);
void complete_proc(proc_stats_t* p_stats);

#endif /* PROCSIM_HPP */
