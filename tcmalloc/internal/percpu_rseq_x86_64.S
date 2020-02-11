/*
 * Copyright 2019 The TCMalloc Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __x86_64__
#error "percpu_rseq_x86_64.S should only be included for x86-64 builds"
#endif  //  __x86_64__

#include "tcmalloc/internal/percpu.h"

/*
 * API Exposition:
 *
 *   METHOD_abort:  // Emitted as part of START_RSEQ()
 *     START_RSEQ() // Starts critical section between [start,commit)
 *   METHOD_start:  // Emitted as part of START_RSEQ()
 *     FETCH_CPU()  // Reads current CPU
 *     ...
 *     single store // Commits sequence
 *   METHOD_commit:
 *     ...return...
 *
 * This process is assisted by the DEFINE_UPSTREAM_CS macro, which encodes a
 * (rodata) constant table, whose address is used to start the critical
 * section, and the abort trampoline.
 *
 * The trampoline is used because:
 * 1.  Restarts are expected to be rare, so the extra jump when restarting is
 *     expected to be infrequent.
 * 2.  The upstream restartable sequence implementation expects the trailing 4
 *     bytes of the abort PC to be "signed" (to prevent manipulation of the PC
 *     to an arbitrary choice).  For us, this is PERCPU_RSEQ_SIGNATURE.  This
 *     value is passed to the kernel during configuration of the rseq syscall.
 *     This would either need to be encoded as a nop (SIGN_ABORT) at the start
 *     of every restartable sequence, increasing instruction cache pressure, or
 *     placed directly before the entry point.
 *
 * The trampoline returns us to METHOD_abort, which is the normal entry point
 * for the restartable sequence.  Upon restart, the (upstream) kernel API
 * clears the per-thread restartable sequence state. We return to METHOD_abort
 * (rather than METHOD_start), as we need to reinitialize this value.
 */

/* Place the code into the google_malloc section. This section is the heaviest
 * user of Rseq code, so it makes sense to co-locate it.
 */

.section google_malloc, "ax"

/* ---------------- start helper macros ----------------  */

// This macro defines a relocation associated with the provided label to keep
// section GC from discarding it independently of label.
#if !defined(__clang_major__) || __clang_major__ >= 9
#define PINSECTION(label) .reloc 0, R_X86_64_NONE, label
#else
#define PINSECTION(label)
#endif

