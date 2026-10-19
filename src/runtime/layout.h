/* Emulated address-space layout. Must match the constants in tools/recomp.py. */
#pragma once

#define LOWMEM_END      0x00004000u
#define SEG_BASE0       0x00010000u   /* CODE segments are loaded from here */
#define A5_WORLD        0x00080000u   /* below-A5 globals start here        */
#define QD_GLOBALS      0x000F0000u   /* QuickDraw globals record           */
#define STACK_LIMIT     0x00100000u
#define STACK_TOP       0x00200000u
#define HEAP_START      0x00200000u   /* Memory Manager heap up to MEM_SIZE */

/* Fake code addresses (never dereferenced as memory) */
#define TRAP_THUNK_BASE 0x7F000000u   /* GetTrapAddress results: base + trap*4 */
#define HLE_PROC_BASE   0x7E000000u   /* procs supplied by the HLE layer        */
#define MAGIC_RETURN    0x7DFFFFF0u   /* return address used for HLE callbacks  */

void hle_proc_call(uint32_t addr);
void cpu_init(void);
void cpu_run_main(void);
const char *func_name_for(uint32_t addr);
