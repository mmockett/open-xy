/* The auxiliary controller (vtable 0803fb6c) for aux modules (aux_modules.cpp).
 * Stock calls its methods through the vtable, too far from the module for a
 * relative branch, so each hook replaces an eight-byte instruction group early
 * in the method with an absolute call. A stub either returns from the stock
 * method itself, through the method's own epilogue, or replays the group and
 * returns into it.
 */
.include "sdk/platform/opxy-1.2.1/hook.inc"
.section .text
.align 2
.extern sdk_aux_screen
.extern sdk_aux_turn
.extern sdk_aux_select

/* 8c08f540: method 11, the page for the selected aux track, after its LINK
 * 0xc, in place of P1 = 0x840a0534 (the selected track). A nonzero page from
 * C++ is the method's result. */
.global sdk_aux_screen_hook
.type sdk_aux_screen_hook, STT_FUNC
sdk_aux_screen_hook:
    HOOK_SAVE
    CALL sdk_aux_screen;
    CC = R0 == 0;
    IF CC JUMP 1f;
    HOOK_RESULT R0
    HOOK_RESTORE
    UNLINK;
    RTS;
1:
    HOOK_RESTORE
    P1.L = 0x0534;
    P1.H = 0x840a;
    RTS;
.size sdk_aux_screen_hook, .-sdk_aux_screen_hook

/* 8c08fa14: method 6, an encoder turn (R1 the encoder, R2 the event), after
 * LINK 0x30, [--SP] = (R7:4, P5:3) and SP += -16, in place of P5 = 0x840a0534.
 * When C++ edited a module's control, the method returns 1 through its
 * epilogue; otherwise stock's turn continues with every register intact. */
.global sdk_aux_turn_hook
.type sdk_aux_turn_hook, STT_FUNC
sdk_aux_turn_hook:
    HOOK_SAVE
    R0 = [SP + 16];     /* the saved R1 and R2 */
    R1 = [SP + 20];
    CALL sdk_aux_turn;
    CC = R0 == 0;
    IF CC JUMP 1f;
    HOOK_RESULT R0
    HOOK_RESTORE
    SP += 16;
    (R7:4, P5:3) = [SP++];
    UNLINK;
    RTS;
1:
    HOOK_RESTORE
    P5.L = 0x0534;
    P5.H = 0x840a;
    RTS;
.size sdk_aux_turn_hook, .-sdk_aux_turn_hook

/* 8c08f624: the selector (8c08f60c(controller, track)) leaving for any track
 * but a send, in place of its epilogue SP += 0x14; (R7:4) = [SP++]; UNLINK.
 * R4 holds the track. The stub completes the epilogue and returns to the
 * selector's caller. */
.global sdk_aux_select_hook
.type sdk_aux_select_hook, STT_FUNC
sdk_aux_select_hook:
    HOOK_SAVE
    R0 = R4;
    CALL sdk_aux_select;
    HOOK_RESTORE
    SP += 0x14;
    (R7:4) = [SP++];
    UNLINK;
    RTS;
.size sdk_aux_select_hook, .-sdk_aux_select_hook
