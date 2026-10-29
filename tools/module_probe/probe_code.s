/* The code a probe file carries (sdk/platform/opxy-1.2.1/module_probe.cpp):
 * position-independent, returns (R0 ^ 0x12345678) + 3. */
.section .text
.align 2
.global probe_code
probe_code:
    R1.L = 0x5678;
    R1.H = 0x1234;
    R0 = R0 ^ R1;
    R0 += 3;
    RTS;
