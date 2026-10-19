/* Emulated 68k register file, flags and big-endian memory used by the
 * statically recompiled game code (build/gen/recomp_*.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER)
#define FORCEINLINE static __forceinline
#else
#define FORCEINLINE static inline __attribute__((always_inline))
#endif

extern uint32_t D[8], A[8];
extern uint8_t fN, fZ, fV, fC, fX;
extern uint8_t *MEM;

#define MEM_SIZE (64u * 1024u * 1024u)
#define MEM_MASK (MEM_SIZE - 1u)

FORCEINLINE uint32_t rdb(uint32_t a) { return MEM[a & MEM_MASK]; }
FORCEINLINE uint32_t rdw(uint32_t a) { const uint8_t *p = MEM + (a & MEM_MASK); return ((uint32_t)p[0] << 8) | p[1]; }
FORCEINLINE uint32_t rdl(uint32_t a) {
    const uint8_t *p = MEM + (a & MEM_MASK);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
FORCEINLINE void wrb(uint32_t a, uint32_t v) { MEM[a & MEM_MASK] = (uint8_t)v; }
FORCEINLINE void wrw(uint32_t a, uint32_t v) { uint8_t *p = MEM + (a & MEM_MASK); p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
FORCEINLINE void wrl(uint32_t a, uint32_t v) {
    uint8_t *p = MEM + (a & MEM_MASK);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
/* host pointer for bulk access (caller guarantees range) */
FORCEINLINE uint8_t *HP(uint32_t a) { return MEM + (a & MEM_MASK); }

#define PUSH32(v) do { A[7] -= 4; wrl(A[7], (v)); } while (0)
#define PUSH16(v) do { A[7] -= 2; wrw(A[7], (v)); } while (0)
#define POP32() (A[7] += 4, rdl(A[7] - 4))
#define POP16() (A[7] += 2, rdw(A[7] - 2))

/* ---- flags ---- */
#define FLG_LOGIC_B(v) do { uint32_t v_ = (v); fN = (v_ >> 7) & 1; fZ = (v_ & 0xFF) == 0; fV = 0; fC = 0; } while (0)
#define FLG_LOGIC_W(v) do { uint32_t v_ = (v); fN = (v_ >> 15) & 1; fZ = (v_ & 0xFFFF) == 0; fV = 0; fC = 0; } while (0)
#define FLG_LOGIC_L(v) do { uint32_t v_ = (v); fN = v_ >> 31; fZ = v_ == 0; fV = 0; fC = 0; } while (0)

#define DEF_ARITH(SZ, BITS, MSK)                                                            \
FORCEINLINE uint32_t ADD_##SZ(uint32_t s, uint32_t d) {                                   \
    s &= MSK; d &= MSK; uint32_t r = (d + s) & MSK;                                         \
    fC = fX = (uint8_t)((((uint64_t)d + s) >> BITS) & 1);                                  \
    fV = (uint8_t)(((~(s ^ d)) & (s ^ r)) >> (BITS - 1) & 1);                              \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }                         \
FORCEINLINE uint32_t SUB_##SZ(uint32_t s, uint32_t d) {                                   \
    s &= MSK; d &= MSK; uint32_t r = (d - s) & MSK;                                         \
    fC = fX = s > d;                                                                        \
    fV = (uint8_t)((((s ^ d) & (r ^ d)) >> (BITS - 1)) & 1);                               \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }                         \
FORCEINLINE void CMP_##SZ(uint32_t s, uint32_t d) {                                       \
    s &= MSK; d &= MSK; uint32_t r = (d - s) & MSK;                                         \
    fC = s > d;                                                                             \
    fV = (uint8_t)((((s ^ d) & (r ^ d)) >> (BITS - 1)) & 1);                               \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; }                                   \
FORCEINLINE uint32_t ADDX_##SZ(uint32_t s, uint32_t d) {                                  \
    s &= MSK; d &= MSK; uint64_t w = (uint64_t)d + s + fX; uint32_t r = (uint32_t)w & MSK;  \
    fC = fX = (uint8_t)((w >> BITS) & 1);                                                  \
    fV = (uint8_t)(((~(s ^ d)) & (s ^ r)) >> (BITS - 1) & 1);                              \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); if (r) fZ = 0; return r; }                       \