// This macro defines:
// * the rseq_cs instance that we'll use for label's critical section.
// * a trampoline to return to when we abort.  This label_trampoline is
//   distinct from label_start, as the return IP must be "signed" (see
//   SIGN_ABORT()).
//
// TODO(b/141629158):  __rseq_cs only needs to be writeable to allow for
// relocations, but could be read-only for non-PIE builds.
#define DEFINE_UPSTREAM_CS(label)                   \
  .pushsection __rseq_cs, "aw";                     \
  .balign 32;                                       \
  .protected __rseq_cs_##label;                     \
  .type __rseq_cs_##label,@object;                  \
  .size __rseq_cs_##label,32;                       \
  __rseq_cs_##label:                                \
  .long PERCPU_RSEQ_VERSION, PERCPU_RSEQ_FLAGS;     \
  .quad .L##label##_start;                          \
  .quad .L##label##_commit - .L##label##_start;     \
  .quad label##_trampoline;                         \
  PINSECTION(.L##label##array);                     \
  .popsection;                                      \
  .pushsection __rseq_cs_ptr_array, "aw";           \
  .L##label##array:                                 \
  .quad __rseq_cs_##label;                          \
  .popsection;                                      \
  SIGN_ABORT();                                     \
  .globl label##_trampoline;                        \
  .type  label##_trampoline, @function;             \
label##_trampoline:                                 \
  .cfi_startproc;                                   \
  jmp .L##label##_abort;                            \
  .cfi_endproc;                                     \
  .size label##_trampoline, . - label##_trampoline;

// This is part of the upstream rseq ABI.  The 4 bytes prior to the abort IP
// must match PERCPU_RSEQ_SIGNATURE (as configured by our rseq syscall's
// signature parameter).  This signature is used to annotate valid abort IPs
// (since rseq_cs could live in a user-writable segment).
//
// To allow this to be safely executed as a valid instruction, we encode the
// value with a nop.  This is decoded as:
//
//   nopl 0xSIGNATURE(%rip)
//
#define SIGN_ABORT()           \
  .byte 0x0f, 0x1f, 0x05;      \
  .long PERCPU_RSEQ_SIGNATURE;

/*
 * Provide a directive to specify the size of symbol "label", relative to the
 * current location and its start.
 */
#define ENCODE_SIZE(label) .size label, . - label;

/*
 * Fetches the per-cpu data addressed by (%base) for cpu %cpu into %dest.
 *
 *  %base must be distinct from %dest since the actual address is calculated in
 *  place using %cpu
 */
#define FETCH_PERCPU_DATA(cpu, base, dest) \
  movslq cpu, dest; \
  shlq $PERCPU_BYTES_PER_REGION_SHIFT, dest; \
  addq base, dest;

/*
 *  Combines FETCH_CPU and FETCH_PERCPU_DATA above for the local cpu (which is
 *  also returned in %cpu).
 */
#define FETCH_LOCAL_PERCPU_DATA(cpu, base, dest) \
  FETCH_CPU(cpu); \
  FETCH_PERCPU_DATA(cpu, base, dest);

/* In all non-position independent cases we need to use RIP-relative label
   addresses */
#if !defined(__PIC__)
#define LABEL_ADDR(label) $label
#else
#define LABEL_ADDR(label) label@GOTPCREL(%rip)
#endif /* !defined(__PIC__) */

/* With PIE;  have initial-exec TLS, even in the presence of position
   independent code. */
#if !defined(__PIC__) || defined(__PIE__)
#define FETCH_CPU(dest) movl %fs:__rseq_abi@TPOFF+4, dest;
#define START_RSEQ(src)                         \
   .L##src##_abort:                             \
   leaq __rseq_cs_##src(%rip), %rax;            \
   movq %rax, %fs:__rseq_abi@TPOFF+8;           \
   .L##src##_start:

#else  /* !defined(__PIC__) || defined(__PIE__) */

/*
 * FETCH_CPU assumes &__rseq_abi is in %rax.  We cannot call
 * tcmalloc_tls_fetch_pic at this point, as we have started our restartable
 * sequence.  If we are prempted there, the kernel will clear rseq_cs as
 * tcmalloc_tls_fetch_pic does not appear in the restartable sequence's address
 * range.
 */
#define FETCH_CPU(dest) \
  movl 4(%rax), dest;  /* cpuid is 32-bits */
#define START_RSEQ(src)                \
  .L##src##_abort:                     \
  call tcmalloc_tls_fetch_pic@PLT;     \
  leaq __rseq_cs_##src(%rip), %r11;    \
  movq %r11, 8(%rax);                  \
   .L##src##_start:

/*
 * We can safely call this function from within an RSEQ section as it only
 * generates a thread-local address which will not change across a missed
 * restart.  This must precede the construction of any preparatory state.
 */
  .globl tcmalloc_tls_fetch_pic
  .type tcmalloc_tls_fetch_pic, @function
tcmalloc_tls_fetch_pic:
  .cfi_startproc
  push %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset 6, -16
  mov %rsp, %rbp
  .cfi_def_cfa_register 6
  sub $0x30, %rsp
  mov %rsi, -0x08(%rbp)  /* atypical abi: tcmalloc_tls_fetch_pic preserves regs */
  mov %rdi, -0x10(%rbp)
  mov %rdx, -0x18(%rbp)
  mov %rcx, -0x20(%rbp)
  mov %r8,  -0x28(%rbp)
  mov %r9,  -0x30(%rbp)
  /*
   * Below is an optimized relocatable TLS lookup per ELF spec:
   *   http://www.akkadia.org/drepper/tls.pdf
   * When possible, this is replaced at link-time with a call-free variant.
   */
  .byte 0x66;
  leaq __rseq_abi@TLSGD(%rip), %rdi;
  .word 0x6666;
  rex64;
  call __tls_get_addr@PLT;
  mov -0x08(%rbp), %rsi
  mov -0x10(%rbp), %rdi
  mov -0x18(%rbp), %rdx
  mov -0x20(%rbp), %rcx
  mov -0x28(%rbp), %r8
  mov -0x30(%rbp), %r9
  add $0x30, %rsp
  leave
  .cfi_def_cfa_register 7
  .cfi_def_cfa_offset 8
  ret; /* &__rseq_abi in %rax */
  .cfi_endproc
#endif  /* !defined(__PIC__) || defined(__PIE__) */

/* ---------------- end helper macros ---------------- */

/* start of atomic restartable sequences */

/*
 * NOTE:  We don't use cmpxchgq in the following functions since this would
   make checking the success of our commit operation dependent on flags (which
 * are in turn clobbered by the restart region) -- furthermore we can't just
 * retry to fill in the flags since the restarted cmpxchg may have actually
 * succeeded; spuriously failing subsequent attempts.
 */

/*
 * int TcmallocSlab_PerCpuCmpxchg64(int target_cpu, long *p,
 *                                  long old_val, long new_val)
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_PerCpuCmpxchg64
  .type  TcmallocSlab_PerCpuCmpxchg64, @function
TcmallocSlab_PerCpuCmpxchg64:
  .cfi_startproc
.LTcmallocSlab_PerCpuCmpxchg64_region0:
  START_RSEQ(TcmallocSlab_PerCpuCmpxchg64);
  FETCH_CPU(%eax);
  cmp %eax, %edi; /* check cpu vs current_cpu */
  jne .LTcmallocSlab_PerCpuCmpxchg64_region1;
  cmp %rdx, (%rsi); /* verify *p == old */
  jne .LTcmallocSlab_PerCpuCmpxchg64_region2;
  mov %rcx, (%rsi);
.LTcmallocSlab_PerCpuCmpxchg64_region1:
.LTcmallocSlab_PerCpuCmpxchg64_commit:
  ret;  /* return current cpu, indicating mismatch OR success */
.LTcmallocSlab_PerCpuCmpxchg64_region2:
  mov $-1, %eax;  /* mismatch versus "old" or "check", return -1 */
  ret;
.LTcmallocSlab_PerCpuCmpxchg64_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_PerCpuCmpxchg64)
DEFINE_UPSTREAM_CS(TcmallocSlab_PerCpuCmpxchg64)

