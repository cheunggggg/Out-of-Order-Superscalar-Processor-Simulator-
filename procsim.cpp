/**
 * procsim.cpp
 * Out-of-Order Superscalar Processor Simulator
 * 
 * Implementation of a 5-stage OoO processor with:
 * - Fetch, Dispatch, Schedule (Reservation Station), Execute, State Update stages
 * - Centralized reservation station
 * - Multiple functional units (k0, k1, k2 types)
 * - Result buses for state update
 * - Half-cycle behavior for register file and FU management
 */

#include "procsim.hpp"
#include <cstring>
#include <algorithm>
#include <vector>
#include <deque>
#include <set>
#include <map>

// Set to 0 for Gradescope submission (no debug output)
#define DEBUG_OUTPUT 0  // Must be 0 for Gradescope

// Global state
static uint64_t num_result_buses, num_k0_fus, num_k1_fus, num_k2_fus, fetch_rate, scheduling_queue_size;
static std::deque<proc_inst_t> dispatch_queue, scheduling_queue;
static std::vector<bool> k0_fus_busy, k1_fus_busy, k2_fus_busy;
static uint64_t next_tag = 1, current_cycle = 0;
static bool fetching_done = false;
static uint64_t total_dispatch_queue_size = 0, max_dispatch_queue_size = 0;
static uint64_t total_instructions_fired = 0, total_instructions_retired = 0;

// Store all completed instructions for final output
static std::vector<proc_inst_t> completed_instructions;

// Track which registers have been marked ready by completed instructions
static std::set<uint64_t> completed_instruction_tags;

// Map from register number to the tag of the last instruction that writes to it
static std::map<int32_t, uint64_t> last_writer_map;

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f) 
{
    num_result_buses = r; 
    num_k0_fus = k0; 
    num_k1_fus = k1; 
    num_k2_fus = k2; 
    fetch_rate = f;
    scheduling_queue_size = 2 * (k0 + k1 + k2);
    
    k0_fus_busy.assign(k0, false); 
    k1_fus_busy.assign(k1, false); 
    k2_fus_busy.assign(k2, false);
    dispatch_queue.clear(); 
    scheduling_queue.clear();
    completed_instructions.clear();
    completed_instruction_tags.clear();
    last_writer_map.clear();
    
    next_tag = 1; 
    current_cycle = 0; 
    fetching_done = false;
    total_dispatch_queue_size = 0; 
    max_dispatch_queue_size = 0;
    total_instructions_fired = 0; 
    total_instructions_retired = 0;
}

