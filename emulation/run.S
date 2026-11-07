/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
    .text
    .align 2
    .global finish
    .global run_flac
    .global run_wav
! int run_flac(entry r4, struct r5, r5val r6)
run_flac:
    sts.l pr,@-r15
    mov.l r14,@-r15
    mov.l r13,@-r15
    mov.l r12,@-r15
    mov.l r11,@-r15
    mov.l r10,@-r15
    mov.l r9,@-r15
    mov.l r8,@-r15
    add #-128,r15
    mov r5,r14
    mov r5,r1
    add #84,r1
    mov r6,r5
    jsr @r4
    nop
    nop
! int run_wav(entry r4, struct r5, r10val r6, mode r7)
run_wav:
    sts.l pr,@-r15
    mov.l r14,@-r15
    mov.l r13,@-r15
    mov.l r12,@-r15
    mov.l r11,@-r15
    mov.l r10,@-r15
    mov.l r9,@-r15
    mov.l r8,@-r15
    add #-128,r15
    mov.l r7,@(52,r15)
    mov r5,r1
    mov r5,r14
    mov r5,r12
    mov r6,r10
    jsr @r4
    nop
    nop

    .align 2
finish:
    add #127,r15
    add #1,r15
    mov.l @r15+,r8
    mov.l @r15+,r9
    mov.l @r15+,r10
    mov.l @r15+,r11
    mov.l @r15+,r12
    mov.l @r15+,r13
    mov.l @r15+,r14
    lds.l @r15+,pr
    rts
    nop
