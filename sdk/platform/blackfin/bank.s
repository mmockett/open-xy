/* Sine banks for the Blackfin+ (sdk/include/opxy/bank.hpp has the contract
 * and the portable reference these match bit for bit).
 *
 * Every routine walks the partials two at a time, sample-inner, with the
 * block's accumulator read and written once per sample per pair; an odd last
 * partial runs alone. Phases live in I0/I1 and step by M0/M1 in the parallel
 * slots; the table addresses are formed in P1/P2. The loops are software
 * pipelined so that every P register written from a data register is first
 * used by the address unit at least three instructions later, and every
 * table address is ready at least two instructions before its load: no DAG
 * read-after-write stall on the BF70x pipeline (the simulator models none).
 *
 * ABI (GCC bfin-elf, as divide.s): arguments R0, R1, R2, then [SP + 12],
 * [SP + 16], ... at entry. R4-R7 and P3-P5 are saved; R0-R3, P0-P2, I0-I3,
 * M0-M3, A0, A1, ASTAT, CC, LC0/LT0/LB0 and LC1/LT1/LB1 are clobbered. L0-L3
 * are read as zero and left zero (no circular buffers). B0-B3 are untouched.
 * Uses 16 bytes of stack below the saved registers. No interrupts are masked.
 */

.section .text
.align 2

/* void opxy_bank_sine(i32* acc, u32 frames, const i16* table, u32 shift,
 *                     Partial* partials, u32 count)
 * Partial: phase +0, increment +4, amplitude +8, step +12 (16 bytes).
 * frames >= 1, count >= 1, shift = 32 - bits in 16..31.
 *
 * Pair loop, 13 cycles a sample for two partials:
 *   R0, R1 amplitudes p, q   R2.L -shift   R3 step (reloaded)   R4, R5 index
 *   R6.L, R6.H table values p, q   R7 accumulator word
 *   P0 table   P1, P2 addresses   P3 partial   P4 acc   P5 frames
 *   I0, I1 phases (next sample's)   M0, M1 increments   I2 acc read   I3 acc write
 *   [SP] step p, [SP + 4] step q
 */
.global _opxy_bank_sine
.type _opxy_bank_sine, STT_FUNC
_opxy_bank_sine:
    [--SP] = (R7:4, P5:3);
    SP += -16;
    P4 = R0;                        /* acc */
    P5 = R1;                        /* frames */
    P0 = R2;                        /* table */
    R2 = [SP + 56];                 /* shift (16 + 28 + 12) */
    R2 = -R2;
    P3 = [SP + 60];                 /* partials */
    R3 = [SP + 64];                 /* count */
    [SP + 8] = R3;
    R3 >>= 1;
    CC = R3 == 0;
    IF CC JUMP .Lsine_single;
    P1 = R3;
    LSETUP (.Lsine_pair, .Lsine_pair_end) LC1 = P1;
.Lsine_pair:
    R4 = [P3];                      /* phase p */
    I0 = R4;
    R5 = [P3 + 16];                 /* phase q */
    I1 = R5;
    R4 = LSHIFT R4 BY R2.L;
    R5 = LSHIFT R5 BY R2.L;
    P1 = R4;
    P2 = R5;
    R6 = [P3 + 4];
    M0 = R6;
    R6 = [P3 + 20];
    M1 = R6;
    R0 = [P3 + 8];                  /* amplitude p */
    R1 = [P3 + 24];                 /* amplitude q */
    R6 = [P3 + 12];
    [SP] = R6;                      /* step p */
    R6 = [P3 + 28];
    [SP + 4] = R6;                  /* step q */
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 1);
    P2 = P0 + (P2 << 1);
    I0 += M0;
    I1 += M1;
    LSETUP (.Lsine_pair_sample, .Lsine_pair_sample_end) LC0 = P5;
.Lsine_pair_sample:
    R4 = I0;
    R5 = I1;
    R4 = LSHIFT R4 BY R2.L || R6.L = W[P1] || R7 = [I2++];
    R5 = LSHIFT R5 BY R2.L || R6.H = W[P2] || NOP;
    P1 = R4;
    P2 = R5;
    A0 = R6.L * R0.H (IS) || R3 = [SP] || NOP;
    R4 = (A0 += R6.H * R1.H) (IS) || I0 += M0 || NOP;
    R7 = R7 + R4 (NS) || I1 += M1 || NOP;
    R0 = R0 + R3 (NS) || R3 = [SP + 4] || [I3++] = R7;
    R1 = R1 + R3 (NS);
    P1 = P0 + (P1 << 1);
