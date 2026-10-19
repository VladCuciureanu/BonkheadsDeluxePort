/* Memory Manager: a single application heap living in emulated RAM.
 *
 * Every block carries a 16-byte header directly before its data:
 *   +0  u32 physical block size (header included, multiple of 16)
 *   +4  u32 logical size requested by the program
 *   +8  u8  kind (0 free, 1 pointer, 2 relocatable, 3 master-pointer block)
 *   +9  u8  handle state flags (HF_LOCKED/HF_PURGE/HF_RESOURCE)
 *   +12 u32 owning handle (master pointer address) for relocatable blocks
 * Blocks tile the heap contiguously so the heap can be walked linearly.
 * Handles are addresses of master pointers that hold the data address.
 */
#include "toolbox.h"

#define HDR 16u
#define KIND_FREE 0
#define KIND_PTR 1
#define KIND_REL 2
#define KIND_MP 3

static uint32_t heap_lo, heap_hi;
static uint32_t rover;
static uint32_t free_mps;      /* free list of master pointers (linked through the MP itself) */

static inline uint32_t blk_phys(uint32_t b) { return rdl(b); }
static inline uint32_t blk_kind(uint32_t b) { return rdb(b + 8); }

void mm_set_err(int16_t e) { wrw(LM_MemErr, (uint16_t)e); }

static void make_block(uint32_t b, uint32_t phys, uint32_t kind)
{
    wrl(b, phys); wrl(b + 4, 0); wrb(b + 8, kind); wrb(b + 9, 0); wrw(b + 10, 0); wrl(b + 12, 0);
}

void mm_init(void)
{
    heap_lo = HEAP_START;
    heap_hi = MEM_SIZE - 0x10000;
    make_block(heap_lo, heap_hi - heap_lo, KIND_FREE);
    rover = heap_lo;
    free_mps = 0;
    wrl(LM_ApplZone, heap_lo);
    wrl(LM_TheZone, heap_lo);
    wrl(LM_SysZone, heap_lo);
    wrl(LM_HeapEnd, heap_hi);
    wrl(LM_ApplLimit, heap_hi);
    wrl(LM_MemTop, MEM_SIZE);
    wrl(LM_BufPtr, MEM_SIZE);
    mm_set_err(0);
}

/* merge b with following free blocks */
static void coalesce(uint32_t b)
{
    uint32_t phys = blk_phys(b);
    for (;;) {
        uint32_t n = b + phys;
        if (n >= heap_hi || blk_kind(n) != KIND_FREE) break;
        phys += blk_phys(n);
    }
    wrl(b, phys);
}

static uint32_t find_free(uint32_t need)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t b = pass == 0 ? rover : heap_lo;
        uint32_t end = pass == 0 ? heap_hi : rover;
        while (b < end) {
            uint32_t phys = blk_phys(b);
            if (phys == 0) log_fatal("heap corrupted at 0x%08X", b);
            if (blk_kind(b) == KIND_FREE) {
                coalesce(b);
                phys = blk_phys(b);
                if (phys >= need) return b;
            }
            b += phys;
        }
    }
    return 0;
}

static uint32_t alloc_block(uint32_t size, uint32_t kind, bool clear)
{
    uint32_t need = (size + HDR + 15u) & ~15u;
    if (need < 32) need = 32;
    uint32_t b = find_free(need);
    if (!b) { mm_set_err(memFullErr); return 0; }
    uint32_t phys = blk_phys(b);
    if (phys - need >= 32) {
        make_block(b + need, phys - need, KIND_FREE);
        phys = need;
    }
    make_block(b, phys, kind);
    wrl(b + 4, size);
    rover = b + phys < heap_hi ? b + phys : heap_lo;
    if (clear) memset(HP(b + HDR), 0, size);
    mm_set_err(0);
    return b + HDR;
}

static void free_block(uint32_t data)
{
    uint32_t b = data - HDR;
    wrb(b + 8, KIND_FREE);
    coalesce(b);
}

static bool valid_block(uint32_t data)
{
    return data >= heap_lo + HDR && data < heap_hi && blk_kind(data - HDR) != KIND_FREE;
}