/* typedef int (*OverflowHandler)(
 *     int cpu (%rdi),
 *     size_t cl (%rsi),
 *     void *item (%rdx));
 * int TcmallocSlab_Push(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     void* item (%rdx),
 *     size_t shift (%rcx),
 *     OverflowHandler f (%r8)) {
 *   uint64_t rax = __rseq_abi.cpu_id;
 *   Header* hdr = GetHeader(rdi, rax, cl);
 *   uint64_t r11 = hdr->current;
 *   uint64_t r9 = hdr->end;
 *   if (r11 < r9) {
 *     *(CpuMemoryStart(ptr, rax) + current * 8) = rdx;
 *     r11++;
 *     hdr->current = r11;
 *     return rax;
 *   } else {
 *     return f(rax, cl, item);
 *   }
 * }
 *
 * By default assembler will not align these symbols at all (not even on 8).
 * But their performance is significantly affected by starting position
 * (i.e. +/-20%). It's not that they simply need to be aligned on a sufficient
 * power of two. For example, offset 17 from a 64-byte aligned address worked
 * good as well. Aligning on 64 seems to work good with this particular
 * implementation, but more importantly it makes performance less affected by
 * addition/removal of unrelated code somewhere else.
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Push
  .type  TcmallocSlab_Push, @function
TcmallocSlab_Push:
  .cfi_startproc
.LTcmallocSlab_Push_region0:
  START_RSEQ(TcmallocSlab_Push);
  FETCH_CPU(%eax);
  mov %rax, %r10
  shl %cl, %r10; /* multiply cpu by cache size */
  lea (%rdi, %r10), %r10
  movzwq (%r10, %rsi, 8), %r11 /* current */
  movzwq 6(%r10, %rsi, 8), %r9 /* end */
  cmpq %r9, %r11;
  jae .LTcmallocSlab_Push_region2;
  movq %rdx, (%r10, %r11, 8);
  inc %r11;
  movw %r11w, (%r10, %rsi, 8);
