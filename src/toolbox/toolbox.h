/* Shared declarations for the high-level emulation (HLE) of the Mac Toolbox. */
#pragma once
#include "runtime/cpu.h"
#include "runtime/layout.h"
#include "platform/log.h"
#include <stdbool.h>
#include <string.h>

typedef void (*TrapFn)(uint16_t op);
void trap_register(uint16_t op, TrapFn fn, const char *name);
void toolbox_init(void);

/* ---- Pascal-convention argument helpers (args popped in reverse order) ---- */
#define ARG8()   ((uint8_t)(POP16() >> 8))   /* Boolean/char occupies the high byte */
#define ARG16()  ((uint16_t)POP16())
#define ARGS16() ((int16_t)POP16())
#define ARG32()  ((uint32_t)POP32())
/* v is evaluated before A[7] is read: it may itself pop arguments (ARG32() etc.) */
#define RET8(v)  do { uint32_t r_ = (uint8_t)(v); wrw(A[7], r_ << 8); } while (0)
#define RET16(v) do { uint32_t r_ = (uint16_t)(v); wrw(A[7], r_); } while (0)
#define RET32(v) do { uint32_t r_ = (uint32_t)(v); wrl(A[7], r_); } while (0)

/* OS trap result in D0 with condition codes as after tst.w d0 */
static inline void os_result(int16_t err)
{
    D[0] = (uint32_t)(int32_t)err;
    fN = err < 0; fZ = err == 0; fV = 0; fC = 0;
}

/* ---- low-memory globals ---- */
#define LM_MemErr      0x0220
#define LM_Ticks       0x016A
#define LM_CurrentA5   0x0904
#define LM_ResErr      0x0A60
#define LM_MBarHeight  0x0BAA
#define LM_ScrnBase    0x0824
#define LM_MainDevice  0x08A4
#define LM_TheGDevice  0x0CC8
#define LM_DeviceList  0x08A8
#define LM_RndSeed     0x0156
#define LM_KeyMap      0x0174
#define LM_Time        0x020C
#define LM_ROM85       0x028E
#define LM_CurApName   0x0910
#define LM_WindowList  0x09D6
#define LM_GrayRgn     0x09EE
#define LM_CurResFile  0x0A5A
#define LM_TopMapHndl  0x0A50
#define LM_SysMap      0x0A58
#define LM_ApplZone    0x02AA
#define LM_SysZone     0x02A6
#define LM_TheZone     0x0118
#define LM_HeapEnd     0x0114
#define LM_MemTop      0x0108
#define LM_BufPtr      0x010C
#define LM_ApplLimit   0x0130
#define LM_CurStackBase 0x0908
#define LM_DlgFont     0x0AFA
#define LM_ResLoad     0x0A5E
#define LM_SysEvtMask  0x0144
#define LM_TheMenu     0x0A26
#define LM_MenuList    0x0A1C
#define LM_DAStrings   0x0AA0

/* ---- Memory Manager (mem.c) ---- */
enum { memFullErr = -108, nilHandleErr = -109, memWZErr = -111, resNotFound = -192, fnfErr = -43,
       paramErr = -50, noErr = 0, eofErr = -39, ioErr = -36, dupFNErr = -48, resFNotFound = -193 };
#define HF_LOCKED   0x80
#define HF_PURGE    0x40
#define HF_RESOURCE 0x20

void mm_init(void);
uint32_t mm_new_ptr(uint32_t size, bool clear);
void mm_dispose_ptr(uint32_t p);
uint32_t mm_ptr_size(uint32_t p);
bool mm_set_ptr_size(uint32_t p, uint32_t size);
uint32_t mm_new_handle(uint32_t size, bool clear);
uint32_t mm_new_empty_handle(void);
void mm_dispose_handle(uint32_t h);
uint32_t mm_handle_size(uint32_t h);
bool mm_set_handle_size(uint32_t h, uint32_t size);
uint32_t mm_recover_handle(uint32_t p);
uint8_t mm_hget_state(uint32_t h);
void mm_hset_state(uint32_t h, uint8_t s);
bool mm_is_handle(uint32_t h);
uint32_t mm_handle_from_bytes(const void *data, uint32_t len);
uint32_t mm_free_bytes(void);
void mm_set_err(int16_t e);
/* allocate a nonrelocatable block and copy host bytes into it */
uint32_t mm_ptr_from_bytes(const void *data, uint32_t len);

/* ---- helpers ---- */
static inline uint32_t deref(uint32_t h) { return rdl(h); }
/* Pascal string at emulated address -> C string (max 255) */
void pstr_to_c(uint32_t p, char *out, size_t outsz);
void c_to_pstr(const char *s, uint32_t p);
uint32_t ticks_now(void);
