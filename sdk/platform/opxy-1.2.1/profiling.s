/* Optional OS 1.2.0 per-voice diagnostic bypasses. The profile enables these
 * only with profiling off. Absolute CALL hooks enter the existing LINK frame;
 * each shim jumps to its stock continuation, without touching SP or RETS.
 * Pointer loads/jumps preserve ASTAT. Replayed loads below belong to audio
 * processing, interleaved with the skipped timestamp/statistics instructions.
 * Keep these shims linked in both modes so one browser kit supports either.
 */
.section .text
.align 2

.global sdk_profile_voice_start
.type sdk_profile_voice_start, STT_FUNC
sdk_profile_voice_start:
    P0.H = 0x11a0;
    P0.L = 0x969c;
    JUMP (P0);
.size sdk_profile_voice_start, .-sdk_profile_voice_start

.global sdk_profile_osc_start
.type sdk_profile_osc_start, STT_FUNC
sdk_profile_osc_start:
    P0.H = 0x11a0;
    P0.L = 0x99ae;
    JUMP (P0);
.size sdk_profile_osc_start, .-sdk_profile_osc_start

.global sdk_profile_osc_end
.type sdk_profile_osc_end, STT_FUNC
sdk_profile_osc_end:
    P1 = [FP+8];
    P1 = [P1+0x94];
    P0.H = 0x11a0;
    P0.L = 0x9aec;
    JUMP (P0);
.size sdk_profile_osc_end, .-sdk_profile_osc_end

.global sdk_profile_filter_start
.type sdk_profile_filter_start, STT_FUNC
sdk_profile_filter_start:
    P0.H = 0x11a0;
    P0.L = 0x9b2e;
    JUMP (P0);
.size sdk_profile_filter_start, .-sdk_profile_filter_start

.global sdk_profile_filter_end
.type sdk_profile_filter_end, STT_FUNC
sdk_profile_filter_end:
    P2 = [FP+12];
    P1 = [FP+8];
    P0.H = 0x11a0;
    P0.L = 0x9cd4;
    JUMP (P0);
.size sdk_profile_filter_end, .-sdk_profile_filter_end

.global sdk_profile_voice_end
.type sdk_profile_voice_end, STT_FUNC
sdk_profile_voice_end:
    /* R7 supplies the original voice-lifetime return; P0 is live at resume. */
    R7 = [SP+0x38];
    P0 = [FP+8];
    P1.H = 0x11a0;
    P1.L = 0x9f34;
    JUMP (P1);
.size sdk_profile_voice_end, .-sdk_profile_voice_end

.global sdk_profile_alt_voice_start
.type sdk_profile_alt_voice_start, STT_FUNC
sdk_profile_alt_voice_start:
    P0.H = 0x8d51;
    P0.L = 0xa6b2;
    JUMP (P0);
.size sdk_profile_alt_voice_start, .-sdk_profile_alt_voice_start

.global sdk_profile_alt_osc_start
.type sdk_profile_alt_osc_start, STT_FUNC
sdk_profile_alt_osc_start:
    P0.H = 0x8d51;
    P0.L = 0xa94e;
    JUMP (P0);
.size sdk_profile_alt_osc_start, .-sdk_profile_alt_osc_start

.global sdk_profile_alt_osc_end
.type sdk_profile_alt_osc_end, STT_FUNC
sdk_profile_alt_osc_end:
    P1 = [P4+0x94];
    P0.H = 0x8d51;
    P0.L = 0xaa8e;
    JUMP (P0);
.size sdk_profile_alt_osc_end, .-sdk_profile_alt_osc_end

.global sdk_profile_alt_voice_end
.type sdk_profile_alt_voice_end, STT_FUNC
sdk_profile_alt_voice_end:
    P0.H = 0x8d51;
    P0.L = 0xac8e;
    JUMP (P0);
.size sdk_profile_alt_voice_end, .-sdk_profile_alt_voice_end
