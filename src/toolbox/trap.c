/* A-line trap dispatch to the HLE implementations. */
#include "toolbox.h"

typedef struct { TrapFn fn; const char *name; } TrapEnt;
static TrapEnt os_traps[256];
static TrapEnt tb_traps[1024];
static unsigned unknown_reported[4096 / 32];

void trap_register(uint16_t op, TrapFn fn, const char *name)
{
    if (op & 0x0800) { tb_traps[op & 0x3FF].fn = fn; tb_traps[op & 0x3FF].name = name; }
    else { os_traps[op & 0xFF].fn = fn; os_traps[op & 0xFF].name = name; }
}

static uint16_t trap_from_number(uint16_t op, uint32_t num)
{
    /* GetTrapAddress flavours: bit 9 set = new style, bit 10 = Toolbox table */
    if (op & 0x0200) return (op & 0x0400) ? (uint16_t)(0xA800 | (num & 0x3FF)) : (uint16_t)(0xA000 | (num & 0xFF));
    num &= 0x3FF;
    if (num < 0x50) return (uint16_t)(0xA000 | num);
    return (uint16_t)(0xA800 | num);
}

static void T_GetTrapAddress(uint16_t op)
{
    uint16_t t = trap_from_number(op, D[0]);
    TrapEnt *e = (t & 0x0800) ? &tb_traps[t & 0x3FF] : &os_traps[t & 0xFF];
    if (!e->fn) t = 0xA89F; /* report unimplemented traps as _Unimplemented */
    A[0] = TRAP_THUNK_BASE + (uint32_t)t * 4;
    os_result(0);
}

static void T_SetTrapAddress(uint16_t op)
{
    log_trace("SetTrapAddress(%04X -> %08X) ignored", trap_from_number(op, D[0]), A[0]);
    os_result(0);
}

static void T_Unimplemented(uint16_t op)
{
    (void)op;
    log_fatal("_Unimplemented trap called");
}

void trap_dispatch(uint16_t op)
{
    TrapEnt *e = (op & 0x0800) ? &tb_traps[op & 0x3FF] : &os_traps[op & 0xFF];
    if (!e->fn) {
        unsigned idx = op & 0xFFF;
        if (!(unknown_reported[idx / 32] & (1u << (idx % 32)))) {
            unknown_reported[idx / 32] |= 1u << (idx % 32);
        }
        log_fatal("unimplemented Toolbox trap $%04X", op);
        return;
    }
    if (g_log_trace) log_trace("trap %s ($%04X) sp=%08X", e->name, op, A[7]);
    e->fn(op);
    if ((op & 0x0C00) == 0x0C00) {
        /* auto-pop: return straight to the caller of the glue routine */
        log_warn("auto-pop trap $%04X not supported", op);
    }
}

const char *trap_name(uint16_t op)
{
    TrapEnt *e = (op & 0x0800) ? &tb_traps[op & 0x3FF] : &os_traps[op & 0xFF];
    return e->name ? e->name : "?";
}

void trap_register_core(void)
{
    trap_register(0xA146, T_GetTrapAddress, "GetTrapAddress");
    trap_register(0xA047, T_SetTrapAddress, "SetTrapAddress");
    trap_register(0xA89F, T_Unimplemented, "Unimplemented");
}
