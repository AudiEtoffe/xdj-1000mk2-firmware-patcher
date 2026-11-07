/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
    .text
    .align 2
    .global call_pf
call_pf:            ! r4 = fmt pointer value, r5 = hook address; mimics the firmware's calling convention (format string pushed on the stack)
    sts.l pr,@-r15
    mov.l r4,@-r15
    jsr @r5
    nop
    add #4,r15
    lds.l @r15+,pr
    rts
    nop
