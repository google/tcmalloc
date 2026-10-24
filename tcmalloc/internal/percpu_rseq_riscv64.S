/*
 * Copyright 2026 The TCMalloc Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
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

#ifndef __riscv
#error "percpu_rseq_riscv64.S should only be included for RISC-V 64-bit builds"
#endif  //  __riscv

#include "tcmalloc/internal/percpu.h"

/*
 * API Exposition:
 *
 *   METHOD_abort:  // Emitted as part of START_RSEQ()
 *     START_RSEQ() // Starts critical section between [start,commit)
 *   METHOD_start:  // Emitted as part of START_RSEQ()
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
 *     to an arbitrary choice).  For us, this is TCMALLOC_PERCPU_RSEQ_SIGNATURE.
 *     This value is passed to the kernel during configuration of the rseq
 *     syscall.
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
#define PINSECTION(label) .reloc 0, R_RISCV_NONE, label
#else
#define PINSECTION(label)
#endif

// This macro defines:
// * the rseq_cs instance that we'll use for label's critical section.
// * a trampoline to return to when we abort.  This label_trampoline is
//   distinct from label_start, as the return IP must be "signed" (see
//   SIGN_ABORT()).
//
// __rseq_cs only needs to be writeable to allow for relocations.
#define DEFINE_UPSTREAM_CS(label)                                 \
  .pushsection __rseq_cs, "aw";                                   \
  .balign 32;                                                     \
  .protected __rseq_cs_##label;                                   \
  .type __rseq_cs_##label,@object;                                \
  .size __rseq_cs_##label,32;                                     \
  __rseq_cs_##label:                                              \
  .long TCMALLOC_PERCPU_RSEQ_VERSION, TCMALLOC_PERCPU_RSEQ_FLAGS; \
  .quad .L##label##_start;                                        \
  .quad .L##label##_commit - .L##label##_start;                   \
  .quad label##_trampoline;                                       \
  PINSECTION(.L##label##array);                                   \
  .popsection;                                                    \
  .pushsection __rseq_cs_ptr_array, "aw";                         \
  .L##label##array:                                               \
  .quad __rseq_cs_##label;                                        \
  .popsection;                                                    \
  .pushsection rseq_trampoline, "ax";                             \
  SIGN_ABORT();                                                   \
  .globl label##_trampoline;                                      \
  .type  label##_trampoline, @function;                           \
label##_trampoline:                                               \
  CFI(.cfi_startproc);                                            \
  j .L##label##_abort;                                            \
  CFI(.cfi_endproc);                                              \
  .size label##_trampoline, . - label##_trampoline;               \
  .popsection;

// This is part of the upstream rseq ABI.  The 4 bytes prior to the abort IP
// must match TCMALLOC_PERCPU_RSEQ_SIGNATURE (as configured by our rseq
// syscall's signature parameter).  This signature is used to annotate valid
// abort IPs (since rseq_cs could live in a user-writable segment).
#define SIGN_ABORT()           \
  .balign 4;                   \
  .insn 4, TCMALLOC_PERCPU_RSEQ_SIGNATURE

/*
 * Provide a directive to specify the size of symbol "label", relative to the
 * current location and its start.
 */
#define ENCODE_SIZE(label) .size label, . - label

// START_RSEQ puts &__rseq_abi in t4.
// We can't rely on unaligned loads on RISC-V, so we do two aligned loads.
#define FETCH_SLABS(dest)                        \
  lw  t1, (TCMALLOC_RSEQ_SLABS_OFFSET + 4)(t4);  \
  lwu dest, TCMALLOC_RSEQ_SLABS_OFFSET(t4);      \
  slli t1, t1, 32;                               \
  or  dest, dest, t1

/* With PIE  have initial-exec TLS, even in the presence of position
   independent code. */