.Lsine_pair_sample_end:
    P2 = P0 + (P2 << 1);
    I0 -= M0;                       /* I0, I1 ran one sample ahead */
    I1 -= M1;
    R4 = I0;
    R5 = I1;
    [P3] = R4;
    [P3 + 8] = R0;
    [P3 + 16] = R5;
    [P3 + 24] = R1;
.Lsine_pair_end:
    P3 += 32;

.Lsine_single:
    R3 = [SP + 8];
    CC = BITTST (R3, 0);
    IF !CC JUMP .Lsine_done;
    /* One partial, 7 cycles a sample: R0 amplitude, R1 step. */
    R4 = [P3];
    I0 = R4;
    R4 = LSHIFT R4 BY R2.L;
    P1 = R4;
    R6 = [P3 + 4];
    M0 = R6;
    R0 = [P3 + 8];
    R1 = [P3 + 12];
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 1);
    I0 += M0;
    LSETUP (.Lsine_one, .Lsine_one_end) LC0 = P5;
.Lsine_one:
    R4 = I0;
    R4 = LSHIFT R4 BY R2.L || R6.L = W[P1] || NOP;
    P1 = R4;
    R5 = R6.L * R0.H (IS) || I0 += M0 || R7 = [I2++];
    R7 = R7 + R5 (NS);
    R0 = R0 + R1 (NS) || [I3++] = R7;
.Lsine_one_end:
    P1 = P0 + (P1 << 1);
    I0 -= M0;
    R4 = I0;
    [P3] = R4;
    [P3 + 8] = R0;
.Lsine_done:
    SP += 16;
    (R7:4, P5:3) = [SP++];
    RTS;
.size _opxy_bank_sine, .-_opxy_bank_sine

/* void opxy_bank_quadrature(i32* acc, u32 frames, const u32* pairs, u32 shift,
 *                           Pair* partials, u32 count)
 * Pair: phase +0, increment +4, a +8, b +12, step_a +16, step_b +20 (24 bytes).
 * frames >= 1, count >= 1, shift in {20, 21, 22}.
 *
 * Pair loop, 17 cycles a sample for two partials:
 *   R0, R1 a, b of p   R2, R3 a, b of q   R4, R5 table words p, q
 *   R6 index p, then the accumulator word   R7 index q, then the steps
 *   [SP] .. [SP + 12] steps a, b of p, a, b of q   [SP + 16] count
 */
.macro QUADRATURE sh
.Lquad\sh:
    R3 = [SP + 16];
    R3 >>= 1;
    CC = R3 == 0;
    IF CC JUMP .Lquad_single\sh;
    P1 = R3;
    LSETUP (.Lquad_pair\sh, .Lquad_pair_end\sh) LC1 = P1;
.Lquad_pair\sh:
    R6 = [P3];
    I0 = R6;
    R7 = [P3 + 24];
    I1 = R7;
    R6 >>= \sh;
    R7 >>= \sh;
    P1 = R6;
    P2 = R7;
    R6 = [P3 + 4];
    M0 = R6;
    R7 = [P3 + 28];
    M1 = R7;
    R6 = [P3 + 16];
    [SP] = R6;
    R7 = [P3 + 20];
    [SP + 4] = R7;
    R6 = [P3 + 40];
    [SP + 8] = R6;
    R7 = [P3 + 44];
    [SP + 12] = R7;
    R0 = [P3 + 8];
    R1 = [P3 + 12];
    R2 = [P3 + 32];
    R3 = [P3 + 36];
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 2);
    P2 = P0 + (P2 << 2);
    I0 += M0;
    I1 += M1;
    LSETUP (.Lquad_pair_sample\sh, .Lquad_pair_sample_end\sh) LC0 = P5;
.Lquad_pair_sample\sh:
    R6 = I0;
    R7 = I1;
    R6 = R6 >> \sh || R4 = [P1] || NOP;
    R7 = R7 >> \sh || R5 = [P2] || NOP;
    P1 = R6;
    P2 = R7;
    A0 = R4.L * R0.H (IS) || R7 = [SP] || R6 = [I2++];
    A0 += R4.H * R1.H (IS) || I0 += M0 || NOP;
    A0 += R5.L * R2.H (IS) || I1 += M1 || NOP;
    R4 = (A0 += R5.H * R3.H) (IS);
    R6 = R6 + R4 (NS);
    R0 = R0 + R7 (NS) || R7 = [SP + 4] || [I3++] = R6;
    R1 = R1 + R7 (NS) || R7 = [SP + 8] || NOP;
    R2 = R2 + R7 (NS) || R7 = [SP + 12] || NOP;
    R3 = R3 + R7 (NS);
    P1 = P0 + (P1 << 2);