FORCEINLINE uint32_t SUBX_##SZ(uint32_t s, uint32_t d) {                                  \
    s &= MSK; d &= MSK; uint64_t w = (uint64_t)d - s - fX; uint32_t r = (uint32_t)w & MSK;  \
    fC = fX = (uint8_t)(((uint64_t)s + fX) > d);                                            \
    fV = (uint8_t)((((s ^ d) & (r ^ d)) >> (BITS - 1)) & 1);                               \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); if (r) fZ = 0; return r; }
DEF_ARITH(B, 8, 0xFFu)
DEF_ARITH(W, 16, 0xFFFFu)
DEF_ARITH(L, 32, 0xFFFFFFFFu)

/* shifts: v already masked to size, cnt 0..63 */
#define DEF_SHIFT(SZ, BITS, MSK)                                                              \
FORCEINLINE uint32_t SH_LSL_##SZ(uint32_t v, uint32_t c) {                                  \
    uint32_t r = v; fV = 0;                                                                   \
    if (c == 0) { fC = 0; } else if (c > BITS) { r = 0; fC = fX = 0; }                        \
    else { fC = fX = (uint8_t)((v >> (BITS - c)) & 1); r = (uint32_t)(((uint64_t)v << c) & MSK); } \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = (r & MSK) == 0; return r & MSK; }              \
FORCEINLINE uint32_t SH_LSR_##SZ(uint32_t v, uint32_t c) {                                  \
    uint32_t r = v; fV = 0;                                                                   \
    if (c == 0) { fC = 0; } else if (c > BITS) { r = 0; fC = fX = 0; }                        \
    else { fC = fX = (uint8_t)((v >> (c - 1)) & 1); r = (uint32_t)((uint64_t)v >> c); }        \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = (r & MSK) == 0; return r & MSK; }              \
FORCEINLINE uint32_t SH_ASR_##SZ(uint32_t v, uint32_t c) {                                  \
    int64_t sv = (int64_t)(int32_t)(v << (32 - BITS)) >> (32 - BITS); uint32_t r; fV = 0;     \
    if (c == 0) { fC = 0; r = v; }                                                            \
    else { if (c > BITS) c = BITS; fC = fX = (uint8_t)((sv >> (c - 1)) & 1); r = (uint32_t)(sv >> c) & MSK; } \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = (r & MSK) == 0; return r & MSK; }              \
FORCEINLINE uint32_t SH_ASL_##SZ(uint32_t v, uint32_t c) {                                  \
    uint32_t r = SH_LSL_##SZ(v, c);                                                           \
    if (c) { int64_t sv = (int64_t)(int32_t)(v << (32 - BITS)) >> (32 - BITS);              \
        int64_t back = (int64_t)(int32_t)(r << (32 - BITS)) >> (32 - BITS);                   \
        fV = (c >= BITS) ? (sv != 0) : ((back >> 0) != (sv * ((int64_t)1 << c)));             \
    } return r; }                                                                             \
FORCEINLINE uint32_t SH_ROL_##SZ(uint32_t v, uint32_t c) {                                  \
    uint32_t r = v & MSK; fV = 0; fC = 0;                                                     \
    if (c) { c %= BITS; if (c) r = ((r << c) | (r >> (BITS - c))) & MSK; fC = r & 1; }        \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }                           \
FORCEINLINE uint32_t SH_ROR_##SZ(uint32_t v, uint32_t c) {                                  \
    uint32_t r = v & MSK; fV = 0; fC = 0;                                                     \
    if (c) { c %= BITS; if (c) r = ((r >> c) | (r << (BITS - c))) & MSK; fC = (uint8_t)((r >> (BITS - 1)) & 1); } \
    fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }                           \
