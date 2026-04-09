# Out-of-Order Superscalar Processor Simulator

A cycle-accurate simulator for an N-wide out-of-order superscalar processor. Models a 5-stage pipeline (Fetch → Dispatch → Schedule → Execute → State Update) with a centralized reservation station, multiple functional unit types, result buses (CDBs), and half-cycle pipeline latch behavior.
This project was created for ECE M116C at UCLA. FALL 2025

---

## Pipeline Overview

```
Fetch  →  Dispatch  →  Schedule (RS)  →  Execute  →  State Update
  N            ∞            2(k0+k1+k2)       FUs          out-of-order
insts/cycle  (unlimited)    entries          (latency=1)   up to R/cycle
```

All five stages execute every cycle. Instruction movement happens at the rising clock edge (pipeline latch behavior). An instruction must spend **at least one full cycle** in each stage before advancing.

### Stage Details

**Fetch** — Reads up to F instructions per cycle from the trace into the dispatch queue. No stalls; no branch prediction needed. Instructions are assigned monotonically increasing tags starting at 1.

**Dispatch** — Holds instructions in an unlimited queue until space is available in the RS. Instructions are scanned head-to-tail (program order) and moved into the RS greedily. Data dependencies are recorded at fetch time using a `last_writer_map` that tracks the most recent writer tag for each register.

**Schedule (Reservation Station)** — Centralized RS with capacity `2 × (k0 + k1 + k2)`. An instruction is eligible to fire when:
1. It has spent at least one cycle in the RS (`rs_entry_cycle < current_cycle`), and
2. All source registers' producer instructions have completed state update in a **prior** cycle.

When multiple instructions are ready, they fire in **ascending tag order**, limited by FU availability.

**Execute** — All functional units have a latency of **1 cycle**. A fired instruction completes one cycle after it fires. If an instruction completes but no result bus is available, it waits in the execution stage and its FU remains busy.

**State Update** — Up to R instructions retire per cycle, selected in order of completion cycle first, then ascending tag. State update is **out-of-order**. The FU is freed when an instruction enters state update (first half of cycle). The RS slot is freed in the second half of the same cycle.

### Half-Cycle Behavior

Operations are ordered within each cycle as follows:

| Half | Actions |
|------|---------|
| First | Detect newly completed instructions; retire up to R via result buses (frees FUs); mark ready instructions in RS to fire; dispatch from queue into RS |
| Second | Delete retired instructions from RS; fetch new instructions into dispatch queue |

A dependent instruction whose producer enters state update in cycle T can fire no earlier than cycle T+1.

---

## Functional Units

| Type | Flag | Count | Latency |
|------|------|-------|---------|
| k0 | `-j` | k0 | 1 cycle |
| k1 | `-k` | k1 | 1 cycle |
| k2 | `-l` | k2 | 1 cycle |

An op_code of `-1` in the trace is treated as **FU type 1** (k1). A register number of `-1` means no register for that field (e.g., branches have no destination register).

---

## Default Configuration

| Parameter | Flag | Default |
|-----------|------|---------|
| Fetch rate (F) | `-f` | 4 |
| k0 FUs | `-j` | 1 |
| k1 FUs | `-k` | 2 |
| k2 FUs | `-l` | 3 |
| Result buses (R) | `-r` | 8 |

RS size at default: `2 × (1 + 2 + 3) = 12` entries.

---

## Building and Running

```bash
make
./procsim -r R -f F -j J -k K -l L < trace_file
```

**Example:**
```bash
./procsim -r 8 -f 4 -j 1 -k 2 -l 3 < traces/gcc.trace
```

Output (Gradescope mode) prints only the total cycle count:
```
<total_cycles>
```

---

## Trace Format

Each line represents one instruction:

```
<hex_address> <op_code> <dest_reg> <src1_reg> <src2_reg>
```

- `op_code`: `-1`, `0`, `1`, or `2` (FU type; `-1` → use FU type 1)
- Register numbers: integers in `[0, 127]`, or `-1` for none

**Example:** `ab120024 0 1 2 3` — address `0xab120024`, FU type 0, dest=R1, src1=R2, src2=R3.

---

## File Structure

```
procsim.cpp         — Simulator core: setup_proc(), run_proc(), complete_proc()
procsim.hpp         — Data structures: proc_inst_t, proc_stats_t, defaults, declarations
procsim_driver.cpp  — Entry point: arg parsing, stats printing, Gradescope output
Makefile            — Builds ./procsim
```

Do not create additional `.cpp` or `.hpp` files — the Gradescope autograder expects exactly these three.

---

## Statistics

| Stat | Description |
|------|-------------|
| Total instructions | Total retired instruction count |
| Total cycles | Cycles from first fetch to last state update (off-by-one corrected in driver) |
| Avg inst fired/cycle | `total_fired / cycle_count` |
| Avg inst retired/cycle (IPC) | `total_retired / cycle_count` |
| Avg dispatch queue size | `total_disp_size / cycle_count` |
| Max dispatch queue size | Peak observed dispatch queue depth |

> **Gradescope mode:** `DEBUG_OUTPUT` is set to `0` and `print_statistics()` is commented out. Only the total cycle count is printed. Set `DEBUG_OUTPUT 1` locally to enable per-cycle stage logs on stderr.

---

## Key Implementation Notes

- **Dependency tracking** is done at fetch time. For each source register, the tag of the most recent writer is recorded in `src_writer_tag[]`. Readiness is checked during scheduling by searching the RS and the completed instruction list.
- **WAR and WAW hazards** are not explicitly modeled and can be ignored per the spec.
- **RS slot accounting** during dispatch: slots freed by this cycle's state update are counted before checking capacity, since they free up in the second half of the same cycle.
- **Tag ordering** is enforced in both firing (ascending tag among ready instructions) and retirement (ascending completion cycle, then ascending tag).
- **Cycle count** is decremented by 1 in the driver after `run_proc()` returns, correcting an off-by-one in the loop termination.