.Lquad_pair_sample_end\sh:
    P2 = P0 + (P2 << 2);
    I0 -= M0;
    I1 -= M1;
    R6 = I0;
    R7 = I1;
    [P3] = R6;
    [P3 + 8] = R0;
    [P3 + 12] = R1;
    [P3 + 24] = R7;
    [P3 + 32] = R2;
    [P3 + 36] = R3;
.Lquad_pair_end\sh:
    P3 += 48;
.Lquad_single\sh:
    R3 = [SP + 16];
    CC = BITTST (R3, 0);
    IF CC JUMP 1f;
    JUMP.L .Lbank_return;
1:
    /* One partial, 9 cycles a sample: R0, R1 a, b; R2, R3 steps. */
    R6 = [P3];
    I0 = R6;
    R6 >>= \sh;
    P1 = R6;
    R6 = [P3 + 4];
    M0 = R6;
    R0 = [P3 + 8];
    R1 = [P3 + 12];
    R2 = [P3 + 16];
    R3 = [P3 + 20];
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 2);
    I0 += M0;
    LSETUP (.Lquad_one\sh, .Lquad_one_end\sh) LC0 = P5;
.Lquad_one\sh:
    R6 = I0;
    R6 = R6 >> \sh || R4 = [P1] || NOP;
    P1 = R6;
    A0 = R4.L * R0.H (IS) || I0 += M0 || R7 = [I2++];
    R4 = (A0 += R4.H * R1.H) (IS);
    R7 = R7 + R4 (NS);
    R0 = R0 + R2 (NS) || [I3++] = R7;
    R1 = R1 + R3 (NS);
.Lquad_one_end\sh:
    P1 = P0 + (P1 << 2);
    I0 -= M0;
    R6 = I0;
    [P3] = R6;
    [P3 + 8] = R0;
    [P3 + 12] = R1;
    JUMP.L .Lbank_return;
.endm

/* void opxy_bank_quadrature_held(i32* acc, u32 chunks, const u32* pairs, u32 shift,
 *                                Pair* partials, u32 count, u32 hold)
 * a and b hold for `hold` samples; frames = chunks * hold. chunks, hold,
 * count >= 1; shift in {20, 21, 22}.
 *
 * Pair loop, 12 cycles a sample for two partials, plus 12 a chunk:
 *   R1 accumulator word   R0 sum   R2, R3 (b:a) top halves of p, q
 *   R4, R5 table words p, q   R6 index p   R7 index q, shifted a sample ahead
 *   P5 hold   [SP + 16] count   [SP + 20] chunks
 * a and b step in place in the Pair, once a chunk.
 */
.macro HELD sh
.Lheld\sh:
    R3 = [SP + 16];
    R3 >>= 1;
    CC = R3 == 0;
    IF CC JUMP .Lheld_single\sh;
    [SP + 24] = R3;                 /* pairs left */
.Lheld_pair\sh:
    R6 = [P3];
    I0 = R6;
    R7 = [P3 + 24];
    I1 = R7;
    R6 >>= \sh;
    R7 >>= \sh;                     /* R7: index q of sample 0, as the loop expects */
    P1 = R6;
    R6 = [P3 + 4];
    M0 = R6;
    R6 = [P3 + 28];
    M1 = R6;
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 2);
    I0 += M0;
    I1 += M1;
    P2 = [SP + 20];
    LSETUP (.Lheld_chunk\sh, .Lheld_chunk_end\sh) LC1 = P2;
.Lheld_chunk\sh:
    R0 = [P3 + 8];
    R1 = [P3 + 12];
    R2 = PACK (R1.H, R0.H) || R4 = [P3 + 16] || NOP;
    R0 = R0 + R4 (NS) || R5 = [P3 + 20] || NOP;
    R1 = R1 + R5 (NS) || [P3 + 8] = R0 || NOP;
    [P3 + 12] = R1;
    R0 = [P3 + 32];
    R1 = [P3 + 36];
    R3 = PACK (R1.H, R0.H) || R4 = [P3 + 40] || NOP;
    R0 = R0 + R4 (NS) || R5 = [P3 + 44] || NOP;
    R1 = R1 + R5 (NS) || [P3 + 32] = R0 || NOP;
    [P3 + 36] = R1;
    LSETUP (.Lheld_sample\sh, .Lheld_sample_end\sh) LC0 = P5;