void run_proc(proc_stats_t* p_stats)
{
#if DEBUG_OUTPUT
    // Print header for log (to stderr)
    fprintf(stderr, "CYCLE\tOPERATION\tINSTRUCTION\n");
#endif
    
    while (true) {
        current_cycle++;
        total_dispatch_queue_size += dispatch_queue.size();
        if (dispatch_queue.size() > max_dispatch_queue_size)
            max_dispatch_queue_size = dispatch_queue.size();
        
        // ===== FIRST HALF OF CYCLE =====
        
        // 1. Mark execution complete (but don't print yet)
        for (auto& inst : scheduling_queue) {
            if (inst.fired && !inst.completed) {
                // Check if instruction has been executing for the required latency
                if (inst.fire_cycle == 0) continue; // Not yet fired
                
                uint64_t latency = 1; // All FUs have latency 1
                // Instruction fires at fire_cycle, completes at fire_cycle + latency
                if (current_cycle == inst.fire_cycle + latency) {
                    inst.completed = true;
                    inst.complete_cycle = current_cycle;
                    // execute_cycle was already set when it fired
                    
                    // DO NOT free FU here - wait until state update
                }
            }
        }
        
        // 2. State Update - retire completed instructions (limited by R result buses)
        // Registers are marked ready when instruction enters state update
        // Sort by completion cycle first, then by tag
        // Instructions retire the same cycle they complete
        std::vector<size_t> ready_to_retire;
        for (size_t i = 0; i < scheduling_queue.size(); i++) {
            if (scheduling_queue[i].completed && 
                scheduling_queue[i].state_update_cycle == 0 &&
                scheduling_queue[i].complete_cycle <= current_cycle) {
                ready_to_retire.push_back(i);
            }
        }
        
        std::sort(ready_to_retire.begin(), ready_to_retire.end(),
                  [&](size_t a, size_t b) {
                      if (scheduling_queue[a].complete_cycle != scheduling_queue[b].complete_cycle)
                          return scheduling_queue[a].complete_cycle < scheduling_queue[b].complete_cycle;
                      return scheduling_queue[a].tag < scheduling_queue[b].tag;
                  });
        
        // Retire up to R instructions - registers marked ready at this point
        for (size_t i = 0; i < ready_to_retire.size() && i < num_result_buses; i++) {
            auto& inst = scheduling_queue[ready_to_retire[i]];
            inst.state_update_cycle = current_cycle;
            inst.delete_cycle = current_cycle + 1; // Delete next cycle
            
            // Mark this instruction as having reached state update (registers now ready)
            completed_instruction_tags.insert(inst.tag);
            
            // Free FU when instruction enters state update (first half)
            if (inst.fu_index != -1) {
                if (inst.fu_type == 0) k0_fus_busy[inst.fu_index] = false;
                else if (inst.fu_type == 1) k1_fus_busy[inst.fu_index] = false;
                else if (inst.fu_type == 2) k2_fus_busy[inst.fu_index] = false;
            }
            
#if DEBUG_OUTPUT
            fprintf(stderr, "%lu\tSTATE UPDATE\t%lu\n", current_cycle, inst.tag);
#endif
            total_instructions_retired++;
        }
        
#if DEBUG_OUTPUT
        // Print EXECUTED for instructions that completed this cycle (after state update)
        for (const auto& inst : scheduling_queue) {
            if (inst.completed && inst.complete_cycle == current_cycle) {
                fprintf(stderr, "%lu\tEXECUTED\t%lu\n", current_cycle, inst.tag);
            }
        }
#endif
        
        // 3. Schedule/Fire ready instructions
        // Instructions can fire if their source registers' last writers have reached state update
        std::vector<size_t> ready_to_fire;
        for (size_t i = 0; i < scheduling_queue.size(); i++) {
            auto& inst = scheduling_queue[i];
            // Instruction can fire if not already fired and has spent at least one cycle in RS
            // If enters RS at cycle J, can fire at cycle J+1 or later
            if (!inst.fired && inst.rs_entry_cycle > 0 && inst.rs_entry_cycle < current_cycle) {
                // Check if operands are ready using pre-recorded dependencies
                bool ready = true;
                
                for (int j = 0; j < 2; j++) {
                    if (inst.src_writer_tag[j] > 0) {
                        // This source has a dependency - check if writer has reached state update
                        bool writer_ready = false;
                        
                        // Search for the writer in scheduling queue
                        for (const auto& other : scheduling_queue) {
                            if (other.tag == inst.src_writer_tag[j]) {
                                // Register is ready the cycle AFTER the writer enters state update
                                if (other.state_update_cycle > 0 && other.state_update_cycle < current_cycle) {
                                    writer_ready = true;
                                }
                                break;
                            }
                        }
                        
                        // Also check in completed instructions
                        if (!writer_ready) {
                            for (const auto& other : completed_instructions) {
                                if (other.tag == inst.src_writer_tag[j]) {
                                    if (other.state_update_cycle > 0 && other.state_update_cycle < current_cycle) {
                                        writer_ready = true;
                                    }
                                    break;
                                }
                            }
                        }
                        
                        if (!writer_ready) {
                            ready = false;
                            break;
                        }
                    }
                }
                
                if (ready) {
                    ready_to_fire.push_back(i);
                }
            }
        }
        
        // Sort by tag order
        std::sort(ready_to_fire.begin(), ready_to_fire.end(),
                  [&](size_t a, size_t b) { return scheduling_queue[a].tag < scheduling_queue[b].tag; });
        
        // Try to fire instructions (limited by FU availability)
        for (size_t idx : ready_to_fire) {
            auto& inst = scheduling_queue[idx];
            int32_t free_fu = -1;
            
            if (inst.fu_type == 0) {
                for (size_t i = 0; i < k0_fus_busy.size(); i++) {
                    if (!k0_fus_busy[i]) { 
                        free_fu = i; 
                        k0_fus_busy[i] = true; 
                        break; 
                    }
                }
            } else if (inst.fu_type == 1) {
                for (size_t i = 0; i < k1_fus_busy.size(); i++) {
                    if (!k1_fus_busy[i]) { 
                        free_fu = i; 
                        k1_fus_busy[i] = true; 
                        break; 
                    }
                }
            } else if (inst.fu_type == 2) {
                for (size_t i = 0; i < k2_fus_busy.size(); i++) {
                    if (!k2_fus_busy[i]) { 
                        free_fu = i; 
                        k2_fus_busy[i] = true; 
                        break; 
                    }
                }
            }
            
                if (free_fu != -1) {
                    inst.fired = true;
                    inst.fire_cycle = current_cycle; // Track when it fires
                    inst.execute_cycle = current_cycle; // EXEC in output is when it fires
                    inst.fu_index = free_fu;
                    total_instructions_fired++;
#if DEBUG_OUTPUT
                    fprintf(stderr, "%lu\tSCHEDULED\t%lu\n", current_cycle, inst.tag);
#endif
                    // schedule_cycle was already set when it entered RS
                }
        }
        
        // 4. Dispatch - reserve slots in scheduling queue
        // Count how many instructions will be deleted this cycle
        size_t to_be_deleted = 0;
        for (const auto& inst : scheduling_queue) {
            if (inst.delete_cycle == current_cycle) {
                to_be_deleted++;
            }
        }
        
        while (!dispatch_queue.empty() && scheduling_queue.size() - to_be_deleted < scheduling_queue_size) {
            auto inst = dispatch_queue.front();
            dispatch_queue.pop_front();
            // Track when it enters RS (next cycle after dispatch)
            inst.rs_entry_cycle = current_cycle + 1;
            // schedule_cycle in output is when it enters RS
            inst.schedule_cycle = current_cycle + 1;
            scheduling_queue.push_back(inst);
#if DEBUG_OUTPUT
            fprintf(stderr, "%lu\tDISPATCHED\t%lu\n", current_cycle, inst.tag);
#endif
        }
        
        // ===== SECOND HALF OF CYCLE =====
        
        // 5. Delete instructions whose delete cycle has arrived (after state update)
        // FUs were already freed when state update happened
        for (int i = scheduling_queue.size() - 1; i >= 0; i--) {
            if (scheduling_queue[i].delete_cycle == current_cycle) {
                auto& inst = scheduling_queue[i];
                
                // Save to completed list before deleting
                completed_instructions.push_back(inst);
                scheduling_queue.erase(scheduling_queue.begin() + i);
            }
        }
        
        // 6. Fetch - read new instructions
        if (!fetching_done) {
            for (uint64_t i = 0; i < fetch_rate; i++) {
                proc_inst_t inst;
                memset(&inst, 0, sizeof(proc_inst_t));
                if (read_instruction(&inst)) {
                    inst.tag = next_tag++;
                    inst.fetch_cycle = current_cycle;
                    // dispatch_cycle is when it enters dispatch queue (next cycle after fetch)
                    inst.dispatch_cycle = current_cycle + 1;
                    inst.fu_type = (inst.op_code == -1) ? 1 : inst.op_code;
                    inst.fired = false;
                    inst.completed = false;
                    inst.fu_index = -1;
                    inst.fu_to_free = false;
                    inst.rs_entry_cycle = 0;
                    inst.schedule_cycle = 0;
                    inst.fire_cycle = 0;
                    inst.execute_cycle = 0;
                    inst.state_update_cycle = 0;
                    inst.complete_cycle = 0;
                    inst.delete_cycle = 0;
                    
                    // Record dependencies - find last writer for each source register
                    for (int j = 0; j < 2; j++) {
                        if (inst.src_reg[j] != -1) {
                            // Check if there's a last writer for this register
                            auto it = last_writer_map.find(inst.src_reg[j]);
                            if (it != last_writer_map.end()) {
                                inst.src_writer_tag[j] = it->second;
                            } else {
                                inst.src_writer_tag[j] = 0; // No dependency
                            }
                        } else {
                            inst.src_writer_tag[j] = 0;
                        }
                    }
                    
                    // Update last writer map
                    if (inst.dest_reg != -1) {
                        last_writer_map[inst.dest_reg] = inst.tag;
                    }
                    
                    dispatch_queue.push_back(inst);
#if DEBUG_OUTPUT
                    fprintf(stderr, "%lu\tFETCHED\t%lu\n", current_cycle, inst.tag);
#endif
                } else {
                    fetching_done = true;
                    break;
                }
            }
        }
        
        // Check if we're done
        if (fetching_done && dispatch_queue.empty() && scheduling_queue.empty()) {
            break;
        }
    }
    
    p_stats->cycle_count = current_cycle;
}