FORCEINLINE uint32_t SH_ROXL_##SZ(uint32_t v, uint32_t c) {                                 \
    uint32_t r = v & MSK; fV = 0;                                                             \
    for (uint32_t i = 0; i < c; i++) { uint8_t out = (uint8_t)((r >> (BITS - 1)) & 1); r = ((r << 1) | fX) & MSK; fX = out; } \
    fC = fX; fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }                 \
FORCEINLINE uint32_t SH_ROXR_##SZ(uint32_t v, uint32_t c) {                                 \
    uint32_t r = v & MSK; fV = 0;                                                             \
    for (uint32_t i = 0; i < c; i++) { uint8_t out = (uint8_t)(r & 1); r = (r >> 1) | ((uint32_t)fX << (BITS - 1)); fX = out; } \
    fC = fX; fN = (uint8_t)((r >> (BITS - 1)) & 1); fZ = r == 0; return r; }
DEF_SHIFT(B, 8, 0xFFu)
DEF_SHIFT(W, 16, 0xFFFFu)
DEF_SHIFT(L, 32, 0xFFFFFFFFu)

/* 32/16 word divides: dst = rem:quot */
#define DIVS_W(src, r) do { int32_t s_ = (int16_t)(src); if (!s_) { div_zero(); break; }            \
    int32_t n_ = (int32_t)D[r]; int64_t q_ = (int64_t)n_ / s_; int32_t m_ = n_ % s_;                \
    if (q_ < -32768 || q_ > 32767) { fV = 1; fC = 0; break; }                                       \
    D[r] = ((uint32_t)(uint16_t)m_ << 16) | (uint16_t)q_; fV = 0; fC = 0;                           \
    fN = (uint8_t)(((uint16_t)q_ >> 15) & 1); fZ = (uint16_t)q_ == 0; } while (0)
#define DIVU_W(src, r) do { uint32_t s_ = (uint16_t)(src); if (!s_) { div_zero(); break; }          \
    uint32_t n_ = D[r]; uint32_t q_ = n_ / s_, m_ = n_ % s_;                                        \
    if (q_ > 0xFFFF) { fV = 1; fC = 0; break; }                                                     \
    D[r] = (m_ << 16) | q_; fV = 0; fC = 0; fN = (uint8_t)((q_ >> 15) & 1); fZ = q_ == 0; } while (0)
/* 32/32 long divides; rr = remainder register or -1 */
#define DIVSL_L(src, rr, rq) do { int32_t s_ = (int32_t)(src); if (!s_) { div_zero(); break; }       \
    int32_t n_ = (int32_t)D[rq]; int32_t q_, m_;                                                    \
    if (n_ == INT32_MIN && s_ == -1) { q_ = n_; m_ = 0; } else { q_ = n_ / s_; m_ = n_ % s_; }      \
    if ((rr) >= 0 && (rr) != (rq)) D[(rr) & 7] = (uint32_t)m_;                                     \
    D[rq] = (uint32_t)q_; fV = 0; fC = 0; fN = (uint8_t)((uint32_t)q_ >> 31); fZ = q_ == 0; } while (0)
#define DIVUL_L(src, rr, rq) do { uint32_t s_ = (uint32_t)(src); if (!s_) { div_zero(); break; }     \
    uint32_t n_ = D[rq]; uint32_t q_ = n_ / s_, m_ = n_ % s_;                                       \
    if ((rr) >= 0 && (rr) != (rq)) D[(rr) & 7] = m_;                                               \
    D[rq] = q_; fV = 0; fC = 0; fN = (uint8_t)(q_ >> 31); fZ = q_ == 0; } while (0)

void div_zero(void);
void ccr_op(int op, uint32_t v);
/* indirect control flow into recompiled code */
void call_addr(uint32_t addr);   /* caller already pushed return address */
void jump_addr(uint32_t addr);   /* jmp (An): tail call or return-to-caller */
void unimpl_at(uint32_t addr);
#define UNIMPL(a) unimpl_at(a)
void trap_dispatch(uint16_t op);
#define TRAP(op) trap_dispatch(op)

/* call emulated code from C (HLE callbacks). Arguments must already be pushed. */
void call_emulated(uint32_t addr);