.Lheld_sample\sh:
    P2 = R7;
    R6 = I0;
    R6 = R6 >> \sh || R4 = [P1] || NOP;
    P1 = R6;
    P2 = P0 + (P2 << 2);
    A1 = R4.H * R2.H, A0 = R4.L * R2.L (IS) || R5 = [P2] || R1 = [I2++];
    A1 += R5.H * R3.H, A0 += R5.L * R3.L (IS) || I0 += M0 || NOP;
    R0 = (A0 += A1);
    P1 = P0 + (P1 << 2);
    R1 = R1 + R0 (NS);
    R7 = I1;
.Lheld_sample_end\sh:
    R7 = R7 >> \sh || I1 += M1 || [I3++] = R1;
.Lheld_chunk_end\sh:
    NOP;
    I0 -= M0;
    I1 -= M1;                       /* I0, I1 ran one sample ahead */
    R6 = I0;
    R7 = I1;
    [P3] = R6;
    [P3 + 24] = R7;
    P3 += 48;
    R3 = [SP + 24];
    R3 += -1;
    [SP + 24] = R3;
    CC = R3 == 0;
    IF CC JUMP 1f;
    JUMP.L .Lheld_pair\sh;
1:
.Lheld_single\sh:
    R3 = [SP + 16];
    CC = BITTST (R3, 0);
    IF CC JUMP 1f;
    JUMP.L .Lbank_return;
1:
    /* One partial, 8 cycles a sample. */
    R6 = [P3];
    I0 = R6;
    R6 >>= \sh;
    P1 = R6;
    R6 = [P3 + 4];
    M0 = R6;
    I2 = P4;
    I3 = P4;
    P1 = P0 + (P1 << 2);
    I0 += M0;
    P2 = [SP + 20];
    LSETUP (.Lheld_one_chunk\sh, .Lheld_one_chunk_end\sh) LC1 = P2;
.Lheld_one_chunk\sh:
    R0 = [P3 + 8];
    R1 = [P3 + 12];
    R2 = PACK (R1.H, R0.H) || R4 = [P3 + 16] || NOP;
    R0 = R0 + R4 (NS) || R5 = [P3 + 20] || NOP;
    R1 = R1 + R5 (NS) || [P3 + 8] = R0 || NOP;
    [P3 + 12] = R1;
    LSETUP (.Lheld_one\sh, .Lheld_one_end\sh) LC0 = P5;
.Lheld_one\sh:
    R6 = I0;
    R6 = R6 >> \sh || R4 = [P1] || NOP;
    P1 = R6;
    A1 = R4.H * R2.H, A0 = R4.L * R2.L (IS) || I0 += M0 || R1 = [I2++];
    R0 = (A0 += A1);
    R1 = R1 + R0 (NS);
    P1 = P0 + (P1 << 2);
.Lheld_one_end\sh:
    [I3++] = R1;
.Lheld_one_chunk_end\sh:
    NOP;
    I0 -= M0;
    R6 = I0;
    [P3] = R6;
    JUMP.L .Lbank_return;
.endm

/* Shared entry for the two quadrature routines: saves registers, loads the
 * arguments and dispatches on the shift. */
.global _opxy_bank_quadrature
.type _opxy_bank_quadrature, STT_FUNC
_opxy_bank_quadrature:
    [--SP] = (R7:4, P5:3);
    SP += -32;
    P4 = R0;                        /* acc */
    P5 = R1;                        /* frames */
    P0 = R2;                        /* pairs */
    P3 = [SP + 76];                 /* partials (32 + 28 + 16) */
    R3 = [SP + 80];                 /* count */
    [SP + 16] = R3;
    R2 = [SP + 72];                 /* shift */
    R3 = 20;
    CC = R2 == R3;
    IF !CC JUMP 1f;
    JUMP.L .Lquad20;
1:  R3 = 21;
    CC = R2 == R3;
    IF !CC JUMP 2f;
    JUMP.L .Lquad21;
2:  JUMP.L .Lquad22;
.size _opxy_bank_quadrature, .-_opxy_bank_quadrature