uint32_t mm_new_ptr(uint32_t size, bool clear) { return alloc_block(size, KIND_PTR, clear); }

void mm_dispose_ptr(uint32_t p)
{
    if (!p) return;
    if (!valid_block(p)) { log_warn("DisposePtr on bad pointer 0x%08X", p); return; }
    free_block(p);
    mm_set_err(0);
}

uint32_t mm_ptr_size(uint32_t p)
{
    if (!valid_block(p)) { mm_set_err(memWZErr); return 0; }
    return rdl(p - HDR + 4);
}

static bool grow_in_place(uint32_t data, uint32_t size)
{
    uint32_t b = data - HDR;
    uint32_t need = (size + HDR + 15u) & ~15u;
    uint32_t phys = blk_phys(b);
    if (need <= phys) {
        if (phys - need >= 32) {
            make_block(b + need, phys - need, KIND_FREE);
            coalesce(b + need);
            wrl(b, need);
        }
        wrl(b + 4, size);
        return true;
    }
    uint32_t n = b + phys;
    if (n < heap_hi && blk_kind(n) == KIND_FREE) {
        coalesce(n);
        uint32_t total = phys + blk_phys(n);
        if (total >= need) {
            if (total - need >= 32) {
                make_block(b + need, total - need, KIND_FREE);
                wrl(b, need);
            } else {
                wrl(b, total);
            }
            wrl(b + 4, size);
            return true;
        }
    }
    return false;
}

bool mm_set_ptr_size(uint32_t p, uint32_t size)
{
    if (!valid_block(p)) { mm_set_err(memWZErr); return false; }
    if (grow_in_place(p, size)) { mm_set_err(0); return true; }
    mm_set_err(memFullErr);
    return false;
}

/* ---- master pointers ---- */
static uint32_t new_master_pointer(void)
{
    if (!free_mps) {
        uint32_t blk = alloc_block(64 * 4, KIND_MP, true);
        if (!blk) return 0;
        for (int i = 63; i >= 0; i--) {
            uint32_t mp = blk + i * 4;
            wrl(mp, free_mps);
            free_mps = mp;
        }
    }
    uint32_t mp = free_mps;
    free_mps = rdl(mp);
    wrl(mp, 0);
    return mp;
}

static void release_master_pointer(uint32_t mp)
{
    wrl(mp, free_mps);
    free_mps = mp;
}

uint32_t mm_new_empty_handle(void)
{
    uint32_t mp = new_master_pointer();
    if (!mp) { mm_set_err(memFullErr); return 0; }
    mm_set_err(0);
    return mp;
}

uint32_t mm_new_handle(uint32_t size, bool clear)
{
    uint32_t mp = new_master_pointer();
    if (!mp) { mm_set_err(memFullErr); return 0; }
    uint32_t data = alloc_block(size, KIND_REL, clear);
    if (!data) { release_master_pointer(mp); mm_set_err(memFullErr); return 0; }
    wrl(data - HDR + 12, mp);
    wrl(mp, data);
    mm_set_err(0);
    return mp;
}

bool mm_is_handle(uint32_t h)
{
    if (!h || h < heap_lo || h >= heap_hi) return false;
    uint32_t data = rdl(h);
    if (data == 0) return true; /* empty handle */
    return valid_block(data) && blk_kind(data - HDR) == KIND_REL && rdl(data - HDR + 12) == h;
}

void mm_dispose_handle(uint32_t h)
{
    if (!h) { mm_set_err(nilHandleErr); return; }
    if (!mm_is_handle(h)) { log_warn("DisposeHandle on bad handle 0x%08X", h); mm_set_err(nilHandleErr); return; }
    uint32_t data = rdl(h);
    if (data) free_block(data);
    release_master_pointer(h);
    mm_set_err(0);
}

uint32_t mm_handle_size(uint32_t h)
{
    if (!mm_is_handle(h)) { mm_set_err(nilHandleErr); return 0; }
    uint32_t data = rdl(h);
    mm_set_err(0);
    return data ? rdl(data - HDR + 4) : 0;
}