.LTcmallocSlab_Push_region1:
.LTcmallocSlab_Push_commit:
  ret;
.LTcmallocSlab_Push_region2:
  mov %rax, %rdi; /* rdi = cpu */
#ifdef GOOGLE3_RETPOLINE
  mov %r8, %rcx;
  jmp TcmallocOverflowThunk
#else  // !GOOGLE3_RETPOLINE
  jmpq *%r8; /* tail call to f(cpu, cl, item) */
#endif  // !GOOGLE3_RETPOLINE
.LTcmallocSlab_Push_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Push)
DEFINE_UPSTREAM_CS(TcmallocSlab_Push)

/*
 * Variant of function that uses a fixed shift
 * int TcmallocSlab_Push_FixedShift(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     void* item (%rdx),
 *     OverflowHandler f (%rcx)) {  // Note that this used to be %r8
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Push_FixedShift
  .type  TcmallocSlab_Push_FixedShift, @function
TcmallocSlab_Push_FixedShift:
  .cfi_startproc
.LTcmallocSlab_Push_FixedShift_region0:
  START_RSEQ(TcmallocSlab_Push_FixedShift);
  FETCH_CPU(%eax);
  mov %rax, %r10
  shl $PERCPU_TCMALLOC_FIXED_SLAB_SHIFT, %r10; /* multiply cpu by cache size */
  add %rdi, %r10
  movzwq (%r10, %rsi, 8), %r11 /* current */
  cmpw 6(%r10, %rsi, 8), %r11w /* end */
  jae .LTcmallocSlab_Push_FixedShift_region2;
  movq %rdx, (%r10, %r11, 8);
  inc %r11;
  movw %r11w, (%r10, %rsi, 8);
.LTcmallocSlab_Push_FixedShift_region1:
.LTcmallocSlab_Push_FixedShift_commit:
  ret;
.LTcmallocSlab_Push_FixedShift_region2:
  mov %rax, %rdi; /* rdi = cpu */
#ifdef GOOGLE3_RETPOLINE
  jmp TcmallocOverflowThunk
#else  // !GOOGLE3_RETPOLINE
  jmpq *%rcx; /* tail call to f(cpu, cl, item) */
#endif  // !GOOGLE3_RETPOLINE
.LTcmallocSlab_Push_FixedShift_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Push_FixedShift)
DEFINE_UPSTREAM_CS(TcmallocSlab_Push_FixedShift)

/* typedef void* (*UnderflowHandler*)(
 *     int cpu (%rdi),
 *     size_t cl (%rsi));
 * void* TcmallocSlab_Pop(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     UnderflowHandler f (%rdx),
 *     size_t shift (%rcx))) {
 *   uint64_t rax = __rseq_abi.cpu_id;
 *   Header* hdr = GetHeader(rdi, rax, cl);
 *   uint64_t r10 = hdr->current;
 *   uint64_t r9 = hdr->begin;
 *   if (r10 > r9) {
 *     uint64_t rax = *(CpuMemoryStart(ptr, rax) + current * 8 - 16);
 *     prefetcht0(rax);
 *     rax = *(CpuMemoryStart(ptr, rax) + current * 8 - 8);
 *     hdr->current--;
 *     return rax;
 *   } else {
 *     return f(rax, cl);
 *   }
 * }
 *
 * A note about prefetcht0 in Pop (and Pop_FixedShift):  While this prefetch
 * may appear costly, trace analysis shows the target is frequently used
 * (b/70294962).  Stalling on a TLB miss at the prefetch site (which has no
 * deps) and prefetching the line async is better than stalling at the use
 * (which may have deps) to fill the TLB and the cache miss.
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Pop
  .type  TcmallocSlab_Pop, @function
TcmallocSlab_Pop:
  .cfi_startproc
.LTcmallocSlab_Pop_region0:
  START_RSEQ(TcmallocSlab_Pop);
  FETCH_CPU(%eax);
  shl %cl, %rax; /* multiply cpu by cache size*/
  lea (%rdi, %rax), %r11;
  movzwq (%r11, %rsi, 8), %r10; /* current */
  movzwq 4(%r11, %rsi, 8), %r9; /* begin */
  cmp %r9, %r10;
  jbe .LTcmallocSlab_Pop_region2;
  mov -16(%r11, %r10, 8), %rax;
  prefetcht0 (%rax);
  movq -8(%r11, %r10, 8), %rax;
  decl (%r11, %rsi, 8);