#if !defined(__PIC__) || defined(__PIE__)
#define START_RSEQ(src)                       \
  .L##src##_abort:                            \
  auipc   t4, %tls_ie_pcrel_hi(__rseq_abi);   \
  ld      t4, %pcrel_lo(.L##src##_abort)(t4); \
  add     t4, t4, tp;                         \
  lla     t1, __rseq_cs_##src;                \
  sd      t1, 8(t4);                          \
  .L##src##_start:

#else  /* !defined(__PIC__) || defined(__PIE__) */

#define START_RSEQ(src)                            \
  .L##src##_abort:                                 \
  addi    sp, sp, -48;                             \
  sd      a0, 0(sp);                               \
  sd      a1, 8(sp);                               \
  sd      a2, 16(sp);                              \
  sd      a3, 24(sp);                              \
  sd      ra, 32(sp);                              \
  .L##src##_pcrel_hi:                              \
  auipc   a0, %tls_gd_pcrel_hi(__rseq_abi);        \
  addi    a0, a0, %pcrel_lo(.L##src##_pcrel_hi);   \
  call    __tls_get_addr;                          \
  mv      t4, a0;                                  \
  ld      a0, 0(sp);                               \
  ld      a1, 8(sp);                               \
  ld      a2, 16(sp);                              \
  ld      a3, 24(sp);                              \
  ld      ra, 32(sp);                              \
  addi    sp, sp, 48;                              \
  lla     t1, __rseq_cs_##src;                     \
  sd      t1, 8(t4);                               \
  .L##src##_start:

#endif

/* ---------------- end helper macros ---------------- */

/* start of atomic restartable sequences */

/*
 * ABI note:
 *
 * See the ABI note in percpu_rseq_x86_64.S. For RISC-V, the function arguments
 * registers are a0-a7 and other usable scratch registers are t1-t6.
 */

/* size_t TcmallocSlab_Internal_PushBatch(
 *     size_t size_class (a0),
 *     void** batch (a1),
 *     size_t len (a2)) {
 *   uint64_t* t4 = tcmalloc_rseq.slabs;
 *   if ((t4 & TCMALLOC_CACHED_SLABS_MASK) == 0) return 0;
 *   t4 &= ~TCMALLOC_CACHED_SLABS_MASK;
 *   Header* hdr = t4 + size_class;
 *   Header t2 = *hdr;
 *   uint64_t a5 = current;
 *   uint64_t a6 = remaining_capacity;
 *   if (a6 == 0) return 0;
 *   a6 = min(len, a6);
 *   a5 = t4 + current * 8;
 *   t5 = t4 + (current + a6) * 8;
 *   t3 = batch + len * 8;
 * loop:
 *   t3--;
 *   a4 = *t3;		(pre-index) Pop from Batch
 *   *a5 = a4;		(post-index) Push to Slab
 *   a5++;
 *   if (a5 != t5) goto loop;
 *   *hdr = [current + a6, remaining_capacity - a6]
 *   return a6;
 * }
 */
  .p2align 6 /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PushBatch
  .type  TcmallocSlab_Internal_PushBatch, @function
TcmallocSlab_Internal_PushBatch:
  CFI(.cfi_startproc)
  START_RSEQ(TcmallocSlab_Internal_PushBatch)
  FETCH_SLABS(t4)
  bgez	t4, .LTcmallocSlab_Internal_PushBatch_full
  slli	t4, t4, 1
  srli	t4, t4, 1
  slli	t6, a0, 2   /* size_class * 4 */
  add	a4, t4, t6  /* a4 = hdr */
  lwu	t2, 0(a4)   /* t2 = [current, remaining_capacity] */
  slli	t1, t2, 48
  srli	a5, t1, 48  /* a5 = current */
  srli	a6, t2, 16  /* a6 = remaining_capacity */
  beqz	a6, .LTcmallocSlab_Internal_PushBatch_full
  bge	a2, a6, .LTcmallocSlab_Internal_PushBatch_min /* a6 = min(len, a6) */
  mv	a6, a2
.LTcmallocSlab_Internal_PushBatch_min:
  add	a7, a5, a6   /* a7 = new_current */
  slli	a5, a5, 3    /* set pointers */
  slli	t5, a7, 3
  slli	t3, a2, 3
  add   a5, t4, a5   /* a5 = t4 + current * 8 */
  add   t5, t4, t5   /* t5 = t4 + (current + a6) * 8 */
  add   t3, a1, t3   /* t3 = batch + len * 8 */
.LTcmallocSlab_Internal_PushBatch_loop:
  addi	t3, t3, -8   /* t3-- */
  ld	a4, 0(t3)    /* a4 = *(t3) */
  sd	a4, 0(a5)    /* *(a5) = a4 */
  addi	a5, a5, 8    /* a5++ */
  bne	a5, t5, .LTcmallocSlab_Internal_PushBatch_loop
  add	a4, t4, t6   /* a4 = hdr */
  slli	t1, a6, 16   /* t1 = a6 << 16 */
  add	t2, t2, a6   /* current += a6 */
  sub	t2, t2, t1   /* remaining_capacity -= a6 */
  sw	t2, 0(a4)    /* *hdr = new [current, remaining_capacity] */
.LTcmallocSlab_Internal_PushBatch_commit:
  mv	a0, a6       /* number of items pushed */
  ret
.LTcmallocSlab_Internal_PushBatch_full:
  li	a0, 0
  ret
  CFI(.cfi_endproc)
ENCODE_SIZE(TcmallocSlab_Internal_PushBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PushBatch)

/* size_t TcmallocSlab_Internal_PopBatch(
 *     size_t size_class (a0),
 *     void** batch (a1),
 *     size_t len (a2),
 *     std::atomic<uint16_t>* begin_ptr (a3)) {
 *   uint64_t* t4 = tcmalloc_rseq.slabs;
 *   if ((t4 & TCMALLOC_CACHED_SLABS_MASK) == 0) return 0;
 *   t4 &= ~TCMALLOC_CACHED_SLABS_MASK;
 *   Header* hdr = t4 + size_class;
 *   t2 = *hdr;
 *   uint64_t a5 = current;
 *   uint64_t a6 = *begin_ptr;
 *   if (a5 <= a6) return 0;
 *   a6 = min(len, a5 - a6);
 *   a5 = t4 + current * 8;
 *   t3 = a6;
 *   t5 = batch;
 * loop:
 *   a5--;
 *   a7 = *(a5);	(pre-index) Pop from Slab
 *   *(t5) = a7;	(post-index) Push to Batch
 *   t5++;
 *   t3--;
 *   if (t3 != 0) goto loop;
 *   *hdr = [current - a6, remaining_capacity + a6];
 *   return a6;
 * }
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PopBatch
  .type  TcmallocSlab_Internal_PopBatch, @function
TcmallocSlab_Internal_PopBatch:
  CFI(.cfi_startproc)
  START_RSEQ(TcmallocSlab_Internal_PopBatch);
  FETCH_SLABS(t4)
  bgez	t4, .LTcmallocSlab_Internal_PopBatch_empty
  slli	t4, t4, 1
  srli	t4, t4, 1
  slli	t6, a0, 2   /* size_class * 4 */
  add	a4, t4, t6
  lwu	t2, 0(a4)   /* t2 = [current, remaining_capacity] */
  slli	t1, t2, 48
  srli	a5, t1, 48  /* a5 = current */
  lhu	a6, 0(a3)   /* a6 = *begin_ptr */
  ble	a5, a6, .LTcmallocSlab_Internal_PopBatch_empty
  sub   a6, a5, a6  /* a6 = a5 - a6 */
  bge	a2, a6, .LTcmallocSlab_Internal_PopBatch_min /* a6 = min(len, a6) */
  mv	a6, a2
.LTcmallocSlab_Internal_PopBatch_min:
  slli	a5, a5, 3
  add   a5, t4, a5  /* a5 = t4 + current * 8 */
  mv	t3, a6
  mv	t5, a1
.LTcmallocSlab_Internal_PopBatch_loop:
  addi	a5, a5, -8  /* a5-- */
  ld	a7, 0(a5)   /* a7 = *(a5) */
  sd	a7, 0(t5)   /* *(t5) = a7 */
  addi	t5, t5, 8   /* t5++ */
  add	t3, t3, -1  /* t3-- */
  bnez	t3, .LTcmallocSlab_Internal_PopBatch_loop
  slli	a4, a6, 16  /* a4 = a6 << 16 */
  sub	t2, t2, a6  /* current -= a6 */
  add	t2, t2, a4  /* remaining_capacity += a6 */
  add	a4, t4, t6  /* a4 = hdr */
  sw	t2, 0(a4)   /* *hdr = new [current, remaining_capacity] */
.LTcmallocSlab_Internal_PopBatch_commit:
  mv	a0, a6      /* number of items popped */
  ret
.LTcmallocSlab_Internal_PopBatch_empty:
  li	a0, 0
  ret
  CFI(.cfi_endproc)
ENCODE_SIZE(TcmallocSlab_Internal_PopBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PopBatch)

.section .note.GNU-stack,"",@progbits