bool mm_set_handle_size(uint32_t h, uint32_t size)
{
    if (!mm_is_handle(h)) { mm_set_err(nilHandleErr); return false; }
    uint32_t data = rdl(h);
    if (!data) {
        data = alloc_block(size, KIND_REL, false);
        if (!data) return false;
        wrl(data - HDR + 12, h);
        wrl(h, data);
        return true;
    }
    if (grow_in_place(data, size)) { mm_set_err(0); return true; }
    uint8_t flags = rdb(data - HDR + 9);
    if (flags & HF_LOCKED) log_warn("SetHandleSize moving a locked handle 0x%08X", h);
    uint32_t nd = alloc_block(size, KIND_REL, false);
    if (!nd) return false;
    uint32_t old = rdl(data - HDR + 4);
    memcpy(HP(nd), HP(data), old < size ? old : size);
    wrb(nd - HDR + 9, flags);
    wrl(nd - HDR + 12, h);
    free_block(data);
    wrl(h, nd);
    mm_set_err(0);
    return true;
}

uint32_t mm_recover_handle(uint32_t p)
{
    if (!valid_block(p) || blk_kind(p - HDR) != KIND_REL) { mm_set_err(memWZErr); return 0; }
    mm_set_err(0);
    return rdl(p - HDR + 12);
}

uint8_t mm_hget_state(uint32_t h)
{
    if (!mm_is_handle(h) || !rdl(h)) return 0;
    return rdb(rdl(h) - HDR + 9);
}

void mm_hset_state(uint32_t h, uint8_t s)
{
    if (!mm_is_handle(h) || !rdl(h)) return;
    wrb(rdl(h) - HDR + 9, s);
}

uint32_t mm_handle_from_bytes(const void *data, uint32_t len)
{
    uint32_t h = mm_new_handle(len, false);
    if (h && len) memcpy(HP(rdl(h)), data, len);
    return h;
}

uint32_t mm_ptr_from_bytes(const void *data, uint32_t len)
{
    uint32_t p = mm_new_ptr(len, false);
    if (p && len) memcpy(HP(p), data, len);
    return p;
}

uint32_t mm_free_bytes(void)
{
    uint32_t total = 0;
    for (uint32_t b = heap_lo; b < heap_hi; b += blk_phys(b))
        if (blk_kind(b) == KIND_FREE) total += blk_phys(b) - HDR;
    return total;
}

/* ================= traps ================= */
/* OS traps: register based. Flag bit 9 (0x0200) = clear, bit 10 (0x0400) = sys heap */