.global _opxy_bank_quadrature_held
.type _opxy_bank_quadrature_held, STT_FUNC
_opxy_bank_quadrature_held:
    [--SP] = (R7:4, P5:3);
    SP += -32;
    P4 = R0;                        /* acc */
    [SP + 20] = R1;                 /* chunks */
    P0 = R2;                        /* pairs */
    P3 = [SP + 76];                 /* partials */
    R3 = [SP + 80];                 /* count */
    [SP + 16] = R3;
    P5 = [SP + 84];                 /* hold */
    R2 = [SP + 72];                 /* shift */
    R3 = 20;
    CC = R2 == R3;
    IF !CC JUMP 1f;
    JUMP.L .Lheld20;
1:  R3 = 21;
    CC = R2 == R3;
    IF !CC JUMP 2f;
    JUMP.L .Lheld21;
2:  JUMP.L .Lheld22;
.size _opxy_bank_quadrature_held, .-_opxy_bank_quadrature_held

    QUADRATURE 20
    QUADRATURE 21
    QUADRATURE 22
    HELD 20
    HELD 21
    HELD 22

.Lbank_return:
    SP += 32;
    (R7:4, P5:3) = [SP++];
    RTS;

/* void opxy_bank_sine_glide(i32* acc, u32 frames, const i16* table, u32 shift,
 *                           Glide* partials, u32 count, u32 scale)
 * Glide: phase +0, increment +4, amplitude +8, step +12, glide +16 (20 bytes).
 * frames >= 1, count >= 1, shift in 16..31, scale in 0..15.
 *
 * One partial a pass, 9 cycles a sample (the increment needs a data register):
 *   R0 amplitude   R1.L -scale   R2 phase, a sample ahead   R3 increment, a sample ahead
 *   R4.L -shift   R5 index, then glide, then step   R6 table value, then the product
 *   R7 accumulator word   [SP] glide   [SP + 4] step
 */
.global _opxy_bank_sine_glide
.type _opxy_bank_sine_glide, STT_FUNC
_opxy_bank_sine_glide:
    [--SP] = (R7:4, P5:3);
    SP += -16;
    P4 = R0;                        /* acc */
    P5 = R1;                        /* frames */
    P0 = R2;                        /* table */
    R4 = [SP + 56];                 /* shift */
    R4 = -R4;
    P3 = [SP + 60];                 /* partials */
    R3 = [SP + 64];                 /* count */
    R1 = [SP + 68];                 /* scale */
    R1 = -R1;
    P1 = R3;
    LSETUP (.Lglide_partial, .Lglide_partial_end) LC1 = P1;
.Lglide_partial:
    R2 = [P3];                      /* phase 0 */
    R5 = LSHIFT R2 BY R4.L;
    P1 = R5;
    R3 = [P3 + 4];                  /* increment 0 */
    R5 = [P3 + 12];
    [SP + 4] = R5;                  /* step */
    R5 = [P3 + 16];
    [SP] = R5;                      /* glide */
    R0 = [P3 + 8];
    I2 = P4;
    I3 = P4;
    R2 = R2 + R3 (NS);              /* phase 1 */
    R3 = R3 + R5 (NS);              /* increment 1 */
    P1 = P0 + (P1 << 1);
    LSETUP (.Lglide_sample, .Lglide_sample_end) LC0 = P5;
.Lglide_sample:
    R5 = LSHIFT R2 BY R4.L || R6.L = W[P1] || R7 = [I2++];
    P1 = R5;
    R2 = R2 + R3 (NS) || R5 = [SP] || NOP;
    R3 = R3 + R5 (NS) || R5 = [SP + 4] || NOP;
    R6 = R6.L * R0.H (IS);
    R6 = ASHIFT R6 BY R1.L;
    R7 = R7 + R6 (NS);
    R0 = R0 + R5 (NS) || [I3++] = R7;
.Lglide_sample_end:
    P1 = P0 + (P1 << 1);
    R5 = [SP];
    R3 = R3 - R5 (NS);              /* the increment and phase ran a sample ahead */
    R2 = R2 - R3 (NS);
    [P3] = R2;
    [P3 + 4] = R3;
    [P3 + 8] = R0;
.Lglide_partial_end:
    P3 += 20;
    SP += 16;
    (R7:4, P5:3) = [SP++];
    RTS;
.size _opxy_bank_sine_glide, .-_opxy_bank_sine_glide
