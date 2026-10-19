#include "cpu.h"
#include "image.h"
#include "layout.h"
#include "../platform/log.h"
#include <stdlib.h>
#include <string.h>

uint32_t D[8], A[8];
uint8_t fN, fZ, fV, fC, fX;
uint8_t *MEM;

static const RecompFunc *find_func(uint32_t addr)
{
    unsigned lo = 0, hi = g_recomp_nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_recomp_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_recomp_nfuncs && g_recomp_funcs[lo].addr == addr) return &g_recomp_funcs[lo];
    return NULL;
}

const char *func_name_for(uint32_t addr)
{
    const RecompFunc *f = find_func(addr);
    return f ? f->name : NULL;
}

/* Resolve jump-table slots (JMP abs.l) to their targets. */
static uint32_t resolve(uint32_t addr)
{
    for (int i = 0; i < 4; i++) {
        if (rdw(addr) == 0x4EF9 && !find_func(addr)) addr = rdl(addr + 2);
        else break;
    }
    return addr;
}

void call_addr(uint32_t addr)
{
    if (addr >= TRAP_THUNK_BASE && addr < TRAP_THUNK_BASE + 0x10000 * 4) {
        /* Toolbox routine called through a ProcPtr: drop the return address,
         * then behave like the inline A-line trap. */
        A[7] += 4;
        trap_dispatch((uint16_t)((addr - TRAP_THUNK_BASE) / 4));
        return;
    }
    if (addr >= HLE_PROC_BASE && addr < HLE_PROC_BASE + 0x1000) {
        A[7] += 4;
        hle_proc_call(addr);
        return;
    }
    uint32_t t = resolve(addr);
    const RecompFunc *f = find_func(t);
    if (!f) {
        log_fatal("call to unknown address 0x%08X (resolved 0x%08X)", addr, t);
        return;
    }
    f->fn();
}

void jump_addr(uint32_t addr)
{
    uint32_t t = resolve(addr);
    if (find_func(t) || (addr >= TRAP_THUNK_BASE && addr < TRAP_THUNK_BASE + 0x40000) ||
        (addr >= HLE_PROC_BASE && addr < HLE_PROC_BASE + 0x1000)) {
        /* tail call: our caller's return address is already on top of the
         * stack, exactly where the callee's rts expects it */
        call_addr(addr);
        return;
    }
    /* otherwise this is a "return to caller" (Pascal callee cleanup idiom) */
}

void call_emulated(uint32_t addr)
{
    PUSH32(MAGIC_RETURN);
    call_addr(addr);
}

void div_zero(void)
{
    log_warn("division by zero in recompiled code");
}

void ccr_op(int op, uint32_t v)
{
    uint32_t ccr = (fX << 4) | (fN << 3) | (fZ << 2) | (fV << 1) | fC;
    if (op == 0) ccr &= v;
    else if (op == 1) ccr |= v;
    else ccr ^= v;
    fX = (ccr >> 4) & 1; fN = (ccr >> 3) & 1; fZ = (ccr >> 2) & 1; fV = (ccr >> 1) & 1; fC = ccr & 1;
}

void unimpl_at(uint32_t addr)
{
    log_fatal("unimplemented instruction at 0x%08X", addr);
}

void cpu_init(void)
{
    MEM = (uint8_t *)calloc(1, MEM_SIZE + 16);
    if (!MEM) log_fatal("out of memory allocating emulated RAM");
    for (unsigned i = 0; i < g_image_nchunks; i++)
        memcpy(MEM + g_image_chunks[i].addr, g_image_chunks[i].data, g_image_chunks[i].len);
    memset(D, 0, sizeof D);
    memset(A, 0, sizeof A);
    A[5] = g_image_a5;
    A[7] = STACK_TOP - 16;
}

void cpu_run_main(void)
{
    call_emulated(g_image_main);
}

uint32_t recomp_func_by_name(const char *name)
{
    for (unsigned i = 0; i < g_recomp_nfuncs; i++)
        if (g_recomp_funcs[i].name && strcmp(g_recomp_funcs[i].name, name) == 0) return g_recomp_funcs[i].addr;
    return 0;
}

void hle_proc_call(uint32_t addr)
{
    log_fatal("call to HLE proc 0x%08X", addr);
}