static void T_NewPtr(uint16_t op)
{
    A[0] = mm_new_ptr(D[0], (op & 0x0200) != 0);
    os_result((int16_t)rdw(LM_MemErr));
}
static void T_DisposePtr(uint16_t op) { (void)op; mm_dispose_ptr(A[0]); os_result(0); }
static void T_GetPtrSize(uint16_t op)
{
    (void)op;
    uint32_t s = mm_ptr_size(A[0]);
    D[0] = s ? s : (uint32_t)(int32_t)(int16_t)rdw(LM_MemErr);
    fN = (D[0] >> 31) & 1; fZ = D[0] == 0;
}
static void T_SetPtrSize(uint16_t op) { (void)op; mm_set_ptr_size(A[0], D[0]); os_result((int16_t)rdw(LM_MemErr)); }
static void T_NewHandle(uint16_t op)
{
    A[0] = mm_new_handle(D[0], (op & 0x0200) != 0);
    os_result((int16_t)rdw(LM_MemErr));
}
static void T_NewEmptyHandle(uint16_t op) { (void)op; A[0] = mm_new_empty_handle(); os_result((int16_t)rdw(LM_MemErr)); }
static void T_DisposeHandle(uint16_t op) { (void)op; mm_dispose_handle(A[0]); os_result((int16_t)rdw(LM_MemErr)); }
static void T_GetHandleSize(uint16_t op)
{
    (void)op;
    uint32_t s = mm_handle_size(A[0]);
    int16_t e = (int16_t)rdw(LM_MemErr);
    D[0] = e ? (uint32_t)(int32_t)e : s;
    fN = (D[0] >> 31) & 1; fZ = D[0] == 0;
}
static void T_SetHandleSize(uint16_t op) { (void)op; mm_set_handle_size(A[0], D[0]); os_result((int16_t)rdw(LM_MemErr)); }
static void T_ReallocHandle(uint16_t op)
{
    (void)op;
    uint32_t h = A[0];
    if (rdl(h)) { free_block(rdl(h)); wrl(h, 0); }
    mm_set_handle_size(h, D[0]);
    os_result((int16_t)rdw(LM_MemErr));
}
static void T_EmptyHandle(uint16_t op)
{
    (void)op;
    uint32_t h = A[0];
    if (mm_is_handle(h) && rdl(h)) { free_block(rdl(h)); wrl(h, 0); }
    os_result(0);
}
static void T_RecoverHandle(uint16_t op) { (void)op; A[0] = mm_recover_handle(A[0]); D[0] = 0; }
static void T_HLock(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) | HF_LOCKED); os_result(0); }
static void T_HUnlock(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) & ~HF_LOCKED); os_result(0); }
static void T_HPurge(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) | HF_PURGE); os_result(0); }
static void T_HNoPurge(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) & ~HF_PURGE); os_result(0); }
static void T_HSetRBit(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) | HF_RESOURCE); os_result(0); }
static void T_HClrRBit(uint16_t op) { (void)op; mm_hset_state(A[0], mm_hget_state(A[0]) & ~HF_RESOURCE); os_result(0); }
static void T_HGetState(uint16_t op) { (void)op; D[0] = mm_hget_state(A[0]); fZ = D[0] == 0; fN = 0; }
static void T_HSetState(uint16_t op) { (void)op; mm_hset_state(A[0], (uint8_t)D[0]); os_result(0); }
static void T_MoveHHi(uint16_t op) { (void)op; os_result(0); }
static void T_FreeMem(uint16_t op) { (void)op; uint32_t f = mm_free_bytes(); D[0] = f > 0x1000000 ? 0x1000000 : f; }
static void T_MaxMem(uint16_t op) { (void)op; uint32_t f = mm_free_bytes(); D[0] = f > 0x1000000 ? 0x1000000 : f; A[0] = 0; }
static void T_MaxBlock(uint16_t op) { (void)op; uint32_t f = mm_free_bytes(); D[0] = f > 0x1000000 ? 0x1000000 : f; }
static void T_CompactMem(uint16_t op) { (void)op; uint32_t f = mm_free_bytes(); D[0] = f > 0x1000000 ? 0x1000000 : f; }
static void T_PurgeSpace(uint16_t op) { (void)op; uint32_t f = mm_free_bytes(); D[0] = A[0] = f > 0x1000000 ? 0x1000000 : f; }
static void T_noop_os(uint16_t op) { (void)op; os_result(0); }
static void T_BlockMove(uint16_t op)
{
    (void)op;
    if ((int32_t)D[0] > 0) memmove(HP(A[1]), HP(A[0]), D[0]);
    os_result(0);
}
static void T_StripAddress(uint16_t op) { (void)op; }
static void T_GetZone(uint16_t op) { (void)op; A[0] = heap_lo; os_result(0); }
static void T_StackSpace(uint16_t op) { (void)op; D[0] = A[7] - STACK_LIMIT; }
static void T_HandleZone(uint16_t op) { (void)op; A[0] = heap_lo; os_result(0); }
static void T_PtrZone(uint16_t op) { (void)op; A[0] = heap_lo; os_result(0); }

