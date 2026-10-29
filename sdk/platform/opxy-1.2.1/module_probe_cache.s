/* Module probe (module_probe.cpp): make code the module wrote with data
 * stores visible to instruction fetch. R0 = start, R1 = bytes. Writes back
 * each 32-byte data cache line, then invalidates the instruction cache lines. */
.section .text
.align 2
.global probe_sync_code
.type probe_sync_code, STT_FUNC
probe_sync_code:
    CC = R1 == 0;
    IF CC JUMP .Ldone;
    R1 += 31;
    R1 >>= 5;
    P0 = R0;
    R2 = R1;
.Lflush:
    FLUSH [P0++];
    R2 += -1;
    CC = R2 == 0;
    IF !CC JUMP .Lflush;
    SSYNC;
    P0 = R0;
.Linvalidate:
    IFLUSH [P0++];
    R1 += -1;
    CC = R1 == 0;
    IF !CC JUMP .Linvalidate;
    SSYNC;
    CSYNC;
.Ldone:
    RTS;
.size probe_sync_code, .-probe_sync_code