.LTcmallocSlab_Pop_region1:
.LTcmallocSlab_Pop_commit:
  ret;
.LTcmallocSlab_Pop_region2:
  mov %rax, %rdi; /* rdi = cpu */
  shr %cl, %rdi; /* undo shift in the beginning of the function */
#ifdef GOOGLE3_RETPOLINE
  jmp TcmallocUnderflowThunk;
#else  // !GOOGLE3_RETPOLINE
  jmp *%rdx; /* tail call to f(cpu, cl) */
#endif  // !GOOGLE3_RETPOLINE
.LTcmallocSlab_Pop_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Pop)
DEFINE_UPSTREAM_CS(TcmallocSlab_Pop)

/*
 * Variant of function that uses a fixed shift
 * void* TcmallocSlab_Pop_FixedShift(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     UnderflowHandler f (%rdx) {
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Pop_FixedShift
  .type  TcmallocSlab_Pop_FixedShift, @function
TcmallocSlab_Pop_FixedShift:
  .cfi_startproc
.LTcmallocSlab_Pop_FixedShift_region0:
  START_RSEQ(TcmallocSlab_Pop_FixedShift);
  FETCH_CPU(%eax);
  shl $PERCPU_TCMALLOC_FIXED_SLAB_SHIFT, %rax; /* multiply cpu by cache size*/
  lea (%rdi, %rax), %r11;
  movzwq (%r11, %rsi, 8), %r10; /* current */
  cmpw 4(%r11, %rsi, 8), %r10w /* begin */
  jbe .LTcmallocSlab_Pop_FixedShift_region2;
  mov -16(%r11, %r10, 8), %rax;
  prefetcht0 (%rax);
  movq -8(%r11, %r10, 8), %rax;
  decl (%r11, %rsi, 8);
.LTcmallocSlab_Pop_FixedShift_region1:
.LTcmallocSlab_Pop_FixedShift_commit:
  ret;
.LTcmallocSlab_Pop_FixedShift_region2:
  mov %rax, %rdi; /* rdi = cpu */
  shr $PERCPU_TCMALLOC_FIXED_SLAB_SHIFT, %rdi; /* undo shift in the beginning of the function */
#ifdef GOOGLE3_RETPOLINE
  jmp TcmallocUnderflowThunk;
#else  // !GOOGLE3_RETPOLINE
  jmp *%rdx; /* tail call to f(cpu, cl) */
#endif
.LTcmallocSlab_Pop_FixedShift_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Pop_FixedShift)
DEFINE_UPSTREAM_CS(TcmallocSlab_Pop_FixedShift)

/* size_t TcmallocSlab_PushBatch_FixedShift(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     void** batch (%rdx),
 *     size_t len (%rcx) {
 *   uint64_t r8 = __rseq_abi.cpu_id;
 *   uint64_t* r8 = CpuMemoryStart(rdi, r8);
 *   Header* hdr = r8 + rsi * 8;
 *   uint64_t r9 = hdr->current;
 *   uint64_t r10 = hdr->end;
 *   if (r9 >= r10) return 0;
 *   r11 = rcx;
 *   r10 = r9 + min(rcx, r10 - r9);
 * loop:
 *   r11--;
 *   rax = batch[r11];
 *   *(r8 + r9 * 8) = rax;
 *   r9++;
 *   if (r9 != r10) goto loop;
 *   hdr->current = r9;
 *   return rcx - r11;
 * }
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_PushBatch_FixedShift
  .type  TcmallocSlab_PushBatch_FixedShift, @function
TcmallocSlab_PushBatch_FixedShift:
  .cfi_startproc
.LTcmallocSlab_PushBatch_FixedShift_region0:
  START_RSEQ(TcmallocSlab_PushBatch_FixedShift);
  FETCH_CPU(%r8d);
  shl $PERCPU_TCMALLOC_FIXED_SLAB_SHIFT, %r8; /* multiply cpu by 256k */
  lea (%rdi, %r8), %r8;
  movzwq (%r8, %rsi, 8), %r9; /* current */
  movzwq 6(%r8, %rsi, 8), %r10; /* end */
  cmpq %r10, %r9;
  jae .LTcmallocSlab_PushBatch_FixedShift_region2;
  movq %rcx, %r11; /* r11 = copy of len */
  subq %r9, %r10; /* r10 = free capacity */
  cmpq %rcx, %r10;
  cmovaq %rcx, %r10; /* r10 = min(len, free capacity) */
  addq %r9, %r10;