/* toolbox (stack-based) memory utilities */
static void T_HandToHand(uint16_t op)
{
    (void)op;
    uint32_t h = A[0];
    uint32_t s = mm_handle_size(h);
    uint32_t n = mm_new_handle(s, false);
    if (n) memcpy(HP(rdl(n)), HP(rdl(h)), s);
    A[0] = n;
    os_result(n ? 0 : memFullErr);
}
static void T_PtrToHand(uint16_t op)
{
    (void)op;
    uint32_t n = mm_new_handle(D[0], false);
    if (n) memcpy(HP(rdl(n)), HP(A[0]), D[0]);
    A[0] = n;
    os_result(n ? 0 : memFullErr);
}
static void T_PtrToXHand(uint16_t op)
{
    (void)op;
    uint32_t h = A[1];
    if (!mm_set_handle_size(h, D[0])) { os_result(memFullErr); return; }
    memcpy(HP(rdl(h)), HP(A[0]), D[0]);
    A[0] = h;
    os_result(0);
}
static void T_HandAndHand(uint16_t op)
{
    (void)op;
    uint32_t a = A[0], b = A[1];
    uint32_t sa = mm_handle_size(a), sb = mm_handle_size(b);
    if (!mm_set_handle_size(b, sa + sb)) { os_result(memFullErr); return; }
    memcpy(HP(rdl(b) + sb), HP(rdl(a)), sa);
    A[0] = b;
    os_result(0);
}
static void T_PtrAndHand(uint16_t op)
{
    (void)op;
    uint32_t h = A[1];
    uint32_t s = mm_handle_size(h);
    if (!mm_set_handle_size(h, s + D[0])) { os_result(memFullErr); return; }
    memcpy(HP(rdl(h) + s), HP(A[0]), D[0]);
    A[0] = h;
    os_result(0);
}

void mem_register_traps(void)
{
    trap_register(0xA11E, T_NewPtr, "NewPtr");
    trap_register(0xA01F, T_DisposePtr, "DisposePtr");
    trap_register(0xA021, T_GetPtrSize, "GetPtrSize");
    trap_register(0xA020, T_SetPtrSize, "SetPtrSize");
    trap_register(0xA122, T_NewHandle, "NewHandle");
    trap_register(0xA166, T_NewEmptyHandle, "NewEmptyHandle");
    trap_register(0xA023, T_DisposeHandle, "DisposeHandle");
    trap_register(0xA025, T_GetHandleSize, "GetHandleSize");
    trap_register(0xA024, T_SetHandleSize, "SetHandleSize");
    trap_register(0xA027, T_ReallocHandle, "ReallocHandle");
    trap_register(0xA02B, T_EmptyHandle, "EmptyHandle");
    trap_register(0xA128, T_RecoverHandle, "RecoverHandle");
    trap_register(0xA029, T_HLock, "HLock");
    trap_register(0xA02A, T_HUnlock, "HUnlock");
    trap_register(0xA049, T_HPurge, "HPurge");
    trap_register(0xA04A, T_HNoPurge, "HNoPurge");
    trap_register(0xA067, T_HSetRBit, "HSetRBit");
    trap_register(0xA068, T_HClrRBit, "HClrRBit");
    trap_register(0xA069, T_HGetState, "HGetState");
    trap_register(0xA06A, T_HSetState, "HSetState");
    trap_register(0xA064, T_MoveHHi, "MoveHHi");
    trap_register(0xA01C, T_FreeMem, "FreeMem");
    trap_register(0xA11D, T_MaxMem, "MaxMem");
    trap_register(0xA061, T_MaxBlock, "MaxBlock");
    trap_register(0xA04C, T_CompactMem, "CompactMem");
    trap_register(0xA162, T_PurgeSpace, "PurgeSpace");
    trap_register(0xA04D, T_noop_os, "PurgeMem");
    trap_register(0xA036, T_noop_os, "MoreMasters");
    trap_register(0xA063, T_noop_os, "MaxApplZone");
    trap_register(0xA02D, T_noop_os, "SetApplLimit");
    trap_register(0xA040, T_noop_os, "ResrvMem");
    trap_register(0xA02E, T_BlockMove, "BlockMove");
    trap_register(0xA055, T_StripAddress, "StripAddress");
    trap_register(0xA11A, T_GetZone, "GetZone");
    trap_register(0xA01B, T_noop_os, "SetZone");
    trap_register(0xA065, T_StackSpace, "StackSpace");
    trap_register(0xA126, T_HandleZone, "HandleZone");
    trap_register(0xA148, T_PtrZone, "PtrZone");
    trap_register(0xA9E1, T_HandToHand, "HandToHand");
    trap_register(0xA9E3, T_PtrToHand, "PtrToHand");
    trap_register(0xA9E2, T_PtrToXHand, "PtrToXHand");
    trap_register(0xA9E4, T_HandAndHand, "HandAndHand");
    trap_register(0xA9EF, T_PtrAndHand, "PtrAndHand");
}