void complete_proc(proc_stats_t *p_stats) 
{
    p_stats->retired_instruction = total_instructions_retired;
    p_stats->max_disp_size = max_dispatch_queue_size;
    // Subtract the last cycle's dispatch queue size (which should be 0)
    p_stats->total_disp_size = total_dispatch_queue_size - 0;  // Last cycle has empty dispatch queue
    p_stats->total_fired = total_instructions_fired;
    
    if (p_stats->cycle_count > 0) {
        p_stats->avg_inst_fired = static_cast<double>(total_instructions_fired) / static_cast<double>(p_stats->cycle_count);
        p_stats->avg_inst_retired = static_cast<double>(total_instructions_retired) / static_cast<double>(p_stats->cycle_count);
        p_stats->avg_disp_size = static_cast<double>(total_dispatch_queue_size) / static_cast<double>(p_stats->cycle_count);
    } else {
        p_stats->avg_inst_fired = 0.0;
        p_stats->avg_inst_retired = 0.0;
        p_stats->avg_disp_size = 0.0;
    }
    
    // Print all instructions in tag order (COMMENTED OUT FOR GRADESCOPE)
    // std::sort(completed_instructions.begin(), completed_instructions.end(),
    //           [](const proc_inst_t& a, const proc_inst_t& b) { return a.tag < b.tag; });
    
    // printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");
    // for (const auto& inst : completed_instructions) {
    //     printf("%lu\t%lu\t%lu\t%lu\t%lu\t%lu\n", 
    //            inst.tag, inst.fetch_cycle, inst.dispatch_cycle, 
    //            inst.schedule_cycle, inst.execute_cycle, inst.state_update_cycle);
    // }
    // printf("\n");  // Blank line after instruction table
}