.LTcmallocSlab_PushBatch_FixedShift_loop:
  decq %r11;
  movq (%rdx, %r11, 8), %rax;
  movq %rax, (%r8, %r9, 8);
  incq %r9;
  cmpq %r9, %r10;
  jne .LTcmallocSlab_PushBatch_FixedShift_loop
  movq %rcx, %rax;
  subq %r11, %rax;
  movw %r9w, (%r8, %rsi, 8);
.LTcmallocSlab_PushBatch_FixedShift_region1:
.LTcmallocSlab_PushBatch_FixedShift_commit:
  ret;
.LTcmallocSlab_PushBatch_FixedShift_region2:
  xor %rax, %rax;
  ret;
.LTcmallocSlab_PushBatch_FixedShift_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_PushBatch_FixedShift)
DEFINE_UPSTREAM_CS(TcmallocSlab_PushBatch_FixedShift)

/* size_t TcmallocSlab_PopBatch_FixedShift(
 *     void *ptr (%rdi),
 *     size_t cl (%rsi),
 *     void** batch (%rdx),
 *     size_t len (%rcx) {
 *   uint64_t r8 = __rseq_abi.cpu_id;
 *   uint64_t* r8 = CpuMemoryStart(rdi, r8);
 *   Header* hdr = GetHeader(rdi, rax, cl);
 *   uint64_t r9 = hdr->current;
 *   uint64_t r10 = hdr->begin;
 *   if (r9 <= r10) return 0;
 *   r11 = min(rcx, r9 - r10);
 *   rax = 0;
 * loop:
 *   r9--;
 *   r10 = *(r8 + r9 * 8);
 *   batch[rax] = r10;
 *   rax++;
 *   if (rax != r11) goto loop;
 *   hdr->current = r9;
 *   return rax;
 * }
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_PopBatch_FixedShift
  .type  TcmallocSlab_PopBatch_FixedShift, @function
TcmallocSlab_PopBatch_FixedShift:
  .cfi_startproc
.LTcmallocSlab_PopBatch_FixedShift_region0:
  START_RSEQ(TcmallocSlab_PopBatch_FixedShift);
  FETCH_CPU(%r8d);
  shl $PERCPU_TCMALLOC_FIXED_SLAB_SHIFT, %r8; /* multiply cpu by 256k */
  lea (%rdi, %r8), %r8;
  movzwq (%r8, %rsi, 8), %r9; /* current */
  movzwq 4(%r8, %rsi, 8), %r10; /* begin */
  cmp %r10, %r9;
  jbe .LTcmallocSlab_PopBatch_FixedShift_region2;
  movq %r9, %r11;
  subq %r10, %r11; /* r11 = available items */
  cmpq %rcx, %r11;
  cmovaq %rcx, %r11; /* r11 = min(len, available items) */
  xorq %rax, %rax;
.LTcmallocSlab_PopBatch_FixedShift_loop:
  decq %r9;
  movq (%r8, %r9, 8), %r10;
  movq %r10, (%rdx, %rax, 8);
  incq %rax;
  cmpq %rax, %r11;
  jne .LTcmallocSlab_PopBatch_FixedShift_loop
  movw %r9w, (%r8, %rsi, 8);
.LTcmallocSlab_PopBatch_FixedShift_region1:
.LTcmallocSlab_PopBatch_FixedShift_commit:
  ret;
.LTcmallocSlab_PopBatch_FixedShift_region2:
  xor %rax, %rax;
  ret;
.LTcmallocSlab_PopBatch_FixedShift_region3:
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_PopBatch_FixedShift)
DEFINE_UPSTREAM_CS(TcmallocSlab_PopBatch_FixedShift)

.section .note.GNU-stack,"",@progbits
