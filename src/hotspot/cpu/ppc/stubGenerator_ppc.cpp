/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2025 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "asm/macroAssembler.inline.hpp"
#include "compiler/oopMap.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "interpreter/interpreter.hpp"
#include "nativeInst_ppc.hpp"
#include "oops/instanceOop.hpp"
#include "oops/method.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "prims/upcallLinker.hpp"
#include "runtime/continuation.hpp"
#include "runtime/continuationEntry.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/align.hpp"
#include "utilities/powerOfTwo.hpp"
#if INCLUDE_ZGC
#include "gc/z/zBarrierSetAssembler.hpp"
#endif

// Declaration and definition of StubGenerator (no .hpp file).
// For a more detailed description of the stub routine structure
// see the comment in stubRoutines.hpp.

#define __ _masm->

#ifdef PRODUCT
#define BLOCK_COMMENT(str) // nothing
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#if defined(ABI_ELFv2)
#define STUB_ENTRY(name) StubRoutines::name
#else
#define STUB_ENTRY(name) ((FunctionDescriptor*)StubRoutines::name)->entry()
#endif

class StubGenerator: public StubCodeGenerator {
 private:

  // Call stubs are used to call Java from C
  //
  // Arguments:
  //
  //   R3  - call wrapper address     : address
  //   R4  - result                   : intptr_t*
  //   R5  - result type              : BasicType
  //   R6  - method                   : Method
  //   R7  - frame mgr entry point    : address
  //   R8  - parameter block          : intptr_t*
  //   R9  - parameter count in words : int
  //   R10 - thread                   : Thread*
  //
  address generate_call_stub(address& return_address) {
    // Setup a new c frame, copy java arguments, call template interpreter or
    // native_entry, and process result.

    StubGenStubId stub_id = StubGenStubId::call_stub_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    int save_nonvolatile_registers_size = __ save_nonvolatile_registers_size(true, SuperwordUseVSX);

    // some sanity checks
    STATIC_ASSERT(StackAlignmentInBytes == 16);
    assert((sizeof(frame::native_abi_minframe) % 16) == 0,    "unaligned");
    assert((sizeof(frame::native_abi_reg_args) % 16) == 0,    "unaligned");
    assert((save_nonvolatile_registers_size % 16) == 0,       "unaligned");
    assert((sizeof(frame::parent_ijava_frame_abi) % 16) == 0, "unaligned");
    assert((sizeof(frame::entry_frame_locals) % 16) == 0,     "unaligned");

    Register r_arg_call_wrapper_addr        = R3;
    Register r_arg_result_addr              = R4;
    Register r_arg_result_type              = R5;
    Register r_arg_method                   = R6;
    Register r_arg_entry                    = R7;
    Register r_arg_argument_addr            = R8;
    Register r_arg_argument_count           = R9;
    Register r_arg_thread                   = R10;

    Register r_entryframe_fp                = R2; // volatile
    Register r_argument_size                = R11_scratch1; // volatile
    Register r_top_of_arguments_addr        = R21_tmp1;

    {
      // Stack on entry to call_stub:
      //
      //      F1      [C_FRAME]
      //              ...
      Register r_frame_size  = R12_scratch2; // volatile
      Label arguments_copied;

      // Save LR/CR to caller's C_FRAME.
      __ save_LR_CR(R0);

      // Keep copy of our frame pointer (caller's SP).
      __ mr(r_entryframe_fp, R1_SP);

      // calculate frame size
      STATIC_ASSERT(Interpreter::logStackElementSize == 3);

      // space for arguments aligned up: ((arg_count + 1) * 8) &~ 15
      __ addi(r_frame_size, r_arg_argument_count, 1);
      __ rldicr(r_frame_size, r_frame_size, 3, 63 - 4);

      // this is the pure space for arguments (excluding alignment padding)
      __ sldi(r_argument_size, r_arg_argument_count, 3);

      __ addi(r_frame_size, r_frame_size,
              save_nonvolatile_registers_size + frame::entry_frame_locals_size + frame::top_ijava_frame_abi_size);

      // push ENTRY_FRAME
      __ push_frame(r_frame_size, R0);

      // Save non-volatiles registers to ENTRY_FRAME.
      __ save_nonvolatile_registers(r_entryframe_fp, -(frame::entry_frame_locals_size + save_nonvolatile_registers_size),
                                    true, SuperwordUseVSX);

      BLOCK_COMMENT("Push ENTRY_FRAME including arguments");
      // Push ENTRY_FRAME including arguments:
      //
      //      F0      [TOP_IJAVA_FRAME_ABI]
      //              alignment (optional)
      //              [outgoing Java arguments]
      //              [non-volatiles]
      //              [ENTRY_FRAME_LOCALS]
      //      F1      [C_FRAME]
      //              ...

      // initialize call_stub locals (step 1)
      __ std(r_arg_call_wrapper_addr, _entry_frame_locals_neg(call_wrapper_address), r_entryframe_fp);
      __ std(r_arg_result_addr, _entry_frame_locals_neg(result_address), r_entryframe_fp);
      __ std(r_arg_result_type, _entry_frame_locals_neg(result_type), r_entryframe_fp);
      // we will save arguments_tos_address later

      BLOCK_COMMENT("Copy Java arguments");
      // copy Java arguments

      // Calculate top_of_arguments_addr which will be R17_tos (not prepushed) later.
      __ addi(r_top_of_arguments_addr, r_entryframe_fp,
              -(save_nonvolatile_registers_size + frame::entry_frame_locals_size));
      __ sub(r_top_of_arguments_addr, r_top_of_arguments_addr, r_argument_size);

      // any arguments to copy?
      __ cmpdi(CR0, r_arg_argument_count, 0);
      __ beq(CR0, arguments_copied);

      // prepare loop and copy arguments in reverse order
      {
        Register r_argument_addr     = R22_tmp2;
        Register r_argumentcopy_addr = R23_tmp3;
        // init CTR with arg_argument_count
        __ mtctr(r_arg_argument_count);

        // let r_argumentcopy_addr point to last outgoing Java arguments P
        __ mr(r_argumentcopy_addr, r_top_of_arguments_addr);

        // let r_argument_addr point to last incoming java argument
        __ add(r_argument_addr, r_arg_argument_addr, r_argument_size);
        __ addi(r_argument_addr, r_argument_addr, -BytesPerWord);

        // now loop while CTR > 0 and copy arguments
        {
          Label next_argument;
          __ bind(next_argument);

          __ ld(R0, 0, r_argument_addr);
          // argument_addr--;
          __ addi(r_argument_addr, r_argument_addr, -BytesPerWord);
          __ std(R0, 0, r_argumentcopy_addr);
          // argumentcopy_addr++;
          __ addi(r_argumentcopy_addr, r_argumentcopy_addr, BytesPerWord);

          __ bdnz(next_argument);
        }
      }

      // Arguments copied, continue.
      __ bind(arguments_copied);
    }

    {
      BLOCK_COMMENT("Call template interpreter or native entry.");
      assert_different_registers(r_arg_entry, r_top_of_arguments_addr, r_arg_method, r_arg_thread);

      // Register state on entry to template interpreter / native entry:
      //
      //   tos         -  intptr_t*    sender tos (prepushed) Lesp = (SP) + copied_arguments_offset - 8
      //   R19_method  -  Method
      //   R16_thread  -  JavaThread*

      // Tos must point to last argument - element_size.
      const Register tos = R15_esp;

      __ addi(tos, r_top_of_arguments_addr, -Interpreter::stackElementSize);

      // initialize call_stub locals (step 2)
      // now save tos as arguments_tos_address
      __ std(tos, _entry_frame_locals_neg(arguments_tos_address), r_entryframe_fp);

      // load argument registers for call
      __ mr(R19_method, r_arg_method);
      __ mr(R16_thread, r_arg_thread);
      assert(tos != r_arg_method, "trashed r_arg_method");
      assert(tos != r_arg_thread && R19_method != r_arg_thread, "trashed r_arg_thread");

      // Set R15_prev_state to 0 for simplifying checks in callee.
      __ load_const_optimized(R25_templateTableBase, (address)Interpreter::dispatch_table((TosState)0), R0);
      // Stack on entry to template interpreter / native entry:
      //
      //      F0      [TOP_IJAVA_FRAME_ABI]
      //              alignment (optional)
      //              [outgoing Java arguments]
      //              [non-volatiles]
      //              [ENTRY_FRAME_LOCALS]
      //      F1      [C_FRAME]
      //              ...
      //

      // global toc register
      __ load_const_optimized(R29_TOC, MacroAssembler::global_toc(), R0);
      // Remember the senderSP so we interpreter can pop c2i arguments off of the stack
      // when called via a c2i.

      // Pass initial_caller_sp to framemanager.
      __ mr(R21_sender_SP, R1_SP);

      // Do a light-weight C-call here, r_arg_entry holds the address
      // of the interpreter entry point (template interpreter or native entry)
      // and save runtime-value of LR in return_address.
      assert(r_arg_entry != tos && r_arg_entry != R19_method && r_arg_entry != R16_thread,
             "trashed r_arg_entry");
      return_address = __ call_stub(r_arg_entry);
    }

    {
      BLOCK_COMMENT("Returned from template interpreter or native entry.");
      // Now pop frame, process result, and return to caller.

      // Stack on exit from template interpreter / native entry:
      //
      //      F0      [ABI]
      //              ...
      //              [non-volatiles]
      //              [ENTRY_FRAME_LOCALS]
      //      F1      [C_FRAME]
      //              ...
      //
      // Just pop the topmost frame ...
      //

      Label ret_is_object;
      Label ret_is_long;
      Label ret_is_float;
      Label ret_is_double;

      Register r_lr = R11_scratch1;
      Register r_cr = R12_scratch2;

      // Reload some volatile registers which we've spilled before the call
      // to template interpreter / native entry.
      // Access all locals via frame pointer, because we know nothing about
      // the topmost frame's size.
      __ ld(r_entryframe_fp, _abi0(callers_sp), R1_SP); // restore after call
      assert_different_registers(r_entryframe_fp, R3_RET, r_arg_result_addr, r_arg_result_type, r_cr, r_lr);
      __ ld(r_arg_result_addr, _entry_frame_locals_neg(result_address), r_entryframe_fp);
      __ ld(r_arg_result_type, _entry_frame_locals_neg(result_type), r_entryframe_fp);
      __ ld(r_cr, _abi0(cr), r_entryframe_fp);
      __ ld(r_lr, _abi0(lr), r_entryframe_fp);
      __ mtcr(r_cr); // restore CR
      __ mtlr(r_lr); // restore LR

      // Store result depending on type. Everything that is not
      // T_OBJECT, T_LONG, T_FLOAT, or T_DOUBLE is treated as T_INT.
      // Using volatile CRs.
      __ cmpwi(CR1, r_arg_result_type, T_OBJECT);
      __ cmpwi(CR5, r_arg_result_type, T_LONG);
      __ cmpwi(CR6, r_arg_result_type, T_FLOAT);
      __ cmpwi(CR7, r_arg_result_type, T_DOUBLE);

      __ pop_cont_fastpath(); // kills CR0, uses R16_thread

      // restore non-volatile registers
      __ restore_nonvolatile_registers(r_entryframe_fp, -(frame::entry_frame_locals_size + save_nonvolatile_registers_size),
                                       true, SuperwordUseVSX);

      // pop frame
      __ mr(R1_SP, r_entryframe_fp);

      // Stack on exit from call_stub:
      //
      //      0       [C_FRAME]
      //              ...
      //
      //  no call_stub frames left.

      __ beq(CR1, ret_is_object);
      __ beq(CR5, ret_is_long);
      __ beq(CR6, ret_is_float);
      __ beq(CR7, ret_is_double);

      // default:
      __ stw(R3_RET, 0, r_arg_result_addr);
      __ blr(); // return to caller

      // case T_OBJECT:
      // case T_LONG:
      __ bind(ret_is_object);
      __ bind(ret_is_long);
      __ std(R3_RET, 0, r_arg_result_addr);
      __ blr(); // return to caller

      // case T_FLOAT:
      __ bind(ret_is_float);
      __ stfs(F1_RET, 0, r_arg_result_addr);
      __ blr(); // return to caller

      // case T_DOUBLE:
      __ bind(ret_is_double);
      __ stfd(F1_RET, 0, r_arg_result_addr);
      __ blr(); // return to caller
    }

    return start;
  }

  // Return point for a Java call if there's an exception thrown in
  // Java code.  The exception is caught and transformed into a
  // pending exception stored in JavaThread that can be tested from
  // within the VM.
  //
  address generate_catch_exception() {
    StubGenStubId stub_id = StubGenStubId::catch_exception_id;
    StubCodeMark mark(this, stub_id);

    address start = __ pc();

    // Registers alive
    //
    //  R16_thread
    //  R3_ARG1 - address of pending exception
    //  R4_ARG2 - return address in call stub

    const Register exception_file = R21_tmp1;
    const Register exception_line = R22_tmp2;

    __ load_const(exception_file, (void*)__FILE__);
    __ load_const(exception_line, (void*)__LINE__);

    __ std(R3_ARG1, in_bytes(JavaThread::pending_exception_offset()), R16_thread);
    // store into `char *'
    __ std(exception_file, in_bytes(JavaThread::exception_file_offset()), R16_thread);
    // store into `int'
    __ stw(exception_line, in_bytes(JavaThread::exception_line_offset()), R16_thread);

    // complete return to VM
    assert(StubRoutines::_call_stub_return_address != nullptr, "must have been generated before");

    __ mtlr(R4_ARG2);
    // continue in call stub
    __ blr();

    return start;
  }

  // Continuation point for runtime calls returning with a pending
  // exception.  The pending exception check happened in the runtime
  // or native call stub.  The pending exception in Thread is
  // converted into a Java-level exception.
  //
  // Read:
  //
  //   LR:     The pc the runtime library callee wants to return to.
  //           Since the exception occurred in the callee, the return pc
  //           from the point of view of Java is the exception pc.
  //   thread: Needed for method handles.
  //
  // Invalidate:
  //
  //   volatile registers (except below).
  //
  // Update:
  //
  //   R4_ARG2: exception
  //
  // (LR is unchanged and is live out).
  //
  address generate_forward_exception() {
    StubGenStubId stub_id = StubGenStubId::forward_exception_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    if (VerifyOops) {
      // Get pending exception oop.
      __ ld(R3_ARG1,
                in_bytes(Thread::pending_exception_offset()),
                R16_thread);
      // Make sure that this code is only executed if there is a pending exception.
      {
        Label L;
        __ cmpdi(CR0, R3_ARG1, 0);
        __ bne(CR0, L);
        __ stop("StubRoutines::forward exception: no pending exception (1)");
        __ bind(L);
      }
      __ verify_oop(R3_ARG1, "StubRoutines::forward exception: not an oop");
    }

    // Save LR/CR and copy exception pc (LR) into R4_ARG2.
    __ save_LR(R4_ARG2);
    __ push_frame_reg_args(0, R0);
    // Find exception handler.
    __ call_VM_leaf(CAST_FROM_FN_PTR(address,
                     SharedRuntime::exception_handler_for_return_address),
                    R16_thread,
                    R4_ARG2);
    // Copy handler's address.
    __ mtctr(R3_RET);
    __ pop_frame();
    __ restore_LR(R0);

    // Set up the arguments for the exception handler:
    //  - R3_ARG1: exception oop
    //  - R4_ARG2: exception pc.

    // Load pending exception oop.
    __ ld(R3_ARG1,
              in_bytes(Thread::pending_exception_offset()),
              R16_thread);

    // The exception pc is the return address in the caller.
    // Must load it into R4_ARG2.
    __ mflr(R4_ARG2);

#ifdef ASSERT
    // Make sure exception is set.
    {
      Label L;
      __ cmpdi(CR0, R3_ARG1, 0);
      __ bne(CR0, L);
      __ stop("StubRoutines::forward exception: no pending exception (2)");
      __ bind(L);
    }
#endif

    // Clear the pending exception.
    __ li(R0, 0);
    __ std(R0,
               in_bytes(Thread::pending_exception_offset()),
               R16_thread);
    // Jump to exception handler.
    __ bctr();

    return start;
  }

#undef __
#define __ _masm->

#if !defined(PRODUCT)
  // Wrapper which calls oopDesc::is_oop_or_null()
  // Only called by MacroAssembler::verify_oop
  static void verify_oop_helper(const char* message, oopDesc* o) {
    if (!oopDesc::is_oop_or_null(o)) {
      fatal("%s. oop: " PTR_FORMAT, message, p2i(o));
    }
    ++ StubRoutines::_verify_oop_count;
  }
#endif

  // Return address of code to be called from code generated by
  // MacroAssembler::verify_oop.
  //
  // Don't generate, rather use C++ code.
  address generate_verify_oop() {
    // this is actually a `FunctionDescriptor*'.
    address start = nullptr;

#if !defined(PRODUCT)
    start = CAST_FROM_FN_PTR(address, verify_oop_helper);
#endif

    return start;
  }

  // Computes the Galois/Counter Mode (GCM) product and reduction.
  //
  // This function performs polynomial multiplication of the subkey H with
  // the current GHASH state using vectorized polynomial multiplication (`vpmsumd`).
  // The subkey H is divided into lower, middle, and higher halves.
  // The multiplication results are reduced using `vConstC2` to stay within GF(2^128).
  // The final computed value is stored back into `vState`.
  static void computeGCMProduct(MacroAssembler* _masm,
                                VectorRegister vLowerH, VectorRegister vH, VectorRegister vHigherH,
                                VectorRegister vConstC2, VectorRegister vZero, VectorRegister vState,
                                VectorRegister vLowProduct, VectorRegister vMidProduct, VectorRegister vHighProduct,
                                VectorRegister vReducedLow, VectorRegister vTmp8, VectorRegister vTmp9,
                                VectorRegister vCombinedResult, VectorRegister vSwappedH) {
    __ vxor(vH, vH, vState);
    __ vpmsumd(vLowProduct, vLowerH, vH);                          // L : Lower Half of subkey H
    __ vpmsumd(vMidProduct, vSwappedH, vH);                        // M : Combined halves of subkey H
    __ vpmsumd(vHighProduct, vHigherH, vH);                        // H : Higher Half of subkey H
    __ vpmsumd(vReducedLow, vLowProduct, vConstC2);                // Reduction
    __ vsldoi(vTmp8, vMidProduct, vZero, 8);                       // mL : Extract the lower 64 bits of M
    __ vsldoi(vTmp9, vZero, vMidProduct, 8);                       // mH : Extract the higher 64 bits of M
    __ vxor(vLowProduct, vLowProduct, vTmp8);                      // LL + mL : Partial result for lower half
    __ vxor(vHighProduct, vHighProduct, vTmp9);                    // HH + mH : Partial result for upper half
    __ vsldoi(vLowProduct, vLowProduct, vLowProduct, 8);           // Swap
    __ vxor(vLowProduct, vLowProduct, vReducedLow);
    __ vsldoi(vCombinedResult, vLowProduct, vLowProduct, 8);       // Swap
    __ vpmsumd(vLowProduct, vLowProduct, vConstC2);                // Reduction using constant
    __ vxor(vCombinedResult, vCombinedResult, vHighProduct);       // Combine reduced Low & High products
    __ vxor(vState, vLowProduct, vCombinedResult);
  }

  // Generate stub for ghash process blocks.
  //
  // Arguments for generated stub:
  //      state:    R3_ARG1 (long[] state)
  //      subkeyH:  R4_ARG2 (long[] subH)
  //      data:     R5_ARG3 (byte[] data)
  //      blocks:   R6_ARG4 (number of 16-byte blocks to process)
  //
  // The polynomials are processed in bit-reflected order for efficiency reasons.
  // This optimization leverages the structure of the Galois field arithmetic
  // to minimize the number of bit manipulations required during multiplication.
  // For an explanation of how this works, refer :
  // Vinodh Gopal, Erdinc Ozturk, Wajdi Feghali, Jim Guilford, Gil Wolrich,
  // Martin Dixon. "Optimized Galois-Counter-Mode Implementation on Intel®
  // Architecture Processor"
  // http://web.archive.org/web/20130609111954/http://www.intel.com/content/dam/www/public/us/en/documents/white-papers/communications-ia-galois-counter-mode-paper.pdf
  //
  //
  address generate_ghash_processBlocks() {
    StubCodeMark mark(this, "StubRoutines", "ghash");
    address start = __ function_entry();

    // Registers for parameters
    Register state = R3_ARG1;                     // long[] state
    Register subkeyH = R4_ARG2;                   // long[] subH
    Register data = R5_ARG3;                      // byte[] data
    Register blocks = R6_ARG4;
    Register temp1 = R8;
    // Vector Registers
    VectorRegister vZero = VR0;
    VectorRegister vH = VR1;
    VectorRegister vLowerH = VR2;
    VectorRegister vHigherH = VR3;
    VectorRegister vLowProduct = VR4;
    VectorRegister vMidProduct = VR5;
    VectorRegister vHighProduct = VR6;
    VectorRegister vReducedLow = VR7;
    VectorRegister vTmp8 = VR8;
    VectorRegister vTmp9 = VR9;
    VectorRegister vTmp10 = VR10;
    VectorRegister vSwappedH = VR11;
    VectorRegister vTmp12 = VR12;
    VectorRegister loadOrder = VR13;
    VectorRegister vHigh = VR14;
    VectorRegister vLow = VR15;
    VectorRegister vState = VR16;
    VectorRegister vPerm = VR17;
    VectorRegister vCombinedResult = VR18;
    VectorRegister vConstC2 = VR19;

    __ li(temp1, 0xc2);
    __ sldi(temp1, temp1, 56);
    __ vspltisb(vZero, 0);
    __ mtvrd(vConstC2, temp1);
    __ lxvd2x(vH->to_vsr(), subkeyH);
    __ lxvd2x(vState->to_vsr(), state);
    // Operations to obtain lower and higher bytes of subkey H.
    __ vspltisb(vReducedLow, 1);
    __ vspltisb(vTmp10, 7);
    __ vsldoi(vTmp8, vZero, vReducedLow, 1);            // 0x1
    __ vor(vTmp8, vConstC2, vTmp8);                     // 0xC2...1
    __ vsplt(vTmp9, 0, vH);                             // MSB of H
    __ vsl(vH, vH, vReducedLow);                        // Carry = H<<7
    __ vsrab(vTmp9, vTmp9, vTmp10);
    __ vand(vTmp9, vTmp9, vTmp8);                       // Carry
    __ vxor(vTmp10, vH, vTmp9);
    __ vsldoi(vConstC2, vZero, vConstC2, 8);
    __ vsldoi(vSwappedH, vTmp10, vTmp10, 8);            // swap Lower and Higher Halves of subkey H
    __ vsldoi(vLowerH, vZero, vSwappedH, 8);            // H.L
    __ vsldoi(vHigherH, vSwappedH, vZero, 8);           // H.H
#ifdef ASSERT
    __ cmpwi(CR0, blocks, 0);                           // Compare 'blocks' (R6_ARG4) with zero
    __ asm_assert_ne("blocks should NOT be zero");
#endif
    __ clrldi(blocks, blocks, 32);
    __ mtctr(blocks);
    __ lvsl(loadOrder, temp1);
#ifdef VM_LITTLE_ENDIAN
    __ vspltisb(vTmp12, 0xf);
    __ vxor(loadOrder, loadOrder, vTmp12);
#define LE_swap_bytes(x) __ vec_perm(x, x, x, loadOrder)
#else
#define LE_swap_bytes(x)
#endif

    // This code performs Karatsuba multiplication in Galois fields to compute the GHASH operation.
    //
    // The Karatsuba method breaks the multiplication of two 128-bit numbers into smaller parts,
    // performing three 128-bit multiplications and combining the results efficiently.
    //
    // (C1:C0) = A1*B1, (D1:D0) = A0*B0, (E1:E0) = (A0+A1)(B0+B1)
    // (A1:A0)(B1:B0) = C1:(C0+C1+D1+E1):(D1+C0+D0+E0):D0
    //
    // Inputs:
    // - vH:       The data vector (state), containing both B0 (lower half) and B1 (higher half).
    // - vLowerH:  Lower half of the subkey H (A0).
    // - vHigherH: Higher half of the subkey H (A1).
    // - vConstC2: Constant used for reduction (for final processing).
    //
    // References:
    // Shay Gueron, Michael E. Kounavis.
    // "Intel® Carry-Less Multiplication Instruction and its Usage for Computing the GCM Mode"
    // https://web.archive.org/web/20110609115824/https://software.intel.com/file/24918
    //
    Label L_aligned_loop, L_store, L_unaligned_loop, L_initialize_unaligned_loop;
    __ andi(temp1, data, 15);
    __ cmpwi(CR0, temp1, 0);
    __ bne(CR0, L_initialize_unaligned_loop);

    __ bind(L_aligned_loop);
      __ lvx(vH, temp1, data);
      LE_swap_bytes(vH);
      computeGCMProduct(_masm, vLowerH, vH, vHigherH, vConstC2, vZero, vState,
                    vLowProduct, vMidProduct, vHighProduct, vReducedLow, vTmp8, vTmp9, vCombinedResult, vSwappedH);
      __ addi(data, data, 16);
    __ bdnz(L_aligned_loop);
    __ b(L_store);

    __ bind(L_initialize_unaligned_loop);
    __ li(temp1, 0);
    __ lvsl(vPerm, temp1, data);
    __ lvx(vHigh, temp1, data);
#ifdef VM_LITTLE_ENDIAN
    __ vspltisb(vTmp12, -1);
    __ vxor(vPerm, vPerm, vTmp12);
#endif
    __ bind(L_unaligned_loop);
      __ addi(data, data, 16);
      __ lvx(vLow, temp1, data);
      __ vec_perm(vH, vHigh, vLow, vPerm);
      computeGCMProduct(_masm, vLowerH, vH, vHigherH, vConstC2, vZero, vState,
                    vLowProduct, vMidProduct, vHighProduct, vReducedLow, vTmp8, vTmp9, vCombinedResult, vSwappedH);
      __ vmr(vHigh, vLow);
    __ bdnz(L_unaligned_loop);

    __ bind(L_store);
    __ stxvd2x(vState->to_vsr(), state);
    __ blr();

    return start;
  }
  // -XX:+OptimizeFill : convert fill/copy loops into intrinsic
  //
  // The code is implemented(ported from sparc) as we believe it benefits JVM98, however
  // tracing(-XX:+TraceOptimizeFill) shows the intrinsic replacement doesn't happen at all!
  //
  // Source code in function is_range_check_if() shows that OptimizeFill relaxed the condition
  // for turning on loop predication optimization, and hence the behavior of "array range check"
  // and "loop invariant check" could be influenced, which potentially boosted JVM98.
  //
  // Generate stub for disjoint short fill. If "aligned" is true, the
  // "to" address is assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //   to:    R3_ARG1
  //   value: R4_ARG2
  //   count: R5_ARG3 treated as signed
  //
  address generate_fill(StubGenStubId stub_id) {
    BasicType t;
    bool aligned;

    switch (stub_id) {
    case jbyte_fill_id:
      t = T_BYTE;
      aligned = false;
      break;
    case jshort_fill_id:
      t = T_SHORT;
      aligned = false;
      break;
    case jint_fill_id:
      t = T_INT;
      aligned = false;
      break;
    case arrayof_jbyte_fill_id:
      t = T_BYTE;
      aligned = true;
      break;
    case arrayof_jshort_fill_id:
      t = T_SHORT;
      aligned = true;
      break;
    case arrayof_jint_fill_id:
      t = T_INT;
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    const Register to    = R3_ARG1;   // source array address
    const Register value = R4_ARG2;   // fill value
    const Register count = R5_ARG3;   // elements count
    const Register temp  = R6_ARG4;   // temp register

    //assert_clean_int(count, O3);    // Make sure 'count' is clean int.

    Label L_exit, L_skip_align1, L_skip_align2, L_fill_byte;
    Label L_fill_2_bytes, L_fill_4_bytes, L_fill_elements, L_fill_32_bytes;

    int shift = -1;
    switch (t) {
       case T_BYTE:
        shift = 2;
        // Clone bytes (zero extend not needed because store instructions below ignore high order bytes).
        __ rldimi(value, value, 8, 48);     // 8 bit -> 16 bit
        __ cmpdi(CR0, count, 2<<shift);    // Short arrays (< 8 bytes) fill by element.
        __ blt(CR0, L_fill_elements);
        __ rldimi(value, value, 16, 32);    // 16 bit -> 32 bit
        break;
       case T_SHORT:
        shift = 1;
        // Clone bytes (zero extend not needed because store instructions below ignore high order bytes).
        __ rldimi(value, value, 16, 32);    // 16 bit -> 32 bit
        __ cmpdi(CR0, count, 2<<shift);    // Short arrays (< 8 bytes) fill by element.
        __ blt(CR0, L_fill_elements);
        break;
      case T_INT:
        shift = 0;
        __ cmpdi(CR0, count, 2<<shift);    // Short arrays (< 8 bytes) fill by element.
        __ blt(CR0, L_fill_4_bytes);
        break;
      default: ShouldNotReachHere();
    }

    if (!aligned && (t == T_BYTE || t == T_SHORT)) {
      // Align source address at 4 bytes address boundary.
      if (t == T_BYTE) {
        // One byte misalignment happens only for byte arrays.
        __ andi_(temp, to, 1);
        __ beq(CR0, L_skip_align1);
        __ stb(value, 0, to);
        __ addi(to, to, 1);
        __ addi(count, count, -1);
        __ bind(L_skip_align1);
      }
      // Two bytes misalignment happens only for byte and short (char) arrays.
      __ andi_(temp, to, 2);
      __ beq(CR0, L_skip_align2);
      __ sth(value, 0, to);
      __ addi(to, to, 2);
      __ addi(count, count, -(1 << (shift - 1)));
      __ bind(L_skip_align2);
    }

    if (!aligned) {
      // Align to 8 bytes, we know we are 4 byte aligned to start.
      __ andi_(temp, to, 7);
      __ beq(CR0, L_fill_32_bytes);
      __ stw(value, 0, to);
      __ addi(to, to, 4);
      __ addi(count, count, -(1 << shift));
      __ bind(L_fill_32_bytes);
    }

    __ li(temp, 8<<shift);                  // Prepare for 32 byte loop.
    // Clone bytes int->long as above.
    __ rldimi(value, value, 32, 0);         // 32 bit -> 64 bit

    Label L_check_fill_8_bytes;
    // Fill 32-byte chunks.
    __ subf_(count, temp, count);
    __ blt(CR0, L_check_fill_8_bytes);

    Label L_fill_32_bytes_loop;
    __ align(32);
    __ bind(L_fill_32_bytes_loop);

    __ std(value, 0, to);
    __ std(value, 8, to);
    __ subf_(count, temp, count);           // Update count.
    __ std(value, 16, to);
    __ std(value, 24, to);

    __ addi(to, to, 32);
    __ bge(CR0, L_fill_32_bytes_loop);

    __ bind(L_check_fill_8_bytes);
    __ add_(count, temp, count);
    __ beq(CR0, L_exit);
    __ addic_(count, count, -(2 << shift));
    __ blt(CR0, L_fill_4_bytes);

    //
    // Length is too short, just fill 8 bytes at a time.
    //
    Label L_fill_8_bytes_loop;
    __ bind(L_fill_8_bytes_loop);
    __ std(value, 0, to);
    __ addic_(count, count, -(2 << shift));
    __ addi(to, to, 8);
    __ bge(CR0, L_fill_8_bytes_loop);

    // Fill trailing 4 bytes.
    __ bind(L_fill_4_bytes);
    __ andi_(temp, count, 1<<shift);
    __ beq(CR0, L_fill_2_bytes);

    __ stw(value, 0, to);
    if (t == T_BYTE || t == T_SHORT) {
      __ addi(to, to, 4);
      // Fill trailing 2 bytes.
      __ bind(L_fill_2_bytes);
      __ andi_(temp, count, 1<<(shift-1));
      __ beq(CR0, L_fill_byte);
      __ sth(value, 0, to);
      if (t == T_BYTE) {
        __ addi(to, to, 2);
        // Fill trailing byte.
        __ bind(L_fill_byte);
        __ andi_(count, count, 1);
        __ beq(CR0, L_exit);
        __ stb(value, 0, to);
      } else {
        __ bind(L_fill_byte);
      }
    } else {
      __ bind(L_fill_2_bytes);
    }
    __ bind(L_exit);
    __ blr();

    // Handle copies less than 8 bytes. Int is handled elsewhere.
    if (t == T_BYTE) {
      __ bind(L_fill_elements);
      Label L_fill_2, L_fill_4;
      __ andi_(temp, count, 1);
      __ beq(CR0, L_fill_2);
      __ stb(value, 0, to);
      __ addi(to, to, 1);
      __ bind(L_fill_2);
      __ andi_(temp, count, 2);
      __ beq(CR0, L_fill_4);
      __ stb(value, 0, to);
      __ stb(value, 0, to);
      __ addi(to, to, 2);
      __ bind(L_fill_4);
      __ andi_(temp, count, 4);
      __ beq(CR0, L_exit);
      __ stb(value, 0, to);
      __ stb(value, 1, to);
      __ stb(value, 2, to);
      __ stb(value, 3, to);
      __ blr();
    }

    if (t == T_SHORT) {
      Label L_fill_2;
      __ bind(L_fill_elements);
      __ andi_(temp, count, 1);
      __ beq(CR0, L_fill_2);
      __ sth(value, 0, to);
      __ addi(to, to, 2);
      __ bind(L_fill_2);
      __ andi_(temp, count, 2);
      __ beq(CR0, L_exit);
      __ sth(value, 0, to);
      __ sth(value, 2, to);
      __ blr();
    }
    return start;
  }

  inline void assert_positive_int(Register count) {
#ifdef ASSERT
    __ srdi_(R0, count, 31);
    __ asm_assert_eq("missing zero extend");
#endif
  }

  // Generate overlap test for array copy stubs.
  //
  // Input:
  //   R3_ARG1    -  from
  //   R4_ARG2    -  to
  //   R5_ARG3    -  element count
  //
  void array_overlap_test(address no_overlap_target, int log2_elem_size) {
    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;

    assert_positive_int(R5_ARG3);

    __ subf(tmp1, R3_ARG1, R4_ARG2); // distance in bytes
    __ sldi(tmp2, R5_ARG3, log2_elem_size); // size in bytes
    __ cmpld(CR0, R3_ARG1, R4_ARG2); // Use unsigned comparison!
    __ cmpld(CR1, tmp1, tmp2);
    __ crnand(CR0, Assembler::less, CR1, Assembler::less);
    // Overlaps if Src before dst and distance smaller than size.
    // Branch to forward copy routine otherwise (within range of 32kB).
    __ bc(Assembler::bcondCRbiIs1, Assembler::bi0(CR0, Assembler::less), no_overlap_target);

    // need to copy backwards
  }

  // This is common errorexit stub for UnsafeMemoryAccess.
  address generate_unsafecopy_common_error_exit() {
    address start_pc = __ pc();
    Register tmp1 = R6_ARG4;
    // probably copy stub would have changed value reset it.
    if (VM_Version::has_mfdscr()) {
      __ load_const_optimized(tmp1, VM_Version::_dscr_val);
      __ mtdscr(tmp1);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();
    return start_pc;
  }

  // The guideline in the implementations of generate_disjoint_xxx_copy
  // (xxx=byte,short,int,long,oop) is to copy as many elements as possible with
  // single instructions, but to avoid alignment interrupts (see subsequent
  // comment). Furthermore, we try to minimize misaligned access, even
  // though they cause no alignment interrupt.
  //
  // In Big-Endian mode, the PowerPC architecture requires implementations to
  // handle automatically misaligned integer halfword and word accesses,
  // word-aligned integer doubleword accesses, and word-aligned floating-point
  // accesses. Other accesses may or may not generate an Alignment interrupt
  // depending on the implementation.
  // Alignment interrupt handling may require on the order of hundreds of cycles,
  // so every effort should be made to avoid misaligned memory values.
  //
  //
  // Generate stub for disjoint byte copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_disjoint_byte_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jbyte_disjoint_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jbyte_disjoint_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);

    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R9_ARG7;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    Label l_1, l_2, l_3, l_4, l_5, l_6, l_7, l_8, l_9, l_10;
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);

      // Don't try anything fancy if arrays don't have many elements.
      __ li(tmp3, 0);
      __ cmpwi(CR0, R5_ARG3, 17);
      __ ble(CR0, l_6); // copy 4 at a time

      if (!aligned) {
        __ xorr(tmp1, R3_ARG1, R4_ARG2);
        __ andi_(tmp1, tmp1, 3);
        __ bne(CR0, l_6); // If arrays don't have the same alignment mod 4, do 4 element copy.

        // Copy elements if necessary to align to 4 bytes.
        __ neg(tmp1, R3_ARG1); // Compute distance to alignment boundary.
        __ andi_(tmp1, tmp1, 3);
        __ beq(CR0, l_2);

        __ subf(R5_ARG3, tmp1, R5_ARG3);
        __ bind(l_9);
        __ lbz(tmp2, 0, R3_ARG1);
        __ addic_(tmp1, tmp1, -1);
        __ stb(tmp2, 0, R4_ARG2);
        __ addi(R3_ARG1, R3_ARG1, 1);
        __ addi(R4_ARG2, R4_ARG2, 1);
        __ bne(CR0, l_9);

        __ bind(l_2);
      }

      // copy 8 elements at a time
      __ xorr(tmp2, R3_ARG1, R4_ARG2); // skip if src & dest have differing alignment mod 8
      __ andi_(tmp1, tmp2, 7);
      __ bne(CR0, l_7); // not same alignment -> to or from is aligned -> copy 8

      // copy a 2-element word if necessary to align to 8 bytes
      __ andi_(R0, R3_ARG1, 7);
      __ beq(CR0, l_7);

      __ lwzx(tmp2, R3_ARG1, tmp3);
      __ addi(R5_ARG3, R5_ARG3, -4);
      __ stwx(tmp2, R4_ARG2, tmp3);
      { // FasterArrayCopy
        __ addi(R3_ARG1, R3_ARG1, 4);
        __ addi(R4_ARG2, R4_ARG2, 4);
      }
      __ bind(l_7);

      { // FasterArrayCopy
        __ cmpwi(CR0, R5_ARG3, 31);
        __ ble(CR0, l_6); // copy 2 at a time if less than 32 elements remain

        __ srdi(tmp1, R5_ARG3, 5);
        __ andi_(R5_ARG3, R5_ARG3, 31);
        __ mtctr(tmp1);


        // Prefetch the data into the L2 cache.
        __ dcbt(R3_ARG1, 0);

        // If supported set DSCR pre-fetch to deepest.
        if (VM_Version::has_mfdscr()) {
          __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
          __ mtdscr(tmp2);
        }
        __ li(tmp1, 16);

        // Backbranch target aligned to 32-byte. Not 16-byte align as
        // loop contains < 8 instructions that fit inside a single
        // i-cache sector.
        __ align(32);

        __ bind(l_10);
        // Use loop with VSX load/store instructions to
        // copy 32 elements a time.
        __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load src
        __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst
        __ lxvd2x(tmp_vsr2, tmp1, R3_ARG1);  // Load src + 16
        __ stxvd2x(tmp_vsr2, tmp1, R4_ARG2); // Store to dst + 16
        __ addi(R3_ARG1, R3_ARG1, 32);       // Update src+=32
        __ addi(R4_ARG2, R4_ARG2, 32);       // Update dsc+=32
        __ bdnz(l_10);                       // Dec CTR and loop if not zero.

        // Restore DSCR pre-fetch value.
        if (VM_Version::has_mfdscr()) {
          __ load_const_optimized(tmp2, VM_Version::_dscr_val);
          __ mtdscr(tmp2);
        }

     } // FasterArrayCopy

      __ bind(l_6);

      // copy 4 elements at a time
      __ cmpwi(CR0, R5_ARG3, 4);
      __ blt(CR0, l_1);
      __ srdi(tmp1, R5_ARG3, 2);
      __ mtctr(tmp1); // is > 0
      __ andi_(R5_ARG3, R5_ARG3, 3);

      { // FasterArrayCopy
        __ addi(R3_ARG1, R3_ARG1, -4);
        __ addi(R4_ARG2, R4_ARG2, -4);
        __ bind(l_3);
        __ lwzu(tmp2, 4, R3_ARG1);
        __ stwu(tmp2, 4, R4_ARG2);
        __ bdnz(l_3);
        __ addi(R3_ARG1, R3_ARG1, 4);
        __ addi(R4_ARG2, R4_ARG2, 4);
      }

      // do single element copy
      __ bind(l_1);
      __ cmpwi(CR0, R5_ARG3, 0);
      __ beq(CR0, l_4);

      { // FasterArrayCopy
        __ mtctr(R5_ARG3);
        __ addi(R3_ARG1, R3_ARG1, -1);
        __ addi(R4_ARG2, R4_ARG2, -1);

        __ bind(l_5);
        __ lbzu(tmp2, 1, R3_ARG1);
        __ stbu(tmp2, 1, R4_ARG2);
        __ bdnz(l_5);
      }
    }

    __ bind(l_4);
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate stub for conjoint byte copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_conjoint_byte_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jbyte_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jbyte_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);

    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;

    address nooverlap_target = aligned ?
      STUB_ENTRY(arrayof_jbyte_disjoint_arraycopy()) :
      STUB_ENTRY(jbyte_disjoint_arraycopy());

    array_overlap_test(nooverlap_target, 0);
    // Do reverse copy. We assume the case of actual overlap is rare enough
    // that we don't have to optimize it.
    Label l_1, l_2;
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      __ b(l_2);
      __ bind(l_1);
      __ stbx(tmp1, R4_ARG2, R5_ARG3);
      __ bind(l_2);
      __ addic_(R5_ARG3, R5_ARG3, -1);
      __ lbzx(tmp1, R3_ARG1, R5_ARG3);
      __ bge(CR0, l_1);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate stub for disjoint short copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //  elm.count: R5_ARG3 treated as signed
  //
  // Strategy for aligned==true:
  //
  //  If length <= 9:
  //     1. copy 2 elements at a time (l_6)
  //     2. copy last element if original element count was odd (l_1)
  //
  //  If length > 9:
  //     1. copy 4 elements at a time until less than 4 elements are left (l_7)
  //     2. copy 2 elements at a time until less than 2 elements are left (l_6)
  //     3. copy last element if one was left in step 2. (l_1)
  //
  //
  // Strategy for aligned==false:
  //
  //  If length <= 9: same as aligned==true case, but NOTE: load/stores
  //                  can be unaligned (see comment below)
  //
  //  If length > 9:
  //     1. continue with step 6. if the alignment of from and to mod 4
  //        is different.
  //     2. align from and to to 4 bytes by copying 1 element if necessary
  //     3. at l_2 from and to are 4 byte aligned; continue with
  //        5. if they cannot be aligned to 8 bytes because they have
  //        got different alignment mod 8.
  //     4. at this point we know that both, from and to, have the same
  //        alignment mod 8, now copy one element if necessary to get
  //        8 byte alignment of from and to.
  //     5. copy 4 elements at a time until less than 4 elements are
  //        left; depending on step 3. all load/stores are aligned or
  //        either all loads or all stores are unaligned.
  //     6. copy 2 elements at a time until less than 2 elements are
  //        left (l_6); arriving here from step 1., there is a chance
  //        that all accesses are unaligned.
  //     7. copy last element if one was left in step 6. (l_1)
  //
  //  There are unaligned data accesses using integer load/store
  //  instructions in this stub. POWER allows such accesses.
  //
  //  According to the manuals (PowerISA_V2.06_PUBLIC, Book II,
  //  Chapter 2: Effect of Operand Placement on Performance) unaligned
  //  integer load/stores have good performance. Only unaligned
  //  floating point load/stores can have poor performance.
  //
  //  TODO:
  //
  //  1. check if aligning the backbranch target of loops is beneficial
  //
  address generate_disjoint_short_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jshort_disjoint_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jshort_disjoint_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);

    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R9_ARG7;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    address start = __ function_entry();
    assert_positive_int(R5_ARG3);

    Label l_1, l_2, l_3, l_4, l_5, l_6, l_7, l_8, l_9;
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      // don't try anything fancy if arrays don't have many elements
      __ li(tmp3, 0);
      __ cmpwi(CR0, R5_ARG3, 9);
      __ ble(CR0, l_6); // copy 2 at a time

      if (!aligned) {
        __ xorr(tmp1, R3_ARG1, R4_ARG2);
        __ andi_(tmp1, tmp1, 3);
        __ bne(CR0, l_6); // if arrays don't have the same alignment mod 4, do 2 element copy

        // At this point it is guaranteed that both, from and to have the same alignment mod 4.

        // Copy 1 element if necessary to align to 4 bytes.
        __ andi_(tmp1, R3_ARG1, 3);
        __ beq(CR0, l_2);

        __ lhz(tmp2, 0, R3_ARG1);
        __ addi(R3_ARG1, R3_ARG1, 2);
        __ sth(tmp2, 0, R4_ARG2);
        __ addi(R4_ARG2, R4_ARG2, 2);
        __ addi(R5_ARG3, R5_ARG3, -1);
        __ bind(l_2);

        // At this point the positions of both, from and to, are at least 4 byte aligned.

        // Copy 4 elements at a time.
        // Align to 8 bytes, but only if both, from and to, have same alignment mod 8.
        __ xorr(tmp2, R3_ARG1, R4_ARG2);
        __ andi_(tmp1, tmp2, 7);
        __ bne(CR0, l_7); // not same alignment mod 8 -> copy 4, either from or to will be unaligned

        // Copy a 2-element word if necessary to align to 8 bytes.
        __ andi_(R0, R3_ARG1, 7);
        __ beq(CR0, l_7);

        __ lwzx(tmp2, R3_ARG1, tmp3);
        __ addi(R5_ARG3, R5_ARG3, -2);
        __ stwx(tmp2, R4_ARG2, tmp3);
        { // FasterArrayCopy
          __ addi(R3_ARG1, R3_ARG1, 4);
          __ addi(R4_ARG2, R4_ARG2, 4);
        }
      }

      __ bind(l_7);

      // Copy 4 elements at a time; either the loads or the stores can
      // be unaligned if aligned == false.

      { // FasterArrayCopy
        __ cmpwi(CR0, R5_ARG3, 15);
        __ ble(CR0, l_6); // copy 2 at a time if less than 16 elements remain

        __ srdi(tmp1, R5_ARG3, 4);
        __ andi_(R5_ARG3, R5_ARG3, 15);
        __ mtctr(tmp1);


        // Processor supports VSX, so use it to mass copy.

          // Prefetch src data into L2 cache.
          __ dcbt(R3_ARG1, 0);

          // If supported set DSCR pre-fetch to deepest.
          if (VM_Version::has_mfdscr()) {
            __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
            __ mtdscr(tmp2);
          }
          __ li(tmp1, 16);

          // Backbranch target aligned to 32-byte. It's not aligned 16-byte
          // as loop contains < 8 instructions that fit inside a single
          // i-cache sector.
          __ align(32);

          __ bind(l_9);
          // Use loop with VSX load/store instructions to
          // copy 16 elements a time.
          __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load from src.
          __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst.
          __ lxvd2x(tmp_vsr2, R3_ARG1, tmp1);  // Load from src + 16.
          __ stxvd2x(tmp_vsr2, R4_ARG2, tmp1); // Store to dst + 16.
          __ addi(R3_ARG1, R3_ARG1, 32);       // Update src+=32.
          __ addi(R4_ARG2, R4_ARG2, 32);       // Update dsc+=32.
          __ bdnz(l_9);                        // Dec CTR and loop if not zero.

          // Restore DSCR pre-fetch value.
          if (VM_Version::has_mfdscr()) {
            __ load_const_optimized(tmp2, VM_Version::_dscr_val);
            __ mtdscr(tmp2);
          }

      } // FasterArrayCopy
      __ bind(l_6);

      // copy 2 elements at a time
      { // FasterArrayCopy
        __ cmpwi(CR0, R5_ARG3, 2);
        __ blt(CR0, l_1);
        __ srdi(tmp1, R5_ARG3, 1);
        __ andi_(R5_ARG3, R5_ARG3, 1);

        __ addi(R3_ARG1, R3_ARG1, -4);
        __ addi(R4_ARG2, R4_ARG2, -4);
        __ mtctr(tmp1);

        __ bind(l_3);
        __ lwzu(tmp2, 4, R3_ARG1);
        __ stwu(tmp2, 4, R4_ARG2);
        __ bdnz(l_3);

        __ addi(R3_ARG1, R3_ARG1, 4);
        __ addi(R4_ARG2, R4_ARG2, 4);
      }

      // do single element copy
      __ bind(l_1);
      __ cmpwi(CR0, R5_ARG3, 0);
      __ beq(CR0, l_4);

      { // FasterArrayCopy
        __ mtctr(R5_ARG3);
        __ addi(R3_ARG1, R3_ARG1, -2);
        __ addi(R4_ARG2, R4_ARG2, -2);

        __ bind(l_5);
        __ lhzu(tmp2, 2, R3_ARG1);
        __ sthu(tmp2, 2, R4_ARG2);
        __ bdnz(l_5);
      }
    }

    __ bind(l_4);
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate stub for conjoint short copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_conjoint_short_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jshort_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jshort_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);

    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;

    address nooverlap_target = aligned ?
      STUB_ENTRY(arrayof_jshort_disjoint_arraycopy()) :
      STUB_ENTRY(jshort_disjoint_arraycopy());

    array_overlap_test(nooverlap_target, 1);

    Label l_1, l_2;
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      __ sldi(tmp1, R5_ARG3, 1);
      __ b(l_2);
      __ bind(l_1);
      __ sthx(tmp2, R4_ARG2, tmp1);
      __ bind(l_2);
      __ addic_(tmp1, tmp1, -2);
      __ lhzx(tmp2, R3_ARG1, tmp1);
      __ bge(CR0, l_1);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate core code for disjoint int copy (and oop copy on 32-bit).  If "aligned"
  // is true, the "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  void generate_disjoint_int_copy_core(bool aligned) {
    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R0;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    Label l_1, l_2, l_3, l_4, l_5, l_6, l_7;

    // for short arrays, just do single element copy
    __ li(tmp3, 0);
    __ cmpwi(CR0, R5_ARG3, 5);
    __ ble(CR0, l_2);

    if (!aligned) {
        // check if arrays have same alignment mod 8.
        __ xorr(tmp1, R3_ARG1, R4_ARG2);
        __ andi_(R0, tmp1, 7);
        // Not the same alignment, but ld and std just need to be 4 byte aligned.
        __ bne(CR0, l_4); // to OR from is 8 byte aligned -> copy 2 at a time

        // copy 1 element to align to and from on an 8 byte boundary
        __ andi_(R0, R3_ARG1, 7);
        __ beq(CR0, l_4);

        __ lwzx(tmp2, R3_ARG1, tmp3);
        __ addi(R5_ARG3, R5_ARG3, -1);
        __ stwx(tmp2, R4_ARG2, tmp3);
        { // FasterArrayCopy
          __ addi(R3_ARG1, R3_ARG1, 4);
          __ addi(R4_ARG2, R4_ARG2, 4);
        }
        __ bind(l_4);
      }

    { // FasterArrayCopy
      __ cmpwi(CR0, R5_ARG3, 7);
      __ ble(CR0, l_2); // copy 1 at a time if less than 8 elements remain

      __ srdi(tmp1, R5_ARG3, 3);
      __ andi_(R5_ARG3, R5_ARG3, 7);
      __ mtctr(tmp1);

    // Processor supports VSX, so use it to mass copy.

    // Prefetch the data into the L2 cache.
    __ dcbt(R3_ARG1, 0);

    // Set DSCR pre-fetch to deepest.
    if (VM_Version::has_mfdscr()) {
      __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
      __ mtdscr(tmp2);
    }
    __ li(tmp1, 16);

    // Backbranch target aligned to 32-byte. Not 16-byte align as
    // loop contains < 8 instructions that fit inside a single
    // i-cache sector.
    __ align(32);

    __ bind(l_7);
    // Use loop with VSX load/store instructions to
    // copy 8 elements a time.
    __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load src
    __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst
    __ lxvd2x(tmp_vsr2, tmp1, R3_ARG1);  // Load src + 16
    __ stxvd2x(tmp_vsr2, tmp1, R4_ARG2); // Store to dst + 16
    __ addi(R3_ARG1, R3_ARG1, 32);       // Update src+=32
    __ addi(R4_ARG2, R4_ARG2, 32);       // Update dsc+=32
    __ bdnz(l_7);                        // Dec CTR and loop if not zero.

    // Restore DSCR pre-fetch value.
    if (VM_Version::has_mfdscr()) {
      __ load_const_optimized(tmp2, VM_Version::_dscr_val);
      __ mtdscr(tmp2);
    }

   } // FasterArrayCopy

    // copy 1 element at a time
    __ bind(l_2);
    __ cmpwi(CR0, R5_ARG3, 0);
    __ beq(CR0, l_1);

    { // FasterArrayCopy
      __ mtctr(R5_ARG3);
      __ addi(R3_ARG1, R3_ARG1, -4);
      __ addi(R4_ARG2, R4_ARG2, -4);

      __ bind(l_3);
      __ lwzu(tmp2, 4, R3_ARG1);
      __ stwu(tmp2, 4, R4_ARG2);
      __ bdnz(l_3);
    }

    __ bind(l_1);
    return;
  }

  // Generate stub for disjoint int copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_disjoint_int_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jint_disjoint_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jint_disjoint_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      generate_disjoint_int_copy_core(aligned);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();
    return start;
  }

  // Generate core code for conjoint int copy (and oop copy on
  // 32-bit).  If "aligned" is true, the "from" and "to" addresses
  // are assumed to be heapword aligned.
  //
  // Arguments:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  void generate_conjoint_int_copy_core(bool aligned) {
    // Do reverse copy.  We assume the case of actual overlap is rare enough
    // that we don't have to optimize it.

    Label l_1, l_2, l_3, l_4, l_5, l_6, l_7;

    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R0;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    { // FasterArrayCopy
      __ cmpwi(CR0, R5_ARG3, 0);
      __ beq(CR0, l_6);

      __ sldi(R5_ARG3, R5_ARG3, 2);
      __ add(R3_ARG1, R3_ARG1, R5_ARG3);
      __ add(R4_ARG2, R4_ARG2, R5_ARG3);
      __ srdi(R5_ARG3, R5_ARG3, 2);

      if (!aligned) {
        // check if arrays have same alignment mod 8.
        __ xorr(tmp1, R3_ARG1, R4_ARG2);
        __ andi_(R0, tmp1, 7);
        // Not the same alignment, but ld and std just need to be 4 byte aligned.
        __ bne(CR0, l_7); // to OR from is 8 byte aligned -> copy 2 at a time

        // copy 1 element to align to and from on an 8 byte boundary
        __ andi_(R0, R3_ARG1, 7);
        __ beq(CR0, l_7);

        __ addi(R3_ARG1, R3_ARG1, -4);
        __ addi(R4_ARG2, R4_ARG2, -4);
        __ addi(R5_ARG3, R5_ARG3, -1);
        __ lwzx(tmp2, R3_ARG1);
        __ stwx(tmp2, R4_ARG2);
        __ bind(l_7);
      }

      __ cmpwi(CR0, R5_ARG3, 7);
      __ ble(CR0, l_5); // copy 1 at a time if less than 8 elements remain

      __ srdi(tmp1, R5_ARG3, 3);
      __ andi(R5_ARG3, R5_ARG3, 7);
      __ mtctr(tmp1);

      // Processor supports VSX, so use it to mass copy.
      // Prefetch the data into the L2 cache.
      __ dcbt(R3_ARG1, 0);

      // Set DSCR pre-fetch to deepest.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
        __ mtdscr(tmp2);
      }
      __ li(tmp1, 16);

      // Backbranch target aligned to 32-byte. Not 16-byte align as
      // loop contains < 8 instructions that fit inside a single
      // i-cache sector.
      __ align(32);

      __ bind(l_4);
      // Use loop with VSX load/store instructions to
      // copy 8 elements a time.
      __ addi(R3_ARG1, R3_ARG1, -32);      // Update src-=32
      __ addi(R4_ARG2, R4_ARG2, -32);      // Update dsc-=32
      __ lxvd2x(tmp_vsr2, tmp1, R3_ARG1);  // Load src+16
      __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load src
      __ stxvd2x(tmp_vsr2, tmp1, R4_ARG2); // Store to dst+16
      __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst
      __ bdnz(l_4);

      // Restore DSCR pre-fetch value.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val);
        __ mtdscr(tmp2);
      }

      __ cmpwi(CR0, R5_ARG3, 0);
      __ beq(CR0, l_6);

      __ bind(l_5);
      __ mtctr(R5_ARG3);
      __ bind(l_3);
      __ lwz(R0, -4, R3_ARG1);
      __ stw(R0, -4, R4_ARG2);
      __ addi(R3_ARG1, R3_ARG1, -4);
      __ addi(R4_ARG2, R4_ARG2, -4);
      __ bdnz(l_3);

      __ bind(l_6);
    }
  }

  // Generate stub for conjoint int copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_conjoint_int_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jint_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jint_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);
    address nooverlap_target = aligned ?
      STUB_ENTRY(arrayof_jint_disjoint_arraycopy()) :
      STUB_ENTRY(jint_disjoint_arraycopy());

    array_overlap_test(nooverlap_target, 2);
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      generate_conjoint_int_copy_core(aligned);
    }

    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate core code for disjoint long copy (and oop copy on
  // 64-bit).  If "aligned" is true, the "from" and "to" addresses
  // are assumed to be heapword aligned.
  //
  // Arguments:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  void generate_disjoint_long_copy_core(bool aligned) {
    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R0;

    Label l_1, l_2, l_3, l_4, l_5;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    { // FasterArrayCopy
      __ cmpwi(CR0, R5_ARG3, 3);
      __ ble(CR0, l_3); // copy 1 at a time if less than 4 elements remain

      __ srdi(tmp1, R5_ARG3, 2);
      __ andi_(R5_ARG3, R5_ARG3, 3);
      __ mtctr(tmp1);

      // Processor supports VSX, so use it to mass copy.

      // Prefetch the data into the L2 cache.
      __ dcbt(R3_ARG1, 0);

      // Set DSCR pre-fetch to deepest.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
        __ mtdscr(tmp2);
      }
      __ li(tmp1, 16);

      // Backbranch target aligned to 32-byte. Not 16-byte align as
      // loop contains < 8 instructions that fit inside a single
      // i-cache sector.
      __ align(32);

      __ bind(l_5);
      // Use loop with VSX load/store instructions to
      // copy 4 elements a time.
      __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load src
      __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst
      __ lxvd2x(tmp_vsr2, tmp1, R3_ARG1);  // Load src + 16
      __ stxvd2x(tmp_vsr2, tmp1, R4_ARG2); // Store to dst + 16
      __ addi(R3_ARG1, R3_ARG1, 32);       // Update src+=32
      __ addi(R4_ARG2, R4_ARG2, 32);       // Update dsc+=32
      __ bdnz(l_5);                        // Dec CTR and loop if not zero.

      // Restore DSCR pre-fetch value.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val);
        __ mtdscr(tmp2);
      }

   } // FasterArrayCopy

    // copy 1 element at a time
    __ bind(l_3);
    __ cmpwi(CR0, R5_ARG3, 0);
    __ beq(CR0, l_1);

    { // FasterArrayCopy
      __ mtctr(R5_ARG3);
      __ addi(R3_ARG1, R3_ARG1, -8);
      __ addi(R4_ARG2, R4_ARG2, -8);

      __ bind(l_2);
      __ ldu(R0, 8, R3_ARG1);
      __ stdu(R0, 8, R4_ARG2);
      __ bdnz(l_2);

    }
    __ bind(l_1);
  }

  // Generate stub for disjoint long copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_disjoint_long_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jlong_disjoint_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jlong_disjoint_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      generate_disjoint_long_copy_core(aligned);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();

  return start;
  }

  // Generate core code for conjoint long copy (and oop copy on
  // 64-bit).  If "aligned" is true, the "from" and "to" addresses
  // are assumed to be heapword aligned.
  //
  // Arguments:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  void generate_conjoint_long_copy_core(bool aligned) {
    Register tmp1 = R6_ARG4;
    Register tmp2 = R7_ARG5;
    Register tmp3 = R8_ARG6;
    Register tmp4 = R0;

    VectorSRegister tmp_vsr1  = VSR1;
    VectorSRegister tmp_vsr2  = VSR2;

    Label l_1, l_2, l_3, l_4, l_5;

    __ cmpwi(CR0, R5_ARG3, 0);
    __ beq(CR0, l_1);

    { // FasterArrayCopy
      __ sldi(R5_ARG3, R5_ARG3, 3);
      __ add(R3_ARG1, R3_ARG1, R5_ARG3);
      __ add(R4_ARG2, R4_ARG2, R5_ARG3);
      __ srdi(R5_ARG3, R5_ARG3, 3);

      __ cmpwi(CR0, R5_ARG3, 3);
      __ ble(CR0, l_5); // copy 1 at a time if less than 4 elements remain

      __ srdi(tmp1, R5_ARG3, 2);
      __ andi(R5_ARG3, R5_ARG3, 3);
      __ mtctr(tmp1);

      // Processor supports VSX, so use it to mass copy.
      // Prefetch the data into the L2 cache.
      __ dcbt(R3_ARG1, 0);

      // Set DSCR pre-fetch to deepest.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val | 7);
        __ mtdscr(tmp2);
      }
      __ li(tmp1, 16);

      // Backbranch target aligned to 32-byte. Not 16-byte align as
      // loop contains < 8 instructions that fit inside a single
      // i-cache sector.
      __ align(32);

      __ bind(l_4);
      // Use loop with VSX load/store instructions to
      // copy 4 elements a time.
      __ addi(R3_ARG1, R3_ARG1, -32);      // Update src-=32
      __ addi(R4_ARG2, R4_ARG2, -32);      // Update dsc-=32
      __ lxvd2x(tmp_vsr2, tmp1, R3_ARG1);  // Load src+16
      __ lxvd2x(tmp_vsr1, R3_ARG1);        // Load src
      __ stxvd2x(tmp_vsr2, tmp1, R4_ARG2); // Store to dst+16
      __ stxvd2x(tmp_vsr1, R4_ARG2);       // Store to dst
      __ bdnz(l_4);

      // Restore DSCR pre-fetch value.
      if (VM_Version::has_mfdscr()) {
        __ load_const_optimized(tmp2, VM_Version::_dscr_val);
        __ mtdscr(tmp2);
      }

      __ cmpwi(CR0, R5_ARG3, 0);
      __ beq(CR0, l_1);

      __ bind(l_5);
      __ mtctr(R5_ARG3);
      __ bind(l_3);
      __ ld(R0, -8, R3_ARG1);
      __ std(R0, -8, R4_ARG2);
      __ addi(R3_ARG1, R3_ARG1, -8);
      __ addi(R4_ARG2, R4_ARG2, -8);
      __ bdnz(l_3);

    }
    __ bind(l_1);
  }

  // Generate stub for conjoint long copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //
  address generate_conjoint_long_copy(StubGenStubId stub_id) {
    bool aligned;
    switch (stub_id) {
    case jlong_arraycopy_id:
      aligned = false;
      break;
    case arrayof_jlong_arraycopy_id:
      aligned = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);
    address nooverlap_target = aligned ?
      STUB_ENTRY(arrayof_jlong_disjoint_arraycopy()) :
      STUB_ENTRY(jlong_disjoint_arraycopy());

    array_overlap_test(nooverlap_target, 3);
    {
      // UnsafeMemoryAccess page error: continue at UnsafeMemoryAccess common_error_exit
      UnsafeMemoryAccessMark umam(this, !aligned, false);
      generate_conjoint_long_copy_core(aligned);
    }
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }

  // Generate stub for conjoint oop copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //      dest_uninitialized: G1 support
  //
  address generate_conjoint_oop_copy(StubGenStubId stub_id) {
    bool aligned;
    bool dest_uninitialized;
    switch (stub_id) {
    case oop_arraycopy_id:
      aligned = false;
      dest_uninitialized = false;
      break;
    case arrayof_oop_arraycopy_id:
      aligned = true;
      dest_uninitialized = false;
      break;
    case oop_arraycopy_uninit_id:
      aligned = false;
      dest_uninitialized = true;
      break;
    case arrayof_oop_arraycopy_uninit_id:
      aligned = true;
      dest_uninitialized = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);
    address nooverlap_target = aligned ?
      STUB_ENTRY(arrayof_oop_disjoint_arraycopy(dest_uninitialized)) :
      STUB_ENTRY(oop_disjoint_arraycopy(dest_uninitialized));

    array_overlap_test(nooverlap_target, UseCompressedOops ? 2 : 3);

    DecoratorSet decorators = IN_HEAP | IS_ARRAY;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }
    if (aligned) {
      decorators |= ARRAYCOPY_ALIGNED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, T_OBJECT, R3_ARG1, R4_ARG2, R5_ARG3, noreg, noreg);

    if (UseCompressedOops) {
      generate_conjoint_int_copy_core(aligned);
    } else {
#if INCLUDE_ZGC
      if (UseZGC) {
        ZBarrierSetAssembler *zbs = (ZBarrierSetAssembler*)bs;
        zbs->generate_conjoint_oop_copy(_masm, dest_uninitialized);
      } else
#endif
      generate_conjoint_long_copy_core(aligned);
    }

    bs->arraycopy_epilogue(_masm, decorators, T_OBJECT, R4_ARG2, R5_ARG3, noreg);
    __ li(R3_RET, 0); // return 0
    __ blr();
    return start;
  }

  // Generate stub for disjoint oop copy.  If "aligned" is true, the
  // "from" and "to" addresses are assumed to be heapword aligned.
  //
  // Arguments for generated stub:
  //      from:  R3_ARG1
  //      to:    R4_ARG2
  //      count: R5_ARG3 treated as signed
  //      dest_uninitialized: G1 support
  //
  address generate_disjoint_oop_copy(StubGenStubId stub_id) {
    bool aligned;
    bool dest_uninitialized;
    switch (stub_id) {
    case oop_disjoint_arraycopy_id:
      aligned = false;
      dest_uninitialized = false;
      break;
    case arrayof_oop_disjoint_arraycopy_id:
      aligned = true;
      dest_uninitialized = false;
      break;
    case oop_disjoint_arraycopy_uninit_id:
      aligned = false;
      dest_uninitialized = true;
      break;
    case arrayof_oop_disjoint_arraycopy_uninit_id:
      aligned = true;
      dest_uninitialized = true;
      break;
    default:
      ShouldNotReachHere();
    }

    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();
    assert_positive_int(R5_ARG3);

    DecoratorSet decorators = IN_HEAP | IS_ARRAY | ARRAYCOPY_DISJOINT;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }
    if (aligned) {
      decorators |= ARRAYCOPY_ALIGNED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, T_OBJECT, R3_ARG1, R4_ARG2, R5_ARG3, noreg, noreg);

    if (UseCompressedOops) {
      generate_disjoint_int_copy_core(aligned);
    } else {
#if INCLUDE_ZGC
      if (UseZGC) {
        ZBarrierSetAssembler *zbs = (ZBarrierSetAssembler*)bs;
        zbs->generate_disjoint_oop_copy(_masm, dest_uninitialized);
      } else
#endif
      generate_disjoint_long_copy_core(aligned);
    }

    bs->arraycopy_epilogue(_masm, decorators, T_OBJECT, R4_ARG2, R5_ARG3, noreg);
    __ li(R3_RET, 0); // return 0
    __ blr();

    return start;
  }


  // Helper for generating a dynamic type check.
  // Smashes only the given temp registers.
  void generate_type_check(Register sub_klass,
                           Register super_check_offset,
                           Register super_klass,
                           Register temp1,
                           Register temp2,
                           Label& L_success) {
    assert_different_registers(sub_klass, super_check_offset, super_klass);

    BLOCK_COMMENT("type_check:");

    Label L_miss;

    __ check_klass_subtype_fast_path(sub_klass, super_klass, temp1, temp2, &L_success, &L_miss, nullptr,
                                     super_check_offset);
    __ check_klass_subtype_slow_path(sub_klass, super_klass, temp1, temp2, &L_success);

    // Fall through on failure!
    __ bind(L_miss);
  }


  //  Generate stub for checked oop copy.
  //
  // Arguments for generated stub:
  //      from:  R3
  //      to:    R4
  //      count: R5 treated as signed
  //      ckoff: R6 (super_check_offset)
  //      ckval: R7 (super_klass)
  //      ret:   R3 zero for success; (-1^K) where K is partial transfer count
  //
  address generate_checkcast_copy(StubGenStubId stub_id) {
    const Register R3_from   = R3_ARG1;      // source array address
    const Register R4_to     = R4_ARG2;      // destination array address
    const Register R5_count  = R5_ARG3;      // elements count
    const Register R6_ckoff  = R6_ARG4;      // super_check_offset
    const Register R7_ckval  = R7_ARG5;      // super_klass

    const Register R8_offset = R8_ARG6;      // loop var, with stride wordSize
    const Register R9_remain = R9_ARG7;      // loop var, with stride -1
    const Register R10_oop   = R10_ARG8;     // actual oop copied
    const Register R11_klass = R11_scratch1; // oop._klass
    const Register R12_tmp   = R12_scratch2;
    const Register R2_tmp    = R2;

    bool dest_uninitialized;
    switch (stub_id) {
    case checkcast_arraycopy_id:
      dest_uninitialized = false;
      break;
    case checkcast_arraycopy_uninit_id:
      dest_uninitialized = true;
      break;
    default:
      ShouldNotReachHere();
    }
    //__ align(CodeEntryAlignment);
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    // Assert that int is 64 bit sign extended and arrays are not conjoint.
#ifdef ASSERT
    {
    assert_positive_int(R5_ARG3);
    const Register tmp1 = R11_scratch1, tmp2 = R12_scratch2;
    Label no_overlap;
    __ subf(tmp1, R3_ARG1, R4_ARG2); // distance in bytes
    __ sldi(tmp2, R5_ARG3, LogBytesPerHeapOop); // size in bytes
    __ cmpld(CR0, R3_ARG1, R4_ARG2); // Use unsigned comparison!
    __ cmpld(CR1, tmp1, tmp2);
    __ crnand(CR0, Assembler::less, CR1, Assembler::less);
    // Overlaps if Src before dst and distance smaller than size.
    // Branch to forward copy routine otherwise.
    __ blt(CR0, no_overlap);
    __ stop("overlap in checkcast_copy");
    __ bind(no_overlap);
    }
#endif

    DecoratorSet decorators = IN_HEAP | IS_ARRAY | ARRAYCOPY_CHECKCAST;
    if (dest_uninitialized) {
      decorators |= IS_DEST_UNINITIALIZED;
    }

    BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
    bs->arraycopy_prologue(_masm, decorators, T_OBJECT, R3_from, R4_to, R5_count, /* preserve: */ R6_ckoff, R7_ckval);

    //inc_counter_np(SharedRuntime::_checkcast_array_copy_ctr, R12_tmp, R3_RET);

    Label load_element, store_element, store_null, success, do_epilogue;
    __ or_(R9_remain, R5_count, R5_count); // Initialize loop index, and test it.
    __ li(R8_offset, 0);                   // Offset from start of arrays.
    __ bne(CR0, load_element);

    // Empty array: Nothing to do.
    __ li(R3_RET, 0);           // Return 0 on (trivial) success.
    __ blr();

    // ======== begin loop ========
    // (Entry is load_element.)
    __ align(OptoLoopAlignment);
    __ bind(store_element);
    if (UseCompressedOops) {
      __ encode_heap_oop_not_null(R10_oop);
      __ bind(store_null);
      __ stw(R10_oop, R8_offset, R4_to);
    } else {
      __ bind(store_null);
#if INCLUDE_ZGC
      if (UseZGC) {
        __ store_heap_oop(R10_oop, R8_offset, R4_to, R11_scratch1, R12_tmp, noreg,
                          MacroAssembler::PRESERVATION_FRAME_LR_GP_REGS,
                          dest_uninitialized ? IS_DEST_UNINITIALIZED : 0);
      } else
#endif
      __ std(R10_oop, R8_offset, R4_to);
    }

    __ addi(R8_offset, R8_offset, heapOopSize);   // Step to next offset.
    __ addic_(R9_remain, R9_remain, -1);          // Decrement the count.
    __ beq(CR0, success);

    // ======== loop entry is here ========
    __ bind(load_element);
#if INCLUDE_ZGC
    if (UseZGC) {
      __ load_heap_oop(R10_oop, R8_offset, R3_from,
                       R11_scratch1, R12_tmp,
                       MacroAssembler::PRESERVATION_FRAME_LR_GP_REGS,
                       0, &store_null);
    } else
#endif
    __ load_heap_oop(R10_oop, R8_offset, R3_from,
                     R11_scratch1, R12_tmp,
                     MacroAssembler::PRESERVATION_FRAME_LR_GP_REGS,
                     AS_RAW, &store_null);

    __ load_klass(R11_klass, R10_oop); // Query the object klass.

    generate_type_check(R11_klass, R6_ckoff, R7_ckval, R12_tmp, R2_tmp,
                        // Branch to this on success:
                        store_element);
    // ======== end loop ========

    // It was a real error; we must depend on the caller to finish the job.
    // Register R9_remain has number of *remaining* oops, R5_count number of *total* oops.
    // Emit GC store barriers for the oops we have copied (R5_count minus R9_remain),
    // and report their number to the caller.
    __ subf_(R5_count, R9_remain, R5_count);
    __ nand(R3_RET, R5_count, R5_count);   // report (-1^K) to caller
    __ bne(CR0, do_epilogue);
    __ blr();

    __ bind(success);
    __ li(R3_RET, 0);

    __ bind(do_epilogue);
    bs->arraycopy_epilogue(_masm, decorators, T_OBJECT, R4_to, R5_count, /* preserve */ R3_RET);

    __ blr();
    return start;
  }


  //  Generate 'unsafe' array copy stub.
  //  Though just as safe as the other stubs, it takes an unscaled
  //  size_t argument instead of an element count.
  //
  // Arguments for generated stub:
  //      from:  R3
  //      to:    R4
  //      count: R5 byte count, treated as ssize_t, can be zero
  //
  // Examines the alignment of the operands and dispatches
  // to a long, int, short, or byte copy loop.
  //
  address generate_unsafe_copy(address byte_copy_entry,
                               address short_copy_entry,
                               address int_copy_entry,
                               address long_copy_entry) {

    const Register R3_from   = R3_ARG1;      // source array address
    const Register R4_to     = R4_ARG2;      // destination array address
    const Register R5_count  = R5_ARG3;      // elements count (as long on PPC64)

    const Register R6_bits   = R6_ARG4;      // test copy of low bits
    const Register R7_tmp    = R7_ARG5;

    //__ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::unsafe_arraycopy_id;
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    // Bump this on entry, not on exit:
    //inc_counter_np(SharedRuntime::_unsafe_array_copy_ctr, R6_bits, R7_tmp);

    Label short_copy, int_copy, long_copy;

    __ orr(R6_bits, R3_from, R4_to);
    __ orr(R6_bits, R6_bits, R5_count);
    __ andi_(R0, R6_bits, (BytesPerLong-1));
    __ beq(CR0, long_copy);

    __ andi_(R0, R6_bits, (BytesPerInt-1));
    __ beq(CR0, int_copy);

    __ andi_(R0, R6_bits, (BytesPerShort-1));
    __ beq(CR0, short_copy);

    // byte_copy:
    __ b(byte_copy_entry);

    __ bind(short_copy);
    __ srwi(R5_count, R5_count, LogBytesPerShort);
    __ b(short_copy_entry);

    __ bind(int_copy);
    __ srwi(R5_count, R5_count, LogBytesPerInt);
    __ b(int_copy_entry);

    __ bind(long_copy);
    __ srwi(R5_count, R5_count, LogBytesPerLong);
    __ b(long_copy_entry);

    return start;
  }


  // Perform range checks on the proposed arraycopy.
  // Kills the two temps, but nothing else.
  // Also, clean the sign bits of src_pos and dst_pos.
  void arraycopy_range_checks(Register src,     // source array oop
                              Register src_pos, // source position
                              Register dst,     // destination array oop
                              Register dst_pos, // destination position
                              Register length,  // length of copy
                              Register temp1, Register temp2,
                              Label& L_failed) {
    BLOCK_COMMENT("arraycopy_range_checks:");

    const Register array_length = temp1;  // scratch
    const Register end_pos      = temp2;  // scratch

    //  if (src_pos + length > arrayOop(src)->length() ) FAIL;
    __ lwa(array_length, arrayOopDesc::length_offset_in_bytes(), src);
    __ add(end_pos, src_pos, length);  // src_pos + length
    __ cmpd(CR0, end_pos, array_length);
    __ bgt(CR0, L_failed);

    //  if (dst_pos + length > arrayOop(dst)->length() ) FAIL;
    __ lwa(array_length, arrayOopDesc::length_offset_in_bytes(), dst);
    __ add(end_pos, dst_pos, length);  // src_pos + length
    __ cmpd(CR0, end_pos, array_length);
    __ bgt(CR0, L_failed);

    BLOCK_COMMENT("arraycopy_range_checks done");
  }


  // Helper for generate_unsafe_setmemory
  //
  // Atomically fill an array of memory using 1-, 2-, 4-, or 8-byte chunks and return.
  static void do_setmemory_atomic_loop(int elem_size, Register dest, Register size, Register byteVal,
                                       MacroAssembler *_masm) {

    Label L_Loop, L_Tail; // 2x unrolled loop

    // Propagate byte to required width
    if (elem_size > 1) __ rldimi(byteVal, byteVal,  8, 64 - 2 *  8);
    if (elem_size > 2) __ rldimi(byteVal, byteVal, 16, 64 - 2 * 16);
    if (elem_size > 4) __ rldimi(byteVal, byteVal, 32, 64 - 2 * 32);

    __ srwi_(R0, size, exact_log2(2 * elem_size)); // size is a 32 bit value
    __ beq(CR0, L_Tail);
    __ mtctr(R0);

    __ align(32); // loop alignment
    __ bind(L_Loop);
    __ store_sized_value(byteVal, 0, dest, elem_size);
    __ store_sized_value(byteVal, elem_size, dest, elem_size);
    __ addi(dest, dest, 2 * elem_size);
    __ bdnz(L_Loop);

    __ bind(L_Tail);
    __ andi_(R0, size, elem_size);
    __ bclr(Assembler::bcondCRbiIs1, Assembler::bi0(CR0, Assembler::equal), Assembler::bhintbhBCLRisReturn);
    __ store_sized_value(byteVal, 0, dest, elem_size);
    __ blr();
  }

  //
  //  Generate 'unsafe' set memory stub
  //  Though just as safe as the other stubs, it takes an unscaled
  //  size_t (# bytes) argument instead of an element count.
  //
  //  Input:
  //    R3_ARG1   - destination array address
  //    R4_ARG2   - byte count (size_t)
  //    R5_ARG3   - byte value
  //
  address generate_unsafe_setmemory(address unsafe_byte_fill) {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, StubGenStubId::unsafe_setmemory_id);
    address start = __ function_entry();

    // bump this on entry, not on exit:
    // inc_counter_np(SharedRuntime::_unsafe_set_memory_ctr);

    {
      Label L_fill8Bytes, L_fill4Bytes, L_fillBytes;

      const Register dest = R3_ARG1;
      const Register size = R4_ARG2;
      const Register byteVal = R5_ARG3;
      const Register rScratch1 = R6;

      // fill_to_memory_atomic(unsigned char*, unsigned long, unsigned char)

      // Check for pointer & size alignment
      __ orr(rScratch1, dest, size);

      __ andi_(R0, rScratch1, 7);
      __ beq(CR0, L_fill8Bytes);

      __ andi_(R0, rScratch1, 3);
      __ beq(CR0, L_fill4Bytes);

      __ andi_(R0, rScratch1, 1);
      __ bne(CR0, L_fillBytes);

      // Mark remaining code as such which performs Unsafe accesses.
      UnsafeMemoryAccessMark umam(this, true, false);

      // At this point, we know the lower bit of size is zero and a
      // multiple of 2
      do_setmemory_atomic_loop(2, dest, size, byteVal, _masm);

      __ align(32);
      __ bind(L_fill8Bytes);
      // At this point, we know the lower 3 bits of size are zero and a
      // multiple of 8
      do_setmemory_atomic_loop(8, dest, size, byteVal, _masm);

      __ align(32);
      __ bind(L_fill4Bytes);
      // At this point, we know the lower 2 bits of size are zero and a
      // multiple of 4
      do_setmemory_atomic_loop(4, dest, size, byteVal, _masm);

      __ align(32);
      __ bind(L_fillBytes);
      do_setmemory_atomic_loop(1, dest, size, byteVal, _masm);
    }

    return start;
  }


  //
  //  Generate generic array copy stubs
  //
  //  Input:
  //    R3    -  src oop
  //    R4    -  src_pos
  //    R5    -  dst oop
  //    R6    -  dst_pos
  //    R7    -  element count
  //
  //  Output:
  //    R3 ==  0  -  success
  //    R3 == -1  -  need to call System.arraycopy
  //
  address generate_generic_copy(address entry_jbyte_arraycopy,
                                address entry_jshort_arraycopy,
                                address entry_jint_arraycopy,
                                address entry_oop_arraycopy,
                                address entry_disjoint_oop_arraycopy,
                                address entry_jlong_arraycopy,
                                address entry_checkcast_arraycopy) {
    Label L_failed, L_objArray;

    // Input registers
    const Register src       = R3_ARG1;  // source array oop
    const Register src_pos   = R4_ARG2;  // source position
    const Register dst       = R5_ARG3;  // destination array oop
    const Register dst_pos   = R6_ARG4;  // destination position
    const Register length    = R7_ARG5;  // elements count

    // registers used as temp
    const Register src_klass = R8_ARG6;  // source array klass
    const Register dst_klass = R9_ARG7;  // destination array klass
    const Register lh        = R10_ARG8; // layout handler
    const Register temp      = R2;

    //__ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::generic_arraycopy_id;
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    // Bump this on entry, not on exit:
    //inc_counter_np(SharedRuntime::_generic_array_copy_ctr, lh, temp);

    // In principle, the int arguments could be dirty.

    //-----------------------------------------------------------------------
    // Assembler stubs will be used for this call to arraycopy
    // if the following conditions are met:
    //
    // (1) src and dst must not be null.
    // (2) src_pos must not be negative.
    // (3) dst_pos must not be negative.
    // (4) length  must not be negative.
    // (5) src klass and dst klass should be the same and not null.
    // (6) src and dst should be arrays.
    // (7) src_pos + length must not exceed length of src.
    // (8) dst_pos + length must not exceed length of dst.
    BLOCK_COMMENT("arraycopy initial argument checks");

    __ cmpdi(CR1, src, 0);      // if (src == nullptr) return -1;
    __ extsw_(src_pos, src_pos); // if (src_pos < 0) return -1;
    __ cmpdi(CR5, dst, 0);      // if (dst == nullptr) return -1;
    __ cror(CR1, Assembler::equal, CR0, Assembler::less);
    __ extsw_(dst_pos, dst_pos); // if (src_pos < 0) return -1;
    __ cror(CR5, Assembler::equal, CR0, Assembler::less);
    __ extsw_(length, length);   // if (length < 0) return -1;
    __ cror(CR1, Assembler::equal, CR5, Assembler::equal);
    __ cror(CR1, Assembler::equal, CR0, Assembler::less);
    __ beq(CR1, L_failed);

    BLOCK_COMMENT("arraycopy argument klass checks");
    __ load_klass(src_klass, src);
    __ load_klass(dst_klass, dst);

    // Load layout helper
    //
    //  |array_tag|     | header_size | element_type |     |log2_element_size|
    // 32        30    24            16              8     2                 0
    //
    //   array_tag: typeArray = 0x3, objArray = 0x2, non-array = 0x0
    //

    int lh_offset = in_bytes(Klass::layout_helper_offset());

    // Load 32-bits signed value. Use br() instruction with it to check icc.
    __ lwz(lh, lh_offset, src_klass);

    // Handle objArrays completely differently...
    jint objArray_lh = Klass::array_layout_helper(T_OBJECT);
    __ load_const_optimized(temp, objArray_lh, R0);
    __ cmpw(CR0, lh, temp);
    __ beq(CR0, L_objArray);

    __ cmpd(CR5, src_klass, dst_klass);          // if (src->klass() != dst->klass()) return -1;
    __ cmpwi(CR6, lh, Klass::_lh_neutral_value); // if (!src->is_Array()) return -1;

    __ crnand(CR5, Assembler::equal, CR6, Assembler::less);
    __ beq(CR5, L_failed);

    // At this point, it is known to be a typeArray (array_tag 0x3).
#ifdef ASSERT
    { Label L;
      jint lh_prim_tag_in_place = (Klass::_lh_array_tag_type_value << Klass::_lh_array_tag_shift);
      __ load_const_optimized(temp, lh_prim_tag_in_place, R0);
      __ cmpw(CR0, lh, temp);
      __ bge(CR0, L);
      __ stop("must be a primitive array");
      __ bind(L);
    }
#endif

    arraycopy_range_checks(src, src_pos, dst, dst_pos, length,
                           temp, dst_klass, L_failed);

    // TypeArrayKlass
    //
    // src_addr = (src + array_header_in_bytes()) + (src_pos << log2elemsize);
    // dst_addr = (dst + array_header_in_bytes()) + (dst_pos << log2elemsize);
    //

    const Register offset = dst_klass;    // array offset
    const Register elsize = src_klass;    // log2 element size

    __ rldicl(offset, lh, 64 - Klass::_lh_header_size_shift, 64 - exact_log2(Klass::_lh_header_size_mask + 1));
    __ andi(elsize, lh, Klass::_lh_log2_element_size_mask);
    __ add(src, offset, src);       // src array offset
    __ add(dst, offset, dst);       // dst array offset

    // Next registers should be set before the jump to corresponding stub.
    const Register from     = R3_ARG1;  // source array address
    const Register to       = R4_ARG2;  // destination array address
    const Register count    = R5_ARG3;  // elements count

    // 'from', 'to', 'count' registers should be set in this order
    // since they are the same as 'src', 'src_pos', 'dst'.

    BLOCK_COMMENT("scale indexes to element size");
    __ sld(src_pos, src_pos, elsize);
    __ sld(dst_pos, dst_pos, elsize);
    __ add(from, src_pos, src);  // src_addr
    __ add(to, dst_pos, dst);    // dst_addr
    __ mr(count, length);        // length

    BLOCK_COMMENT("choose copy loop based on element size");
    // Using conditional branches with range 32kB.
    const int bo = Assembler::bcondCRbiIs1, bi = Assembler::bi0(CR0, Assembler::equal);
    __ cmpwi(CR0, elsize, 0);
    __ bc(bo, bi, entry_jbyte_arraycopy);
    __ cmpwi(CR0, elsize, LogBytesPerShort);
    __ bc(bo, bi, entry_jshort_arraycopy);
    __ cmpwi(CR0, elsize, LogBytesPerInt);
    __ bc(bo, bi, entry_jint_arraycopy);
#ifdef ASSERT
    { Label L;
      __ cmpwi(CR0, elsize, LogBytesPerLong);
      __ beq(CR0, L);
      __ stop("must be long copy, but elsize is wrong");
      __ bind(L);
    }
#endif
    __ b(entry_jlong_arraycopy);

    // ObjArrayKlass
  __ bind(L_objArray);
    // live at this point:  src_klass, dst_klass, src[_pos], dst[_pos], length

    Label L_disjoint_plain_copy, L_checkcast_copy;
    //  test array classes for subtyping
    __ cmpd(CR0, src_klass, dst_klass);         // usual case is exact equality
    __ bne(CR0, L_checkcast_copy);

    // Identically typed arrays can be copied without element-wise checks.
    arraycopy_range_checks(src, src_pos, dst, dst_pos, length,
                           temp, lh, L_failed);

    __ addi(src, src, arrayOopDesc::base_offset_in_bytes(T_OBJECT)); //src offset
    __ addi(dst, dst, arrayOopDesc::base_offset_in_bytes(T_OBJECT)); //dst offset
    __ sldi(src_pos, src_pos, LogBytesPerHeapOop);
    __ sldi(dst_pos, dst_pos, LogBytesPerHeapOop);
    __ add(from, src_pos, src);  // src_addr
    __ add(to, dst_pos, dst);    // dst_addr
    __ mr(count, length);        // length
    __ b(entry_oop_arraycopy);

  __ bind(L_checkcast_copy);
    // live at this point:  src_klass, dst_klass
    {
      // Before looking at dst.length, make sure dst is also an objArray.
      __ lwz(temp, lh_offset, dst_klass);
      __ cmpw(CR0, lh, temp);
      __ bne(CR0, L_failed);

      // It is safe to examine both src.length and dst.length.
      arraycopy_range_checks(src, src_pos, dst, dst_pos, length,
                             temp, lh, L_failed);

      // Marshal the base address arguments now, freeing registers.
      __ addi(src, src, arrayOopDesc::base_offset_in_bytes(T_OBJECT)); //src offset
      __ addi(dst, dst, arrayOopDesc::base_offset_in_bytes(T_OBJECT)); //dst offset
      __ sldi(src_pos, src_pos, LogBytesPerHeapOop);
      __ sldi(dst_pos, dst_pos, LogBytesPerHeapOop);
      __ add(from, src_pos, src);  // src_addr
      __ add(to, dst_pos, dst);    // dst_addr
      __ mr(count, length);        // length

      Register sco_temp = R6_ARG4;             // This register is free now.
      assert_different_registers(from, to, count, sco_temp,
                                 dst_klass, src_klass);

      // Generate the type check.
      int sco_offset = in_bytes(Klass::super_check_offset_offset());
      __ lwz(sco_temp, sco_offset, dst_klass);
      generate_type_check(src_klass, sco_temp, dst_klass,
                          temp, /* temp */ R10_ARG8, L_disjoint_plain_copy);

      // Fetch destination element klass from the ObjArrayKlass header.
      int ek_offset = in_bytes(ObjArrayKlass::element_klass_offset());

      // The checkcast_copy loop needs two extra arguments:
      __ ld(R7_ARG5, ek_offset, dst_klass);   // dest elem klass
      __ lwz(R6_ARG4, sco_offset, R7_ARG5);   // sco of elem klass
      __ b(entry_checkcast_arraycopy);
    }

    __ bind(L_disjoint_plain_copy);
    __ b(entry_disjoint_oop_arraycopy);

  __ bind(L_failed);
    __ li(R3_RET, -1); // return -1
    __ blr();
    return start;
  }

  // Arguments for generated stub:
  //   R3_ARG1   - source byte array address
  //   R4_ARG2   - destination byte array address
  //   R5_ARG3   - round key array
  address generate_aescrypt_encryptBlock() {
    assert(UseAES, "need AES instructions and misaligned SSE support");
    StubGenStubId stub_id = StubGenStubId::aescrypt_encryptBlock_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    Label L_doLast, L_error;

    Register from           = R3_ARG1;  // source array address
    Register to             = R4_ARG2;  // destination array address
    Register key            = R5_ARG3;  // round key array

    Register keylen         = R8;
    Register temp           = R9;
    Register keypos         = R10;
    Register fifteen        = R12;

    VectorRegister vRet     = VR0;

    VectorRegister vKey1    = VR1;
    VectorRegister vKey2    = VR2;
    VectorRegister vKey3    = VR3;
    VectorRegister vKey4    = VR4;

    VectorRegister fromPerm = VR5;
    VectorRegister keyPerm  = VR6;
    VectorRegister toPerm   = VR7;
    VectorRegister fSplt    = VR8;

    VectorRegister vTmp1    = VR9;
    VectorRegister vTmp2    = VR10;
    VectorRegister vTmp3    = VR11;
    VectorRegister vTmp4    = VR12;

    __ li              (fifteen, 15);

    // load unaligned from[0-15] to vRet
    __ lvx             (vRet, from);
    __ lvx             (vTmp1, fifteen, from);
    __ lvsl            (fromPerm, from);
#ifdef VM_LITTLE_ENDIAN
    __ vspltisb        (fSplt, 0x0f);
    __ vxor            (fromPerm, fromPerm, fSplt);
#endif
    __ vperm           (vRet, vRet, vTmp1, fromPerm);

    // load keylen (44 or 52 or 60)
    __ lwz             (keylen, arrayOopDesc::length_offset_in_bytes() - arrayOopDesc::base_offset_in_bytes(T_INT), key);

    // to load keys
    __ load_perm       (keyPerm, key);
#ifdef VM_LITTLE_ENDIAN
    __ vspltisb        (vTmp2, -16);
    __ vrld            (keyPerm, keyPerm, vTmp2);
    __ vrld            (keyPerm, keyPerm, vTmp2);
    __ vsldoi          (keyPerm, keyPerm, keyPerm, 8);
#endif

    // load the 1st round key to vTmp1
    __ lvx             (vTmp1, key);
    __ li              (keypos, 16);
    __ lvx             (vKey1, keypos, key);
    __ vec_perm        (vTmp1, vKey1, keyPerm);

    // 1st round
    __ vxor            (vRet, vRet, vTmp1);

    // load the 2nd round key to vKey1
    __ li              (keypos, 32);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vKey2, keyPerm);

    // load the 3rd round key to vKey2
    __ li              (keypos, 48);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, keyPerm);

    // load the 4th round key to vKey3
    __ li              (keypos, 64);
    __ lvx             (vKey4, keypos, key);
    __ vec_perm        (vKey3, vKey4, keyPerm);

    // load the 5th round key to vKey4
    __ li              (keypos, 80);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey4, vTmp1, keyPerm);

    // 2nd - 5th rounds
    __ vcipher         (vRet, vRet, vKey1);
    __ vcipher         (vRet, vRet, vKey2);
    __ vcipher         (vRet, vRet, vKey3);
    __ vcipher         (vRet, vRet, vKey4);

    // load the 6th round key to vKey1
    __ li              (keypos, 96);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vTmp1, vKey2, keyPerm);

    // load the 7th round key to vKey2
    __ li              (keypos, 112);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, keyPerm);

    // load the 8th round key to vKey3
    __ li              (keypos, 128);
    __ lvx             (vKey4, keypos, key);
    __ vec_perm        (vKey3, vKey4, keyPerm);

    // load the 9th round key to vKey4
    __ li              (keypos, 144);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey4, vTmp1, keyPerm);

    // 6th - 9th rounds
    __ vcipher         (vRet, vRet, vKey1);
    __ vcipher         (vRet, vRet, vKey2);
    __ vcipher         (vRet, vRet, vKey3);
    __ vcipher         (vRet, vRet, vKey4);

    // load the 10th round key to vKey1
    __ li              (keypos, 160);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vTmp1, vKey2, keyPerm);

    // load the 11th round key to vKey2
    __ li              (keypos, 176);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey2, vTmp1, keyPerm);

    // if all round keys are loaded, skip next 4 rounds
    __ cmpwi           (CR0, keylen, 44);
    __ beq             (CR0, L_doLast);

    // 10th - 11th rounds
    __ vcipher         (vRet, vRet, vKey1);
    __ vcipher         (vRet, vRet, vKey2);

    // load the 12th round key to vKey1
    __ li              (keypos, 192);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vTmp1, vKey2, keyPerm);

    // load the 13th round key to vKey2
    __ li              (keypos, 208);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey2, vTmp1, keyPerm);

    // if all round keys are loaded, skip next 2 rounds
    __ cmpwi           (CR0, keylen, 52);
    __ beq             (CR0, L_doLast);

#ifdef ASSERT
    __ cmpwi           (CR0, keylen, 60);
    __ bne             (CR0, L_error);
#endif

    // 12th - 13th rounds
    __ vcipher         (vRet, vRet, vKey1);
    __ vcipher         (vRet, vRet, vKey2);

    // load the 14th round key to vKey1
    __ li              (keypos, 224);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vTmp1, vKey2, keyPerm);

    // load the 15th round key to vKey2
    __ li              (keypos, 240);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey2, vTmp1, keyPerm);

    __ bind(L_doLast);

    // last two rounds
    __ vcipher         (vRet, vRet, vKey1);
    __ vcipherlast     (vRet, vRet, vKey2);

#ifdef VM_LITTLE_ENDIAN
    // toPerm = 0x0F0E0D0C0B0A09080706050403020100
    __ lvsl            (toPerm, keypos); // keypos is a multiple of 16
    __ vxor            (toPerm, toPerm, fSplt);

    // Swap Bytes
    __ vperm           (vRet, vRet, vRet, toPerm);
#endif

    // store result (unaligned)
    // Note: We can't use a read-modify-write sequence which touches additional Bytes.
    Register lo = temp, hi = fifteen; // Reuse
    __ vsldoi          (vTmp1, vRet, vRet, 8);
    __ mfvrd           (hi, vRet);
    __ mfvrd           (lo, vTmp1);
    __ std             (hi, 0 LITTLE_ENDIAN_ONLY(+ 8), to);
    __ std             (lo, 0 BIG_ENDIAN_ONLY(+ 8), to);

    __ blr();

#ifdef ASSERT
    __ bind(L_error);
    __ stop("aescrypt_encryptBlock: invalid key length");
#endif
     return start;
  }

  // Arguments for generated stub:
  //   R3_ARG1   - source byte array address
  //   R4_ARG2   - destination byte array address
  //   R5_ARG3   - K (key) in little endian int array
  address generate_aescrypt_decryptBlock() {
    assert(UseAES, "need AES instructions and misaligned SSE support");
    StubGenStubId stub_id = StubGenStubId::aescrypt_decryptBlock_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    Label L_doLast, L_do44, L_do52, L_error;

    Register from           = R3_ARG1;  // source array address
    Register to             = R4_ARG2;  // destination array address
    Register key            = R5_ARG3;  // round key array

    Register keylen         = R8;
    Register temp           = R9;
    Register keypos         = R10;
    Register fifteen        = R12;

    VectorRegister vRet     = VR0;

    VectorRegister vKey1    = VR1;
    VectorRegister vKey2    = VR2;
    VectorRegister vKey3    = VR3;
    VectorRegister vKey4    = VR4;
    VectorRegister vKey5    = VR5;

    VectorRegister fromPerm = VR6;
    VectorRegister keyPerm  = VR7;
    VectorRegister toPerm   = VR8;
    VectorRegister fSplt    = VR9;

    VectorRegister vTmp1    = VR10;
    VectorRegister vTmp2    = VR11;
    VectorRegister vTmp3    = VR12;
    VectorRegister vTmp4    = VR13;

    __ li              (fifteen, 15);

    // load unaligned from[0-15] to vRet
    __ lvx             (vRet, from);
    __ lvx             (vTmp1, fifteen, from);
    __ lvsl            (fromPerm, from);
#ifdef VM_LITTLE_ENDIAN
    __ vspltisb        (fSplt, 0x0f);
    __ vxor            (fromPerm, fromPerm, fSplt);
#endif
    __ vperm           (vRet, vRet, vTmp1, fromPerm); // align [and byte swap in LE]

    // load keylen (44 or 52 or 60)
    __ lwz             (keylen, arrayOopDesc::length_offset_in_bytes() - arrayOopDesc::base_offset_in_bytes(T_INT), key);

    // to load keys
    __ load_perm       (keyPerm, key);
#ifdef VM_LITTLE_ENDIAN
    __ vxor            (vTmp2, vTmp2, vTmp2);
    __ vspltisb        (vTmp2, -16);
    __ vrld            (keyPerm, keyPerm, vTmp2);
    __ vrld            (keyPerm, keyPerm, vTmp2);
    __ vsldoi          (keyPerm, keyPerm, keyPerm, 8);
#endif

    __ cmpwi           (CR0, keylen, 44);
    __ beq             (CR0, L_do44);

    __ cmpwi           (CR0, keylen, 52);
    __ beq             (CR0, L_do52);

#ifdef ASSERT
    __ cmpwi           (CR0, keylen, 60);
    __ bne             (CR0, L_error);
#endif

    // load the 15th round key to vKey1
    __ li              (keypos, 240);
    __ lvx             (vKey1, keypos, key);
    __ li              (keypos, 224);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vKey2, vKey1, keyPerm);

    // load the 14th round key to vKey2
    __ li              (keypos, 208);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, vKey2, keyPerm);

    // load the 13th round key to vKey3
    __ li              (keypos, 192);
    __ lvx             (vKey4, keypos, key);
    __ vec_perm        (vKey3, vKey4, vKey3, keyPerm);

    // load the 12th round key to vKey4
    __ li              (keypos, 176);
    __ lvx             (vKey5, keypos, key);
    __ vec_perm        (vKey4, vKey5, vKey4, keyPerm);

    // load the 11th round key to vKey5
    __ li              (keypos, 160);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey5, vTmp1, vKey5, keyPerm);

    // 1st - 5th rounds
    __ vxor            (vRet, vRet, vKey1);
    __ vncipher        (vRet, vRet, vKey2);
    __ vncipher        (vRet, vRet, vKey3);
    __ vncipher        (vRet, vRet, vKey4);
    __ vncipher        (vRet, vRet, vKey5);

    __ b               (L_doLast);

    __ align(32);
    __ bind            (L_do52);

    // load the 13th round key to vKey1
    __ li              (keypos, 208);
    __ lvx             (vKey1, keypos, key);
    __ li              (keypos, 192);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vKey2, vKey1, keyPerm);

    // load the 12th round key to vKey2
    __ li              (keypos, 176);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, vKey2, keyPerm);

    // load the 11th round key to vKey3
    __ li              (keypos, 160);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey3, vTmp1, vKey3, keyPerm);

    // 1st - 3rd rounds
    __ vxor            (vRet, vRet, vKey1);
    __ vncipher        (vRet, vRet, vKey2);
    __ vncipher        (vRet, vRet, vKey3);

    __ b               (L_doLast);

    __ align(32);
    __ bind            (L_do44);

    // load the 11th round key to vKey1
    __ li              (keypos, 176);
    __ lvx             (vKey1, keypos, key);
    __ li              (keypos, 160);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey1, vTmp1, vKey1, keyPerm);

    // 1st round
    __ vxor            (vRet, vRet, vKey1);

    __ bind            (L_doLast);

    // load the 10th round key to vKey1
    __ li              (keypos, 144);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vKey2, vTmp1, keyPerm);

    // load the 9th round key to vKey2
    __ li              (keypos, 128);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, vKey2, keyPerm);

    // load the 8th round key to vKey3
    __ li              (keypos, 112);
    __ lvx             (vKey4, keypos, key);
    __ vec_perm        (vKey3, vKey4, vKey3, keyPerm);

    // load the 7th round key to vKey4
    __ li              (keypos, 96);
    __ lvx             (vKey5, keypos, key);
    __ vec_perm        (vKey4, vKey5, vKey4, keyPerm);

    // load the 6th round key to vKey5
    __ li              (keypos, 80);
    __ lvx             (vTmp1, keypos, key);
    __ vec_perm        (vKey5, vTmp1, vKey5, keyPerm);

    // last 10th - 6th rounds
    __ vncipher        (vRet, vRet, vKey1);
    __ vncipher        (vRet, vRet, vKey2);
    __ vncipher        (vRet, vRet, vKey3);
    __ vncipher        (vRet, vRet, vKey4);
    __ vncipher        (vRet, vRet, vKey5);

    // load the 5th round key to vKey1
    __ li              (keypos, 64);
    __ lvx             (vKey2, keypos, key);
    __ vec_perm        (vKey1, vKey2, vTmp1, keyPerm);

    // load the 4th round key to vKey2
    __ li              (keypos, 48);
    __ lvx             (vKey3, keypos, key);
    __ vec_perm        (vKey2, vKey3, vKey2, keyPerm);

    // load the 3rd round key to vKey3
    __ li              (keypos, 32);
    __ lvx             (vKey4, keypos, key);
    __ vec_perm        (vKey3, vKey4, vKey3, keyPerm);

    // load the 2nd round key to vKey4
    __ li              (keypos, 16);
    __ lvx             (vKey5, keypos, key);
    __ vec_perm        (vKey4, vKey5, vKey4, keyPerm);

    // load the 1st round key to vKey5
    __ lvx             (vTmp1, key);
    __ vec_perm        (vKey5, vTmp1, vKey5, keyPerm);

    // last 5th - 1th rounds
    __ vncipher        (vRet, vRet, vKey1);
    __ vncipher        (vRet, vRet, vKey2);
    __ vncipher        (vRet, vRet, vKey3);
    __ vncipher        (vRet, vRet, vKey4);
    __ vncipherlast    (vRet, vRet, vKey5);

#ifdef VM_LITTLE_ENDIAN
    // toPerm = 0x0F0E0D0C0B0A09080706050403020100
    __ lvsl            (toPerm, keypos); // keypos is a multiple of 16
    __ vxor            (toPerm, toPerm, fSplt);

    // Swap Bytes
    __ vperm           (vRet, vRet, vRet, toPerm);
#endif

    // store result (unaligned)
    // Note: We can't use a read-modify-write sequence which touches additional Bytes.
    Register lo = temp, hi = fifteen; // Reuse
    __ vsldoi          (vTmp1, vRet, vRet, 8);
    __ mfvrd           (hi, vRet);
    __ mfvrd           (lo, vTmp1);
    __ std             (hi, 0 LITTLE_ENDIAN_ONLY(+ 8), to);
    __ std             (lo, 0 BIG_ENDIAN_ONLY(+ 8), to);

    __ blr();

#ifdef ASSERT
    __ bind(L_error);
    __ stop("aescrypt_decryptBlock: invalid key length");
#endif
     return start;
  }

  address generate_sha256_implCompress(StubGenStubId stub_id) {
    assert(UseSHA, "need SHA instructions");
    bool multi_block;
    switch (stub_id) {
    case sha256_implCompress_id:
      multi_block = false;
      break;
    case sha256_implCompressMB_id:
      multi_block = true;
      break;
    default:
      ShouldNotReachHere();
    }
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    __ sha256 (multi_block);
    __ blr();

    return start;
  }

  address generate_sha512_implCompress(StubGenStubId stub_id) {
    assert(UseSHA, "need SHA instructions");
    bool multi_block;
    switch (stub_id) {
    case sha512_implCompress_id:
      multi_block = false;
      break;
    case sha512_implCompressMB_id:
      multi_block = true;
      break;
    default:
      ShouldNotReachHere();
    }
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();

    __ sha512 (multi_block);
    __ blr();

    return start;
  }

  address generate_data_cache_writeback() {
    const Register cacheline = R3_ARG1;
    StubGenStubId stub_id = StubGenStubId::data_cache_writeback_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    __ cache_wb(Address(cacheline));
    __ blr();

    return start;
  }

  address generate_data_cache_writeback_sync() {
    const Register is_presync = R3_ARG1;
    Register temp = R4;
    Label SKIP;
    StubGenStubId stub_id = StubGenStubId::data_cache_writeback_sync_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    __ andi_(temp, is_presync, 1);
    __ bne(CR0, SKIP);
    __ cache_wbsync(false); // post sync => emit 'sync'
    __ bind(SKIP);          // pre sync => emit nothing
    __ blr();

    return start;
  }

  void generate_arraycopy_stubs() {
    // Note: the disjoint stubs must be generated first, some of
    // the conjoint stubs use them.

    address ucm_common_error_exit       =  generate_unsafecopy_common_error_exit();
    UnsafeMemoryAccess::set_common_exit_stub_pc(ucm_common_error_exit);

    // non-aligned disjoint versions
    StubRoutines::_jbyte_disjoint_arraycopy       = generate_disjoint_byte_copy(StubGenStubId::jbyte_disjoint_arraycopy_id);
    StubRoutines::_jshort_disjoint_arraycopy      = generate_disjoint_short_copy(StubGenStubId::jshort_disjoint_arraycopy_id);
    StubRoutines::_jint_disjoint_arraycopy        = generate_disjoint_int_copy(StubGenStubId::jint_disjoint_arraycopy_id);
    StubRoutines::_jlong_disjoint_arraycopy       = generate_disjoint_long_copy(StubGenStubId::jlong_disjoint_arraycopy_id);
    StubRoutines::_oop_disjoint_arraycopy         = generate_disjoint_oop_copy(StubGenStubId::oop_disjoint_arraycopy_id);
    StubRoutines::_oop_disjoint_arraycopy_uninit  = generate_disjoint_oop_copy(StubGenStubId::oop_disjoint_arraycopy_uninit_id);

    // aligned disjoint versions
    StubRoutines::_arrayof_jbyte_disjoint_arraycopy      = generate_disjoint_byte_copy(StubGenStubId::arrayof_jbyte_disjoint_arraycopy_id);
    StubRoutines::_arrayof_jshort_disjoint_arraycopy     = generate_disjoint_short_copy(StubGenStubId::arrayof_jshort_disjoint_arraycopy_id);
    StubRoutines::_arrayof_jint_disjoint_arraycopy       = generate_disjoint_int_copy(StubGenStubId::arrayof_jint_disjoint_arraycopy_id);
    StubRoutines::_arrayof_jlong_disjoint_arraycopy      = generate_disjoint_long_copy(StubGenStubId::arrayof_jlong_disjoint_arraycopy_id);
    StubRoutines::_arrayof_oop_disjoint_arraycopy        = generate_disjoint_oop_copy(StubGenStubId::arrayof_oop_disjoint_arraycopy_id);
    StubRoutines::_arrayof_oop_disjoint_arraycopy_uninit = generate_disjoint_oop_copy(StubGenStubId::oop_disjoint_arraycopy_uninit_id);

    // non-aligned conjoint versions
    StubRoutines::_jbyte_arraycopy      = generate_conjoint_byte_copy(StubGenStubId::jbyte_arraycopy_id);
    StubRoutines::_jshort_arraycopy     = generate_conjoint_short_copy(StubGenStubId::jshort_arraycopy_id);
    StubRoutines::_jint_arraycopy       = generate_conjoint_int_copy(StubGenStubId::jint_arraycopy_id);
    StubRoutines::_jlong_arraycopy      = generate_conjoint_long_copy(StubGenStubId::jlong_arraycopy_id);
    StubRoutines::_oop_arraycopy        = generate_conjoint_oop_copy(StubGenStubId::oop_arraycopy_id);
    StubRoutines::_oop_arraycopy_uninit = generate_conjoint_oop_copy(StubGenStubId::oop_arraycopy_uninit_id);

    // aligned conjoint versions
    StubRoutines::_arrayof_jbyte_arraycopy      = generate_conjoint_byte_copy(StubGenStubId::arrayof_jbyte_arraycopy_id);
    StubRoutines::_arrayof_jshort_arraycopy     = generate_conjoint_short_copy(StubGenStubId::arrayof_jshort_arraycopy_id);
    StubRoutines::_arrayof_jint_arraycopy       = generate_conjoint_int_copy(StubGenStubId::arrayof_jint_arraycopy_id);
    StubRoutines::_arrayof_jlong_arraycopy      = generate_conjoint_long_copy(StubGenStubId::arrayof_jlong_arraycopy_id);
    StubRoutines::_arrayof_oop_arraycopy        = generate_conjoint_oop_copy(StubGenStubId::arrayof_oop_arraycopy_id);
    StubRoutines::_arrayof_oop_arraycopy_uninit = generate_conjoint_oop_copy(StubGenStubId::arrayof_oop_arraycopy_id);

    // special/generic versions
    StubRoutines::_checkcast_arraycopy        = generate_checkcast_copy(StubGenStubId::checkcast_arraycopy_id);
    StubRoutines::_checkcast_arraycopy_uninit = generate_checkcast_copy(StubGenStubId::checkcast_arraycopy_uninit_id);

    StubRoutines::_unsafe_arraycopy  = generate_unsafe_copy(STUB_ENTRY(jbyte_arraycopy()),
                                                            STUB_ENTRY(jshort_arraycopy()),
                                                            STUB_ENTRY(jint_arraycopy()),
                                                            STUB_ENTRY(jlong_arraycopy()));
    StubRoutines::_generic_arraycopy = generate_generic_copy(STUB_ENTRY(jbyte_arraycopy()),
                                                             STUB_ENTRY(jshort_arraycopy()),
                                                             STUB_ENTRY(jint_arraycopy()),
                                                             STUB_ENTRY(oop_arraycopy()),
                                                             STUB_ENTRY(oop_disjoint_arraycopy()),
                                                             STUB_ENTRY(jlong_arraycopy()),
                                                             STUB_ENTRY(checkcast_arraycopy()));

    // fill routines
#ifdef COMPILER2
    if (OptimizeFill) {
      StubRoutines::_jbyte_fill          = generate_fill(StubGenStubId::jbyte_fill_id);
      StubRoutines::_jshort_fill         = generate_fill(StubGenStubId::jshort_fill_id);
      StubRoutines::_jint_fill           = generate_fill(StubGenStubId::jint_fill_id);
      StubRoutines::_arrayof_jbyte_fill  = generate_fill(StubGenStubId::arrayof_jbyte_fill_id);
      StubRoutines::_arrayof_jshort_fill = generate_fill(StubGenStubId::arrayof_jshort_fill_id);
      StubRoutines::_arrayof_jint_fill   = generate_fill(StubGenStubId::arrayof_jint_fill_id);
    }
    StubRoutines::_unsafe_setmemory = generate_unsafe_setmemory(StubRoutines::_jbyte_fill);
#endif
  }

  // Stub for BigInteger::multiplyToLen()
  //
  //  Arguments:
  //
  //  Input:
  //    R3 - x address
  //    R4 - x length
  //    R5 - y address
  //    R6 - y length
  //    R7 - z address
  //
  address generate_multiplyToLen() {

    StubGenStubId stub_id = StubGenStubId::multiplyToLen_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    const Register x     = R3;
    const Register xlen  = R4;
    const Register y     = R5;
    const Register ylen  = R6;
    const Register z     = R7;

    const Register tmp1  = R2; // TOC not used.
    const Register tmp2  = R9;
    const Register tmp3  = R10;
    const Register tmp4  = R11;
    const Register tmp5  = R12;

    // non-volatile regs
    const Register tmp6  = R31;
    const Register tmp7  = R30;
    const Register tmp8  = R29;
    const Register tmp9  = R28;
    const Register tmp10 = R27;
    const Register tmp11 = R26;
    const Register tmp12 = R25;
    const Register tmp13 = R24;

    BLOCK_COMMENT("Entry:");

    // C2 does not respect int to long conversion for stub calls.
    __ clrldi(xlen, xlen, 32);
    __ clrldi(ylen, ylen, 32);

    // Save non-volatile regs (frameless).
    int current_offs = 8;
    __ std(R24, -current_offs, R1_SP); current_offs += 8;
    __ std(R25, -current_offs, R1_SP); current_offs += 8;
    __ std(R26, -current_offs, R1_SP); current_offs += 8;
    __ std(R27, -current_offs, R1_SP); current_offs += 8;
    __ std(R28, -current_offs, R1_SP); current_offs += 8;
    __ std(R29, -current_offs, R1_SP); current_offs += 8;
    __ std(R30, -current_offs, R1_SP); current_offs += 8;
    __ std(R31, -current_offs, R1_SP);

    __ multiply_to_len(x, xlen, y, ylen, z, tmp1, tmp2, tmp3, tmp4, tmp5,
                       tmp6, tmp7, tmp8, tmp9, tmp10, tmp11, tmp12, tmp13);

    // Restore non-volatile regs.
    current_offs = 8;
    __ ld(R24, -current_offs, R1_SP); current_offs += 8;
    __ ld(R25, -current_offs, R1_SP); current_offs += 8;
    __ ld(R26, -current_offs, R1_SP); current_offs += 8;
    __ ld(R27, -current_offs, R1_SP); current_offs += 8;
    __ ld(R28, -current_offs, R1_SP); current_offs += 8;
    __ ld(R29, -current_offs, R1_SP); current_offs += 8;
    __ ld(R30, -current_offs, R1_SP); current_offs += 8;
    __ ld(R31, -current_offs, R1_SP);

    __ blr();  // Return to caller.

    return start;
  }

  /**
  *  Arguments:
  *
  *  Input:
  *   R3_ARG1    - out address
  *   R4_ARG2    - in address
  *   R5_ARG3    - offset
  *   R6_ARG4    - len
  *   R7_ARG5    - k
  *  Output:
  *   R3_RET     - carry
  */
  address generate_mulAdd() {
    __ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::mulAdd_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    // C2 does not sign extend signed parameters to full 64 bits registers:
    __ rldic (R5_ARG3, R5_ARG3, 2, 32);  // always positive
    __ clrldi(R6_ARG4, R6_ARG4, 32);     // force zero bits on higher word
    __ clrldi(R7_ARG5, R7_ARG5, 32);     // force zero bits on higher word

    __ muladd(R3_ARG1, R4_ARG2, R5_ARG3, R6_ARG4, R7_ARG5, R8, R9, R10);

    // Moves output carry to return register
    __ mr    (R3_RET,  R10);

    __ blr();

    return start;
  }

  /**
  *  Arguments:
  *
  *  Input:
  *   R3_ARG1    - in address
  *   R4_ARG2    - in length
  *   R5_ARG3    - out address
  *   R6_ARG4    - out length
  */
  address generate_squareToLen() {
    __ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::squareToLen_id;
    StubCodeMark mark(this, stub_id);

    address start = __ function_entry();

    // args - higher word is cleaned (unsignedly) due to int to long casting
    const Register in        = R3_ARG1;
    const Register in_len    = R4_ARG2;
    __ clrldi(in_len, in_len, 32);
    const Register out       = R5_ARG3;
    const Register out_len   = R6_ARG4;
    __ clrldi(out_len, out_len, 32);

    // output
    const Register ret       = R3_RET;

    // temporaries
    const Register lplw_s    = R7;
    const Register in_aux    = R8;
    const Register out_aux   = R9;
    const Register piece     = R10;
    const Register product   = R14;
    const Register lplw      = R15;
    const Register i_minus1  = R16;
    const Register carry     = R17;
    const Register offset    = R18;
    const Register off_aux   = R19;
    const Register t         = R20;
    const Register mlen      = R21;
    const Register len       = R22;
    const Register a         = R23;
    const Register b         = R24;
    const Register i         = R25;
    const Register c         = R26;
    const Register cs        = R27;

    // Labels
    Label SKIP_LSHIFT, SKIP_DIAGONAL_SUM, SKIP_ADDONE, SKIP_LOOP_SQUARE;
    Label LOOP_LSHIFT, LOOP_DIAGONAL_SUM, LOOP_ADDONE, LOOP_SQUARE;

    // Save non-volatile regs (frameless).
    int current_offs = -8;
    __ std(R28, current_offs, R1_SP); current_offs -= 8;
    __ std(R27, current_offs, R1_SP); current_offs -= 8;
    __ std(R26, current_offs, R1_SP); current_offs -= 8;
    __ std(R25, current_offs, R1_SP); current_offs -= 8;
    __ std(R24, current_offs, R1_SP); current_offs -= 8;
    __ std(R23, current_offs, R1_SP); current_offs -= 8;
    __ std(R22, current_offs, R1_SP); current_offs -= 8;
    __ std(R21, current_offs, R1_SP); current_offs -= 8;
    __ std(R20, current_offs, R1_SP); current_offs -= 8;
    __ std(R19, current_offs, R1_SP); current_offs -= 8;
    __ std(R18, current_offs, R1_SP); current_offs -= 8;
    __ std(R17, current_offs, R1_SP); current_offs -= 8;
    __ std(R16, current_offs, R1_SP); current_offs -= 8;
    __ std(R15, current_offs, R1_SP); current_offs -= 8;
    __ std(R14, current_offs, R1_SP);

    // Store the squares, right shifted one bit (i.e., divided by 2)
    __ subi   (out_aux,   out,       8);
    __ subi   (in_aux,    in,        4);
    __ cmpwi  (CR0,      in_len,    0);
    // Initialize lplw outside of the loop
    __ xorr   (lplw,      lplw,      lplw);
    __ ble    (CR0,      SKIP_LOOP_SQUARE);    // in_len <= 0
    __ mtctr  (in_len);

    __ bind(LOOP_SQUARE);
    __ lwzu   (piece,     4,         in_aux);
    __ mulld  (product,   piece,     piece);
    // shift left 63 bits and only keep the MSB
    __ rldic  (lplw_s,    lplw,      63, 0);
    __ mr     (lplw,      product);
    // shift right 1 bit without sign extension
    __ srdi   (product,   product,   1);
    // join them to the same register and store it
    __ orr    (product,   lplw_s,    product);
#ifdef VM_LITTLE_ENDIAN
    // Swap low and high words for little endian
    __ rldicl (product,   product,   32, 0);
#endif
    __ stdu   (product,   8,         out_aux);
    __ bdnz   (LOOP_SQUARE);

    __ bind(SKIP_LOOP_SQUARE);

    // Add in off-diagonal sums
    __ cmpwi  (CR0,      in_len,    0);
    __ ble    (CR0,      SKIP_DIAGONAL_SUM);
    // Avoid CTR usage here in order to use it at mulAdd
    __ subi   (i_minus1,  in_len,    1);
    __ li     (offset,    4);

    __ bind(LOOP_DIAGONAL_SUM);

    __ sldi   (off_aux,   out_len,   2);
    __ sub    (off_aux,   off_aux,   offset);

    __ mr     (len,       i_minus1);
    __ sldi   (mlen,      i_minus1,  2);
    __ lwzx   (t,         in,        mlen);

    __ muladd (out, in, off_aux, len, t, a, b, carry);

    // begin<addOne>
    // off_aux = out_len*4 - 4 - mlen - offset*4 - 4;
    __ addi   (mlen,      mlen,      4);
    __ sldi   (a,         out_len,   2);
    __ subi   (a,         a,         4);
    __ sub    (a,         a,         mlen);
    __ subi   (off_aux,   offset,    4);
    __ sub    (off_aux,   a,         off_aux);

    __ lwzx   (b,         off_aux,   out);
    __ add    (b,         b,         carry);
    __ stwx   (b,         off_aux,   out);

    // if (((uint64_t)s >> 32) != 0) {
    __ srdi_  (a,         b,         32);
    __ beq    (CR0,      SKIP_ADDONE);

    // while (--mlen >= 0) {
    __ bind(LOOP_ADDONE);
    __ subi   (mlen,      mlen,      4);
    __ cmpwi  (CR0,      mlen,      0);
    __ beq    (CR0,      SKIP_ADDONE);

    // if (--offset_aux < 0) { // Carry out of number
    __ subi   (off_aux,   off_aux,   4);
    __ cmpwi  (CR0,      off_aux,   0);
    __ blt    (CR0,      SKIP_ADDONE);

    // } else {
    __ lwzx   (b,         off_aux,   out);
    __ addi   (b,         b,         1);
    __ stwx   (b,         off_aux,   out);
    __ cmpwi  (CR0,      b,         0);
    __ bne    (CR0,      SKIP_ADDONE);
    __ b      (LOOP_ADDONE);

    __ bind(SKIP_ADDONE);
    // } } } end<addOne>

    __ addi   (offset,    offset,    8);
    __ subi   (i_minus1,  i_minus1,  1);
    __ cmpwi  (CR0,      i_minus1,  0);
    __ bge    (CR0,      LOOP_DIAGONAL_SUM);

    __ bind(SKIP_DIAGONAL_SUM);

    // Shift back up and set low bit
    // Shifts 1 bit left up to len positions. Assumes no leading zeros
    // begin<primitiveLeftShift>
    __ cmpwi  (CR0,      out_len,   0);
    __ ble    (CR0,      SKIP_LSHIFT);
    __ li     (i,         0);
    __ lwz    (c,         0,         out);
    __ subi   (b,         out_len,   1);
    __ mtctr  (b);

    __ bind(LOOP_LSHIFT);
    __ mr     (b,         c);
    __ addi   (cs,        i,         4);
    __ lwzx   (c,         out,       cs);

    __ sldi   (b,         b,         1);
    __ srwi   (cs,        c,         31);
    __ orr    (b,         b,         cs);
    __ stwx   (b,         i,         out);

    __ addi   (i,         i,         4);
    __ bdnz   (LOOP_LSHIFT);

    __ sldi   (c,         out_len,   2);
    __ subi   (c,         c,         4);
    __ lwzx   (b,         out,       c);
    __ sldi   (b,         b,         1);
    __ stwx   (b,         out,       c);

    __ bind(SKIP_LSHIFT);
    // end<primitiveLeftShift>

    // Set low bit
    __ sldi   (i,         in_len,    2);
    __ subi   (i,         i,         4);
    __ lwzx   (i,         in,        i);
    __ sldi   (c,         out_len,   2);
    __ subi   (c,         c,         4);
    __ lwzx   (b,         out,       c);

    __ andi   (i,         i,         1);
    __ orr    (i,         b,         i);

    __ stwx   (i,         out,       c);

    // Restore non-volatile regs.
    current_offs = -8;
    __ ld(R28, current_offs, R1_SP); current_offs -= 8;
    __ ld(R27, current_offs, R1_SP); current_offs -= 8;
    __ ld(R26, current_offs, R1_SP); current_offs -= 8;
    __ ld(R25, current_offs, R1_SP); current_offs -= 8;
    __ ld(R24, current_offs, R1_SP); current_offs -= 8;
    __ ld(R23, current_offs, R1_SP); current_offs -= 8;
    __ ld(R22, current_offs, R1_SP); current_offs -= 8;
    __ ld(R21, current_offs, R1_SP); current_offs -= 8;
    __ ld(R20, current_offs, R1_SP); current_offs -= 8;
    __ ld(R19, current_offs, R1_SP); current_offs -= 8;
    __ ld(R18, current_offs, R1_SP); current_offs -= 8;
    __ ld(R17, current_offs, R1_SP); current_offs -= 8;
    __ ld(R16, current_offs, R1_SP); current_offs -= 8;
    __ ld(R15, current_offs, R1_SP); current_offs -= 8;
    __ ld(R14, current_offs, R1_SP);

    __ mr(ret, out);
    __ blr();

    return start;
  }

  /**
   * Arguments:
   *
   * Inputs:
   *   R3_ARG1    - int   crc
   *   R4_ARG2    - byte* buf
   *   R5_ARG3    - int   length (of buffer)
   *
   * scratch:
   *   R2, R6-R12
   *
   * Output:
   *   R3_RET     - int   crc result
   */
  // Compute CRC32 function.
  address generate_CRC32_updateBytes(StubGenStubId stub_id) {
    bool is_crc32c;
    switch (stub_id) {
    case updateBytesCRC32_id:
      is_crc32c = false;
      break;
    case updateBytesCRC32C_id:
      is_crc32c = true;
      break;
    default:
      ShouldNotReachHere();
    }
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, stub_id);
    address start = __ function_entry();  // Remember stub start address (is rtn value).
    __ crc32(R3_ARG1, R4_ARG2, R5_ARG3, R2, R6, R7, R8, R9, R10, R11, R12, is_crc32c);
    __ blr();
    return start;
  }

  address generate_floatToFloat16() {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "floatToFloat16");
    address start = __ function_entry();
    __ f2hf(R3_RET, F1_ARG1, F0);
    __ blr();
    return start;
  }

  address generate_float16ToFloat() {
    __ align(CodeEntryAlignment);
    StubCodeMark mark(this, "StubRoutines", "float16ToFloat");
    address start = __ function_entry();
    __ hf2f(F1_RET, R3_ARG1);
    __ blr();
    return start;
  }

  address generate_method_entry_barrier() {
    __ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::method_entry_barrier_id;
    StubCodeMark mark(this, stub_id);

    address stub_address = __ pc();

    int nbytes_save = MacroAssembler::num_volatile_regs * BytesPerWord;
    __ save_volatile_gprs(R1_SP, -nbytes_save, true);

    // Link register points to instruction in prologue of the guarded nmethod.
    // As the stub requires one layer of indirection (argument is of type address* and not address),
    // passing the link register's value directly doesn't work.
    // Since we have to save the link register on the stack anyway, we calculate the corresponding stack address
    // and pass that one instead.
    __ addi(R3_ARG1, R1_SP, _abi0(lr));

    __ save_LR(R0);
    __ push_frame_reg_args(nbytes_save, R0);

    __ call_VM_leaf(CAST_FROM_FN_PTR(address, BarrierSetNMethod::nmethod_stub_entry_barrier));
    __ mr(R0, R3_RET);

    __ pop_frame();
    __ restore_LR(R3_RET /* used as tmp register */);
    __ restore_volatile_gprs(R1_SP, -nbytes_save, true);

    __ cmpdi(CR0, R0, 0);

    // Return to prologue if no deoptimization is required (bnelr)
    __ bclr(Assembler::bcondCRbiIs1, Assembler::bi0(CR0, Assembler::equal), Assembler::bhintIsTaken);

    // Deoptimization required.
    // For actually handling the deoptimization, the 'wrong method stub' is invoked.
    __ load_const_optimized(R0, SharedRuntime::get_handle_wrong_method_stub());
    __ mtctr(R0);

    // Pop the frame built in the prologue.
    __ pop_frame();

    // Restore link register.  Required as the 'wrong method stub' needs the caller's frame
    // to properly deoptimize this method (e.g. by re-resolving the call site for compiled methods).
    // This method's prologue is aborted.
    __ restore_LR(R0);

    __ bctr();
    return stub_address;
  }

#ifdef VM_LITTLE_ENDIAN
// The following Base64 decode intrinsic is based on an algorithm outlined
// in here:
// http://0x80.pl/notesen/2016-01-17-sse-base64-decoding.html
// in the section titled "Vector lookup (pshufb with bitmask)"
//
// This implementation differs in the following ways:
//  * Instead of Intel SSE instructions, Power AltiVec VMX and VSX instructions
//    are used instead.  It turns out that some of the vector operations
//    needed in the algorithm require fewer AltiVec instructions.
//  * The algorithm in the above mentioned paper doesn't handle the
//    Base64-URL variant in RFC 4648.  Adjustments to both the code and to two
//    lookup tables are needed for this.
//  * The "Pack" section of the code is a complete rewrite for Power because we
//    can utilize better instructions for this step.
//

// Offsets per group of Base64 characters
// Uppercase
#define UC  (signed char)((-'A' + 0) & 0xff)
// Lowercase
#define LC  (signed char)((-'a' + 26) & 0xff)
// Digits
#define DIG (signed char)((-'0' + 52) & 0xff)
// Plus sign (URL = 0)
#define PLS (signed char)((-'+' + 62) & 0xff)
// Hyphen (URL = 1)
#define HYP (signed char)((-'-' + 62) & 0xff)
// Slash (URL = 0)
#define SLS (signed char)((-'/' + 63) & 0xff)
// Underscore (URL = 1)
#define US  (signed char)((-'_' + 63) & 0xff)

// For P10 (or later) only
#define VALID_B64 0x80
#define VB64(x) (VALID_B64 | x)

#define BLK_OFFSETOF(x) (offsetof(constant_block, x))

// In little-endian mode, the lxv instruction loads the element at EA into
// element 15 of the vector register, EA+1 goes into element 14, and so
// on.
//
// To make a look-up table easier to read, ARRAY_TO_LXV_ORDER reverses the
// order of the elements in a vector initialization.
#define ARRAY_TO_LXV_ORDER(e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15) e15, e14, e13, e12, e11, e10, e9, e8, e7, e6, e5, e4, e3, e2, e1, e0

  //
  // Base64 decodeBlock intrinsic
  address generate_base64_decodeBlock() {
    __ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::base64_decodeBlock_id;
    StubCodeMark mark(this, stub_id);
    address start   = __ function_entry();

    typedef struct {
      signed char offsetLUT_val[16];
      signed char offsetLUT_URL_val[16];
      unsigned char maskLUT_val[16];
      unsigned char maskLUT_URL_val[16];
      unsigned char bitposLUT_val[16];
      unsigned char table_32_47_val[16];
      unsigned char table_32_47_URL_val[16];
      unsigned char table_48_63_val[16];
      unsigned char table_64_79_val[16];
      unsigned char table_80_95_val[16];
      unsigned char table_80_95_URL_val[16];
      unsigned char table_96_111_val[16];
      unsigned char table_112_127_val[16];
      unsigned char pack_lshift_val[16];
      unsigned char pack_rshift_val[16];
      unsigned char pack_permute_val[16];
    } constant_block;

    alignas(16) static const constant_block const_block = {

      .offsetLUT_val = {
        ARRAY_TO_LXV_ORDER(
        0,   0, PLS, DIG,  UC,  UC,  LC,  LC,
        0,   0,   0,   0,   0,   0,   0,   0 ) },

      .offsetLUT_URL_val = {
        ARRAY_TO_LXV_ORDER(
        0,   0, HYP, DIG,  UC,  UC,  LC,  LC,
        0,   0,   0,   0,   0,   0,   0,   0 ) },

      .maskLUT_val = {
        ARRAY_TO_LXV_ORDER(
        /* 0        */ (unsigned char)0b10101000,
        /* 1 .. 9   */ (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000,
                       (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000,
                       (unsigned char)0b11111000,
        /* 10       */ (unsigned char)0b11110000,
        /* 11       */ (unsigned char)0b01010100,
        /* 12 .. 14 */ (unsigned char)0b01010000, (unsigned char)0b01010000, (unsigned char)0b01010000,
        /* 15       */ (unsigned char)0b01010100 ) },

      .maskLUT_URL_val = {
        ARRAY_TO_LXV_ORDER(
        /* 0        */ (unsigned char)0b10101000,
        /* 1 .. 9   */ (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000,
                       (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000, (unsigned char)0b11111000,
                       (unsigned char)0b11111000,
        /* 10       */ (unsigned char)0b11110000,
        /* 11 .. 12 */ (unsigned char)0b01010000, (unsigned char)0b01010000,
        /* 13       */ (unsigned char)0b01010100,
        /* 14       */ (unsigned char)0b01010000,
        /* 15       */ (unsigned char)0b01110000 ) },

      .bitposLUT_val = {
        ARRAY_TO_LXV_ORDER(
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, (unsigned char)0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ) },

      // In the following table_*_val constants, a 0 value means the
      // character is not in the Base64 character set
      .table_32_47_val = {
        ARRAY_TO_LXV_ORDER (
         /* space .. '*' = 0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* '+' = 62 */ VB64(62), /* ',' .. '.' = 0 */ 0, 0, 0, /* '/' = 63 */ VB64(63) ) },

      .table_32_47_URL_val = {
        ARRAY_TO_LXV_ORDER(
         /* space .. ',' = 0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* '-' = 62 */ VB64(62), /* '.' .. '/' */ 0, 0 ) },

      .table_48_63_val = {
        ARRAY_TO_LXV_ORDER(
         /* '0' .. '9' = 52 .. 61 */ VB64(52), VB64(53), VB64(54), VB64(55), VB64(56), VB64(57), VB64(58), VB64(59), VB64(60), VB64(61),
         /* ':' .. '?' = 0 */ 0, 0, 0, 0, 0, 0 ) },

      .table_64_79_val = {
        ARRAY_TO_LXV_ORDER(
         /* '@' = 0 */ 0, /* 'A' .. 'O' = 0 .. 14 */ VB64(0), VB64(1), VB64(2), VB64(3), VB64(4), VB64(5), VB64(6), VB64(7), VB64(8),
         VB64(9), VB64(10), VB64(11), VB64(12), VB64(13), VB64(14) ) },

      .table_80_95_val = {
        ARRAY_TO_LXV_ORDER(/* 'P' .. 'Z' = 15 .. 25 */ VB64(15), VB64(16), VB64(17), VB64(18), VB64(19), VB64(20), VB64(21), VB64(22),
        VB64(23), VB64(24), VB64(25), /* '[' .. '_' = 0 */ 0, 0, 0, 0, 0 ) },

      .table_80_95_URL_val = {
        ARRAY_TO_LXV_ORDER(/* 'P' .. 'Z' = 15 .. 25 */ VB64(15), VB64(16), VB64(17), VB64(18), VB64(19), VB64(20), VB64(21), VB64(22),
        VB64(23), VB64(24), VB64(25), /* '[' .. '^' = 0 */ 0, 0, 0, 0, /* '_' = 63 */ VB64(63) ) },

      .table_96_111_val = {
        ARRAY_TO_LXV_ORDER(/* '`' = 0 */ 0, /* 'a' .. 'o' = 26 .. 40 */ VB64(26), VB64(27), VB64(28), VB64(29), VB64(30), VB64(31),
        VB64(32), VB64(33), VB64(34), VB64(35), VB64(36), VB64(37), VB64(38), VB64(39), VB64(40) ) },

      .table_112_127_val = {
        ARRAY_TO_LXV_ORDER(/* 'p' .. 'z' = 41 .. 51 */ VB64(41), VB64(42), VB64(43), VB64(44), VB64(45), VB64(46), VB64(47), VB64(48),
        VB64(49), VB64(50), VB64(51), /* '{' .. DEL = 0 */ 0, 0, 0, 0, 0 ) },

      .pack_lshift_val = {
        ARRAY_TO_LXV_ORDER(
        0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2 ) },

      .pack_rshift_val = {
        ARRAY_TO_LXV_ORDER(
        0, 2, 4, 0, 0, 2, 4, 0, 0, 2, 4, 0, 0, 2, 4, 0 ) },

      // The first 4 index values are "don't care" because
      // we only use the first 12 bytes of the vector,
      // which are decoded from 16 bytes of Base64 characters.
      .pack_permute_val = {
        ARRAY_TO_LXV_ORDER(
         0, 0, 0, 0,
         0,  1,  2,
         4,  5,  6,
         8,  9, 10,
        12, 13, 14 ) }
    };

    const unsigned block_size = 16;  // number of bytes to process in each pass through the loop
    const unsigned block_size_shift = 4;

    // According to the ELF V2 ABI, registers r3-r12 are volatile and available for use without save/restore
    Register s      = R3_ARG1; // source starting address of Base64 characters
    Register sp     = R4_ARG2; // source offset
    Register sl     = R5_ARG3; // source length = # of Base64 characters to be processed
    Register d      = R6_ARG4; // destination address
    Register dp     = R7_ARG5; // destination offset
    Register isURL  = R8_ARG6; // boolean, if non-zero indicates use of RFC 4648 base64url encoding
    Register isMIME = R9_ARG7; // boolean, if non-zero indicates use of RFC 2045 MIME encoding - not used

    // Local variables
    Register const_ptr     = R9;  // used for loading constants
    Register tmp_reg       = R10; // used for speeding up load_constant_optimized()

    // Re-use R9 and R10 to avoid using non-volatile registers (requires save/restore)
    Register out           = R9;  // moving out (destination) pointer
    Register in            = R10; // moving in (source) pointer

    // Volatile VSRS are 0..13, 32..51 (VR0..VR13)
    // VR Constants
    VectorRegister  vec_0s                  = VR0;
    VectorRegister  vec_4s                  = VR1;
    VectorRegister  vec_8s                  = VR2;
    VectorRegister  vec_special_case_char   = VR3;
    VectorRegister  pack_rshift             = VR4;
    VectorRegister  pack_lshift             = VR5;

    // VSR Constants
    VectorSRegister offsetLUT               = VSR0;
    VectorSRegister maskLUT                 = VSR1;
    VectorSRegister bitposLUT               = VSR2;
    VectorSRegister vec_0xfs                = VSR3;
    VectorSRegister vec_special_case_offset = VSR4;
    VectorSRegister pack_permute            = VSR5;

    // P10 (or later) VSR lookup constants
    VectorSRegister table_32_47             = VSR0;
    VectorSRegister table_48_63             = VSR1;
    VectorSRegister table_64_79             = VSR2;
    VectorSRegister table_80_95             = VSR3;
    VectorSRegister table_96_111            = VSR4;
    VectorSRegister table_112_127           = VSR6;

    // Data read in and later converted
    VectorRegister  input                   = VR6;
    // Variable for testing Base64 validity
    VectorRegister  non_match               = VR10;

    // P9 VR Variables for lookup
    VectorRegister  higher_nibble           = VR7;
    VectorRegister  eq_special_case_char    = VR8;
    VectorRegister  offsets                 = VR9;

    // P9 VSR lookup variables
    VectorSRegister bit                     = VSR6;
    VectorSRegister lower_nibble            = VSR7;
    VectorSRegister M                       = VSR8;

    // P10 (or later) VSR lookup variables
    VectorSRegister  xlate_a                = VSR7;
    VectorSRegister  xlate_b                = VSR8;

    // Variables for pack
    // VR
    VectorRegister  l                       = VR7;  // reuse higher_nibble's register
    VectorRegister  r                       = VR8;  // reuse eq_special_case_char's register
    VectorRegister  gathered                = VR10; // reuse non_match's register

    Label not_URL, calculate_size, loop_start, loop_exit, return_zero;

    // The upper 32 bits of the non-pointer parameter registers are not
    // guaranteed to be zero, so mask off those upper bits.
    __ clrldi(sp, sp, 32);
    __ clrldi(sl, sl, 32);

    // Don't handle the last 4 characters of the source, because this
    // VSX-based algorithm doesn't handle padding characters.  Also the
    // vector code will always write 16 bytes of decoded data on each pass,
    // but only the first 12 of those 16 bytes are valid data (16 base64
    // characters become 12 bytes of binary data), so for this reason we
    // need to subtract an additional 8 bytes from the source length, in
    // order not to write past the end of the destination buffer.  The
    // result of this subtraction implies that a Java function in the
    // Base64 class will be used to process the last 12 characters.
    __ sub(sl, sl, sp);
    __ subi(sl, sl, 12);

    // Load CTR with the number of passes through the loop
    // = sl >> block_size_shift.  After the shift, if sl <= 0, there's too
    // little data to be processed by this intrinsic.
    __ srawi_(sl, sl, block_size_shift);
    __ ble(CR0, return_zero);
    __ mtctr(sl);

    // Clear the other two parameter registers upper 32 bits.
    __ clrldi(isURL, isURL, 32);
    __ clrldi(dp, dp, 32);

    // Load constant vec registers that need to be loaded from memory
    __ load_const_optimized(const_ptr, (address)&const_block, tmp_reg);
    __ lxv(bitposLUT, BLK_OFFSETOF(bitposLUT_val), const_ptr);
    __ lxv(pack_rshift->to_vsr(), BLK_OFFSETOF(pack_rshift_val), const_ptr);
    __ lxv(pack_lshift->to_vsr(), BLK_OFFSETOF(pack_lshift_val), const_ptr);
    __ lxv(pack_permute, BLK_OFFSETOF(pack_permute_val), const_ptr);

    // Splat the constants that can use xxspltib
    __ xxspltib(vec_0s->to_vsr(), 0);
    __ xxspltib(vec_8s->to_vsr(), 8);
    if (PowerArchitecturePPC64 >= 10) {
      // Using VALID_B64 for the offsets effectively strips the upper bit
      // of each byte that was selected from the table.  Setting the upper
      // bit gives us a way to distinguish between the 6-bit value of 0
      // from an error code of 0, which will happen if the character is
      // outside the range of the lookup, or is an illegal Base64
      // character, such as %.
      __ xxspltib(offsets->to_vsr(), VALID_B64);

      __ lxv(table_48_63, BLK_OFFSETOF(table_48_63_val), const_ptr);
      __ lxv(table_64_79, BLK_OFFSETOF(table_64_79_val), const_ptr);
      __ lxv(table_80_95, BLK_OFFSETOF(table_80_95_val), const_ptr);
      __ lxv(table_96_111, BLK_OFFSETOF(table_96_111_val), const_ptr);
      __ lxv(table_112_127, BLK_OFFSETOF(table_112_127_val), const_ptr);
    } else {
      __ xxspltib(vec_4s->to_vsr(), 4);
      __ xxspltib(vec_0xfs, 0xf);
      __ lxv(bitposLUT, BLK_OFFSETOF(bitposLUT_val), const_ptr);
    }

    // The rest of the constants use different values depending on the
    // setting of isURL
    __ cmpwi(CR0, isURL, 0);
    __ beq(CR0, not_URL);

    // isURL != 0 (true)
    if (PowerArchitecturePPC64 >= 10) {
      __ lxv(table_32_47, BLK_OFFSETOF(table_32_47_URL_val), const_ptr);
      __ lxv(table_80_95, BLK_OFFSETOF(table_80_95_URL_val), const_ptr);
    } else {
      __ lxv(offsetLUT, BLK_OFFSETOF(offsetLUT_URL_val), const_ptr);
      __ lxv(maskLUT, BLK_OFFSETOF(maskLUT_URL_val), const_ptr);
      __ xxspltib(vec_special_case_char->to_vsr(), '_');
      __ xxspltib(vec_special_case_offset, (unsigned char)US);
    }
    __ b(calculate_size);

    // isURL = 0 (false)
    __ bind(not_URL);
    if (PowerArchitecturePPC64 >= 10) {
      __ lxv(table_32_47, BLK_OFFSETOF(table_32_47_val), const_ptr);
      __ lxv(table_80_95, BLK_OFFSETOF(table_80_95_val), const_ptr);
    } else {
      __ lxv(offsetLUT, BLK_OFFSETOF(offsetLUT_val), const_ptr);
      __ lxv(maskLUT, BLK_OFFSETOF(maskLUT_val), const_ptr);
      __ xxspltib(vec_special_case_char->to_vsr(), '/');
      __ xxspltib(vec_special_case_offset, (unsigned char)SLS);
    }

    __ bind(calculate_size);

    // out starts at d + dp
    __ add(out, d, dp);

    // in starts at s + sp
    __ add(in, s, sp);

    __ align(32);
    __ bind(loop_start);
    __ lxv(input->to_vsr(), 0, in); // offset=0

    //
    // Lookup
    //
    if (PowerArchitecturePPC64 >= 10) {
      // Use xxpermx to do a lookup of each Base64 character in the
      // input vector and translate it to a 6-bit value + 0x80.
      // Characters which are not valid Base64 characters will result
      // in a zero in the corresponding byte.
      //
      // Note that due to align(32) call above, the xxpermx instructions do
      // not require align_prefix() calls, since the final xxpermx
      // prefix+opcode is at byte 24.
      __ xxpermx(xlate_a, table_32_47, table_48_63, input->to_vsr(), 1);    // offset=4
      __ xxpermx(xlate_b, table_64_79, table_80_95, input->to_vsr(), 2);    // offset=12
      __ xxlor(xlate_b, xlate_a, xlate_b);                                  // offset=20
      __ xxpermx(xlate_a, table_96_111, table_112_127, input->to_vsr(), 3); // offset=24
      __ xxlor(input->to_vsr(), xlate_a, xlate_b);
      // Check for non-Base64 characters by comparing each byte to zero.
      __ vcmpequb_(non_match, input, vec_0s);
    } else {
      // Isolate the upper 4 bits of each character by shifting it right 4 bits
      __ vsrb(higher_nibble, input, vec_4s);
      // Isolate the lower 4 bits by masking
      __ xxland(lower_nibble, input->to_vsr(), vec_0xfs);

      // Get the offset (the value to subtract from the byte) by using
      // a lookup table indexed by the upper 4 bits of the character
      __ xxperm(offsets->to_vsr(), offsetLUT, higher_nibble->to_vsr());

      // Find out which elements are the special case character (isURL ? '/' : '-')
      __ vcmpequb(eq_special_case_char, input, vec_special_case_char);

      // For each character in the input which is a special case
      // character, replace its offset with one that is special for that
      // character.
      __ xxsel(offsets->to_vsr(), offsets->to_vsr(), vec_special_case_offset, eq_special_case_char->to_vsr());

      // Use the lower_nibble to select a mask "M" from the lookup table.
      __ xxperm(M, maskLUT, lower_nibble);

      // "bit" is used to isolate which of the bits in M is relevant.
      __ xxperm(bit, bitposLUT, higher_nibble->to_vsr());

      // Each element of non_match correspond to one each of the 16 input
      // characters.  Those elements that become 0x00 after the xxland
      // instruction are invalid Base64 characters.
      __ xxland(non_match->to_vsr(), M, bit);

      // Compare each element to zero
      //
      __ vcmpequb_(non_match, non_match, vec_0s);
    }
    // vmcmpequb_ sets the EQ bit of CR6 if no elements compare equal.
    // Any element comparing equal to zero means there is an error in
    // that element.  Note that the comparison result register
    // non_match is not referenced again.  Only CR6-EQ matters.
    __ bne_predict_not_taken(CR6, loop_exit);

    // The Base64 characters had no errors, so add the offsets, which in
    // the case of Power10 is a constant vector of all 0x80's (see earlier
    // comment where the offsets register is loaded).
    __ vaddubm(input, input, offsets);

    // Pack
    //
    // In the tables below, b0, b1, .. b15 are the bytes of decoded
    // binary data, the first line of each of the cells (except for
    // the constants) uses the bit-field nomenclature from the
    // above-linked paper, whereas the second line is more specific
    // about which exact bits are present, and is constructed using the
    // Power ISA 3.x document style, where:
    //
    // * The specifier after the colon depicts which bits are there.
    // * The bit numbering is big endian style (bit 0 is the most
    //   significant).
    // * || is a concatenate operator.
    // * Strings of 0's are a field of zeros with the shown length, and
    //   likewise for strings of 1's.

    // Note that only e12..e15 are shown here because the shifting
    // and OR'ing pattern replicates for e8..e11, e4..7, and
    // e0..e3.
    //
    // +======================+=================+======================+======================+=============+
    // |        Vector        |       e12       |         e13          |         e14          |     e15     |
    // |       Element        |                 |                      |                      |             |
    // +======================+=================+======================+======================+=============+
    // |    after vaddubm     |    00dddddd     |       00cccccc       |       00bbbbbb       |  00aaaaaa   |
    // |                      |   00||b2:2..7   | 00||b1:4..7||b2:0..1 | 00||b0:6..7||b1:0..3 | 00||b0:0..5 |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // |     pack_lshift      |                 |         << 6         |         << 4         |    << 2     |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // |     l after vslb     |    00dddddd     |       cc000000       |       bbbb0000       |  aaaaaa00   |
    // |                      |   00||b2:2..7   |   b2:0..1||000000    |    b1:0..3||0000     | b0:0..5||00 |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // |     l after vslo     |    cc000000     |       bbbb0000       |       aaaaaa00       |  00000000   |
    // |                      | b2:0..1||000000 |    b1:0..3||0000     |     b0:0..5||00      |  00000000   |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // |     pack_rshift      |                 |         >> 2         |         >> 4         |             |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // |     r after vsrb     |    00dddddd     |       0000cccc       |       000000bb       |  00aaaaaa   |
    // |                      |   00||b2:2..7   |    0000||b1:4..7     |   000000||b0:6..7    | 00||b0:0..5 |
    // +----------------------+-----------------+----------------------+----------------------+-------------+
    // | gathered after xxlor |    ccdddddd     |       bbbbcccc       |       aaaaaabb       |  00aaaaaa   |
    // |                      |     b2:0..7     |       b1:0..7        |       b0:0..7        | 00||b0:0..5 |
    // +======================+=================+======================+======================+=============+
    //
    // Note: there is a typo in the above-linked paper that shows the result of the gathering process is:
    // [ddddddcc|bbbbcccc|aaaaaabb]
    // but should be:
    // [ccdddddd|bbbbcccc|aaaaaabb]
    //
    __ vslb(l, input, pack_lshift);
    // vslo of vec_8s shifts the vector by one octet toward lower
    // element numbers, discarding element 0.  This means it actually
    // shifts to the right (not left) according to the order of the
    // table above.
    __ vslo(l, l, vec_8s);
    __ vsrb(r, input, pack_rshift);
    __ xxlor(gathered->to_vsr(), l->to_vsr(), r->to_vsr());

    // Final rearrangement of bytes into their correct positions.
    // +==============+======+======+======+======+=====+=====+====+====+====+====+=====+=====+=====+=====+=====+=====+
    // |    Vector    |  e0  |  e1  |  e2  |  e3  | e4  | e5  | e6 | e7 | e8 | e9 | e10 | e11 | e12 | e13 | e14 | e15 |
    // |   Elements   |      |      |      |      |     |     |    |    |    |    |     |     |     |     |     |     |
    // +==============+======+======+======+======+=====+=====+====+====+====+====+=====+=====+=====+=====+=====+=====+
    // | after xxlor  | b11  | b10  |  b9  |  xx  | b8  | b7  | b6 | xx | b5 | b4 | b3  | xx  | b2  | b1  | b0  | xx  |
    // +--------------+------+------+------+------+-----+-----+----+----+----+----+-----+-----+-----+-----+-----+-----+
    // | pack_permute |  0   |  0   |  0   |  0   |  0  |  1  | 2  | 4  | 5  | 6  |  8  |  9  | 10  | 12  | 13  | 14  |
    // +--------------+------+------+------+------+-----+-----+----+----+----+----+-----+-----+-----+-----+-----+-----+
    // | after xxperm | b11* | b11* | b11* | b11* | b11 | b10 | b9 | b8 | b7 | b6 | b5  | b4  | b3  | b2  | b1  | b0  |
    // +==============+======+======+======+======+=====+=====+====+====+====+====+=====+=====+=====+=====+=====+=====+
    // xx bytes are not used to form the final data
    // b0..b15 are the decoded and reassembled 8-bit bytes of data
    // b11 with asterisk is a "don't care", because these bytes will be
    // overwritten on the next iteration.
    __ xxperm(gathered->to_vsr(), gathered->to_vsr(), pack_permute);

    // We cannot use a static displacement on the store, since it's a
    // multiple of 12, not 16.  Note that this stxv instruction actually
    // writes 16 bytes, even though only the first 12 are valid data.
    __ stxv(gathered->to_vsr(), 0, out);
    __ addi(out, out, 12);
    __ addi(in, in, 16);
    __ bdnz(loop_start);

    __ bind(loop_exit);

    // Return the number of out bytes produced, which is (out - (d + dp)) == out - d - dp;
    __ sub(R3_RET, out, d);
    __ sub(R3_RET, R3_RET, dp);

    __ blr();

    __ bind(return_zero);
    __ li(R3_RET, 0);
    __ blr();

    return start;
  }

#undef UC
#undef LC
#undef DIG
#undef PLS
#undef HYP
#undef SLS
#undef US

// This algorithm is based on the methods described in this paper:
// http://0x80.pl/notesen/2016-01-12-sse-base64-encoding.html
//
// The details of this implementation vary from the paper due to the
// difference in the ISA between SSE and AltiVec, especially in the
// splitting bytes section where there is no need on Power to mask after
// the shift because the shift is byte-wise rather than an entire an entire
// 128-bit word.
//
// For the lookup part of the algorithm, different logic is used than
// described in the paper because of the availability of vperm, which can
// do a 64-byte table lookup in four instructions, while preserving the
// branchless nature.
//
// Description of the ENCODE_CORE macro
//
// Expand first 12 x 8-bit data bytes into 16 x 6-bit bytes (upper 2
// bits of each byte are zeros)
//
// (Note: e7..e0 are not shown because they follow the same pattern as
// e8..e15)
//
// In the table below, b0, b1, .. b15 are the bytes of unencoded
// binary data, the first line of each of the cells (except for
// the constants) uses the bit-field nomenclature from the
// above-linked paper, whereas the second line is more specific
// about which exact bits are present, and is constructed using the
// Power ISA 3.x document style, where:
//
// * The specifier after the colon depicts which bits are there.
// * The bit numbering is big endian style (bit 0 is the most
//   significant).
// * || is a concatenate operator.
// * Strings of 0's are a field of zeros with the shown length, and
//   likewise for strings of 1's.
//
// +==========================+=============+======================+======================+=============+=============+======================+======================+=============+
// |          Vector          |     e8      |          e9          |         e10          |     e11     |     e12     |         e13          |         e14          |     e15     |
// |         Element          |             |                      |                      |             |             |                      |                      |             |
// +==========================+=============+======================+======================+=============+=============+======================+======================+=============+
// |        after lxv         |  jjjjkkkk   |       iiiiiijj       |       gghhhhhh       |  ffffgggg   |  eeeeeeff   |       ccdddddd       |       bbbbcccc       |  aaaaaabb   |
// |                          |     b7      |          b6          |          b5          |     b4      |     b3      |          b2          |          b1          |     b0      |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |      xxperm indexes      |      0      |          10          |          11          |     12      |      0      |          13          |          14          |     15      |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |     (1) after xxperm     |             |       gghhhhhh       |       ffffgggg       |  eeeeeeff   |             |       ccdddddd       |       bbbbcccc       |  aaaaaabb   |
// |                          |    (b15)    |          b5          |          b4          |     b3      |    (b15)    |          b2          |          b1          |     b0      |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |      rshift_amount       |      0      |          6           |          4           |      2      |      0      |          6           |          4           |      2      |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |        after vsrb        |             |       000000gg       |       0000ffff       |  00eeeeee   |             |       000000cc       |       0000bbbb       |  00aaaaaa   |
// |                          |    (b15)    |   000000||b5:0..1    |    0000||b4:0..3     | 00||b3:0..5 |    (b15)    |   000000||b2:0..1    |    0000||b1:0..3     | 00||b0:0..5 |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |       rshift_mask        |  00000000   |      000000||11      |      0000||1111      | 00||111111  |  00000000   |      000000||11      |      0000||1111      | 00||111111  |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |    rshift after vand     |  00000000   |       000000gg       |       0000ffff       |  00eeeeee   |  00000000   |       000000cc       |       0000bbbb       |  00aaaaaa   |
// |                          |  00000000   |   000000||b5:0..1    |    0000||b4:0..3     | 00||b3:0..5 |  00000000   |   000000||b2:0..1    |    0000||b1:0..3     | 00||b0:0..5 |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |    1 octet lshift (1)    |  gghhhhhh   |       ffffgggg       |       eeeeeeff       |             |  ccdddddd   |       bbbbcccc       |       aaaaaabb       |  00000000   |
// |                          |     b5      |          b4          |          b3          |    (b15)    |     b2      |          b1          |          b0          |  00000000   |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |      lshift_amount       |      0      |          2           |          4           |      0      |      0      |          2           |          4           |      0      |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |        after vslb        |  gghhhhhh   |       ffgggg00       |       eeff0000       |             |  ccdddddd   |       bbcccc00       |       aabb0000       |  00000000   |
// |                          |     b5      |     b4:2..7||00      |    b3:4..7||0000     |    (b15)    |   b2:0..7   |     b1:2..7||00      |    b0:4..7||0000     |  00000000   |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |       lshift_mask        | 00||111111  |     00||1111||00     |     00||11||0000     |  00000000   | 00||111111  |     00||1111||00     |     00||11||0000     |  00000000   |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// |    lshift after vand     |  00hhhhhh   |       00gggg00       |       00ff0000       |  00000000   |  00dddddd   |       00cccc00       |       00bb0000       |  00000000   |
// |                          | 00||b5:2..7 |   00||b4:4..7||00    |  00||b3:6..7||0000   |  00000000   | 00||b2:2..7 |   00||b1:4..7||00    |  00||b0:6..7||0000   |  00000000   |
// +--------------------------+-------------+----------------------+----------------------+-------------+-------------+----------------------+----------------------+-------------+
// | after vor lshift, rshift |  00hhhhhh   |       00gggggg       |       00ffffff       |  00eeeeee   |  00dddddd   |       00cccccc       |       00bbbbbb       |  00aaaaaa   |
// |                          | 00||b5:2..7 | 00||b4:4..7||b5:0..1 | 00||b3:6..7||b4:0..3 | 00||b3:0..5 | 00||b2:2..7 | 00||b1:4..7||b2:0..1 | 00||b0:6..7||b1:0..3 | 00||b0:0..5 |
// +==========================+=============+======================+======================+=============+=============+======================+======================+=============+
//
// Expand the first 12 bytes into 16 bytes, leaving every 4th byte
// blank for now.
// __ xxperm(input->to_vsr(), input->to_vsr(), expand_permute);
//
// Generate two bit-shifted pieces - rshift and lshift - that will
// later be OR'd together.
//
// First the right-shifted piece
// __ vsrb(rshift, input, expand_rshift);
// __ vand(rshift, rshift, expand_rshift_mask);
//
// Now the left-shifted piece, which is done by octet shifting
// the input one byte to the left, then doing a variable shift,
// followed by a mask operation.
//
// __ vslo(lshift, input, vec_8s);
// __ vslb(lshift, lshift, expand_lshift);
// __ vand(lshift, lshift, expand_lshift_mask);
//
// Combine the two pieces by OR'ing
// __ vor(expanded, rshift, lshift);
//
// At this point, expanded is a vector containing a 6-bit value in each
// byte.  These values are used as indexes into a 64-byte lookup table that
// is contained in four vector registers.  The lookup operation is done
// using vperm instructions with the same indexes for the lower 32 and
// upper 32 bytes.  To figure out which of the two looked-up bytes to use
// at each location, all values in expanded are compared to 31.  Using
// vsel, values higher than 31 use the results from the upper 32 bytes of
// the lookup operation, while values less than or equal to 31 use the
// lower 32 bytes of the lookup operation.
//
// Note: it's tempting to use a xxpermx,xxpermx,vor sequence here on
// Power10 (or later), but experiments doing so on Power10 yielded a slight
// performance drop, perhaps due to the need for xxpermx instruction
// prefixes.

#define ENCODE_CORE                                                        \
    __ xxperm(input->to_vsr(), input->to_vsr(), expand_permute);           \
    __ vsrb(rshift, input, expand_rshift);                                 \
    __ vand(rshift, rshift, expand_rshift_mask);                           \
    __ vslo(lshift, input, vec_8s);                                        \
    __ vslb(lshift, lshift, expand_lshift);                                \
    __ vand(lshift, lshift, expand_lshift_mask);                           \
    __ vor(expanded, rshift, lshift);                                      \
    __ vperm(encoded_00_31, vec_base64_00_15, vec_base64_16_31, expanded); \
    __ vperm(encoded_32_63, vec_base64_32_47, vec_base64_48_63, expanded); \
    __ vcmpgtub(gt_31, expanded, vec_31s);                                 \
    __ vsel(expanded, encoded_00_31, encoded_32_63, gt_31);

// Intrinsic function prototype in Base64.java:
// private void encodeBlock(byte[] src, int sp, int sl, byte[] dst, int dp, boolean isURL) {

  address generate_base64_encodeBlock() {
    __ align(CodeEntryAlignment);
    StubGenStubId stub_id = StubGenStubId::base64_encodeBlock_id;
    StubCodeMark mark(this, stub_id);
    address start   = __ function_entry();

    typedef struct {
      unsigned char expand_permute_val[16];
      unsigned char expand_rshift_val[16];
      unsigned char expand_rshift_mask_val[16];
      unsigned char expand_lshift_val[16];
      unsigned char expand_lshift_mask_val[16];
      unsigned char base64_00_15_val[16];
      unsigned char base64_16_31_val[16];
      unsigned char base64_32_47_val[16];
      unsigned char base64_48_63_val[16];
      unsigned char base64_48_63_URL_val[16];
    } constant_block;

    alignas(16) static const constant_block const_block = {
      .expand_permute_val = {
        ARRAY_TO_LXV_ORDER(
        0,  4,  5,  6,
        0,  7,  8,  9,
        0, 10, 11, 12,
        0, 13, 14, 15 ) },

      .expand_rshift_val = {
        ARRAY_TO_LXV_ORDER(
        0, 6, 4, 2,
        0, 6, 4, 2,
        0, 6, 4, 2,
        0, 6, 4, 2 ) },

      .expand_rshift_mask_val = {
        ARRAY_TO_LXV_ORDER(
        0b00000000, 0b00000011, 0b00001111, 0b00111111,
        0b00000000, 0b00000011, 0b00001111, 0b00111111,
        0b00000000, 0b00000011, 0b00001111, 0b00111111,
        0b00000000, 0b00000011, 0b00001111, 0b00111111 ) },

      .expand_lshift_val = {
        ARRAY_TO_LXV_ORDER(
        0, 2, 4, 0,
        0, 2, 4, 0,
        0, 2, 4, 0,
        0, 2, 4, 0 ) },

      .expand_lshift_mask_val = {
        ARRAY_TO_LXV_ORDER(
        0b00111111, 0b00111100, 0b00110000, 0b00000000,
        0b00111111, 0b00111100, 0b00110000, 0b00000000,
        0b00111111, 0b00111100, 0b00110000, 0b00000000,
        0b00111111, 0b00111100, 0b00110000, 0b00000000 ) },

      .base64_00_15_val = {
        ARRAY_TO_LXV_ORDER(
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P' ) },

      .base64_16_31_val = {
        ARRAY_TO_LXV_ORDER(
        'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f' ) },

      .base64_32_47_val = {
        ARRAY_TO_LXV_ORDER(
        'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v' ) },

      .base64_48_63_val = {
        ARRAY_TO_LXV_ORDER(
        'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/' ) },

      .base64_48_63_URL_val = {
        ARRAY_TO_LXV_ORDER(
        'w','x','y','z','0','1','2','3','4','5','6','7','8','9','-','_' ) }
    };

    // Number of bytes to process in each pass through the main loop.
    // 12 of the 16 bytes from each lxv are encoded to 16 Base64 bytes.
    const unsigned block_size = 12;

    // According to the ELF V2 ABI, registers r3-r12 are volatile and available for use without save/restore
    Register src       = R3_ARG1; // source starting address of Base64 characters
    Register sp        = R4_ARG2; // source starting position
    Register sl        = R5_ARG3; // total source length of the Base64 characters to be processed
    Register dst       = R6_ARG4; // destination address
    Register dp        = R7_ARG5; // destination starting position
    Register isURL     = R8_ARG6; // boolean, if non-zero indicates use of RFC 4648 base64url encoding

    // Local variables
    Register const_ptr     = R12; // used for loading constants (reuses isURL's register)
    Register tmp_reg       = R9;  // used for speeding up load_constant()

    Register size           = R9;  // number of bytes to process (reuses tmp_reg's register)
    Register blocked_size   = R10; // number of bytes to process a block at a time
    Register block_modulo   = R12; // == block_size (reuse const_ptr)
    Register remaining      = R12; // bytes remaining to process after the blocks are completed (reuse block_modulo's reg)
    Register in             = R4;  // current input (source) pointer (reuse sp's register)
    Register num_blocks     = R11; // number of blocks to be processed by the loop
    Register out            = R8;  // current output (destination) pointer (reuse const_ptr's register)
    Register three          = R9;  // constant divisor (reuse size's register)
    Register bytes_to_write = R10; // number of bytes to write with the stxvl instr (reused blocked_size's register)
    Register tmp1           = R7;  // temp register for lxvl length (reuse dp's register)
    Register modulo_chars   = R7;  // number of bytes written during the final write % 4 (reuse tmp1's register)
    Register pad_char       = R6;  // literal '=' (reuse dst's register)

    // Volatile VSRS are 0..13, 32..51 (VR0..VR13)
    // VR Constants
    VectorRegister  vec_8s             = VR0;
    VectorRegister  vec_31s            = VR1;
    VectorRegister  vec_base64_00_15   = VR2;
    VectorRegister  vec_base64_16_31   = VR3;
    VectorRegister  vec_base64_32_47   = VR4;
    VectorRegister  vec_base64_48_63   = VR5;
    VectorRegister  expand_rshift      = VR6;
    VectorRegister  expand_rshift_mask = VR7;
    VectorRegister  expand_lshift      = VR8;
    VectorRegister  expand_lshift_mask = VR9;

    // VR variables for expand
    VectorRegister  input              = VR10;
    VectorRegister  rshift             = VR11;
    VectorRegister  lshift             = VR12;
    VectorRegister  expanded           = VR13;

    // VR variables for lookup
    VectorRegister  encoded_00_31      = VR10; // (reuse input)
    VectorRegister  encoded_32_63      = VR11; // (reuse rshift)
    VectorRegister  gt_31              = VR12; // (reuse lshift)

    // VSR Constants
    VectorSRegister expand_permute     = VSR0;

    Label not_URL, calculate_size, calculate_blocked_size, skip_loop;
    Label loop_start, le_16_to_write, no_pad, one_pad_char;

    // The upper 32 bits of the non-pointer parameter registers are not
    // guaranteed to be zero, so mask off those upper bits.
    __ clrldi(sp, sp, 32);
    __ clrldi(sl, sl, 32);
    __ clrldi(dp, dp, 32);
    __ clrldi(isURL, isURL, 32);

    // load up the constants
    __ load_const_optimized(const_ptr, (address)&const_block, tmp_reg);
    __ lxv(expand_permute,               BLK_OFFSETOF(expand_permute_val),     const_ptr);
    __ lxv(expand_rshift->to_vsr(),      BLK_OFFSETOF(expand_rshift_val),      const_ptr);
    __ lxv(expand_rshift_mask->to_vsr(), BLK_OFFSETOF(expand_rshift_mask_val), const_ptr);
    __ lxv(expand_lshift->to_vsr(),      BLK_OFFSETOF(expand_lshift_val),      const_ptr);
    __ lxv(expand_lshift_mask->to_vsr(), BLK_OFFSETOF(expand_lshift_mask_val), const_ptr);
    __ lxv(vec_base64_00_15->to_vsr(),   BLK_OFFSETOF(base64_00_15_val),       const_ptr);
    __ lxv(vec_base64_16_31->to_vsr(),   BLK_OFFSETOF(base64_16_31_val),       const_ptr);
    __ lxv(vec_base64_32_47->to_vsr(),   BLK_OFFSETOF(base64_32_47_val),       const_ptr);

    // Splat the constants that can use xxspltib
    __ xxspltib(vec_8s->to_vsr(), 8);
    __ xxspltib(vec_31s->to_vsr(), 31);


    // Use a different translation lookup table depending on the
    // setting of isURL
    __ cmpdi(CR0, isURL, 0);
    __ beq(CR0, not_URL);
    __ lxv(vec_base64_48_63->to_vsr(), BLK_OFFSETOF(base64_48_63_URL_val), const_ptr);
    __ b(calculate_size);

    __ bind(not_URL);
    __ lxv(vec_base64_48_63->to_vsr(), BLK_OFFSETOF(base64_48_63_val), const_ptr);

    __ bind(calculate_size);

    // size = sl - sp - 4 (*)
    // (*) Don't process the last four bytes in the main loop because
    // we don't want the lxv instruction to read past the end of the src
    // data, in case those four bytes are on the start of an unmapped or
    // otherwise inaccessible page.
    //
    __ sub(size, sl, sp);
    __ subi(size, size, 4);
    __ cmpdi(CR7, size, block_size);
    __ bgt(CR7, calculate_blocked_size);
    __ mr(remaining, size);
    // Add the 4 back into remaining again
    __ addi(remaining, remaining, 4);
    // make "in" point to the beginning of the source data: in = src + sp
    __ add(in, src, sp);
    // out = dst + dp
    __ add(out, dst, dp);
    __ b(skip_loop);

    __ bind(calculate_blocked_size);
    __ li(block_modulo, block_size);
    // num_blocks = size / block_modulo
    __ divwu(num_blocks, size, block_modulo);
    // blocked_size = num_blocks * size
    __ mullw(blocked_size, num_blocks, block_modulo);
    // remaining = size - blocked_size
    __ sub(remaining, size, blocked_size);
    __ mtctr(num_blocks);

    // Add the 4 back in to remaining again
    __ addi(remaining, remaining, 4);

    // make "in" point to the beginning of the source data: in = src + sp
    __ add(in, src, sp);

    // out = dst + dp
    __ add(out, dst, dp);

    __ align(32);
    __ bind(loop_start);

    __ lxv(input->to_vsr(), 0, in);

    ENCODE_CORE

    __ stxv(expanded->to_vsr(), 0, out);
    __ addi(in, in, 12);
    __ addi(out, out, 16);
    __ bdnz(loop_start);

    __ bind(skip_loop);

    // When there are less than 16 bytes left, we need to be careful not to
    // read beyond the end of the src buffer, which might be in an unmapped
    // page.
    // Load the remaining bytes using lxvl.
    __ rldicr(tmp1, remaining, 56, 7);
    __ lxvl(input->to_vsr(), in, tmp1);

    ENCODE_CORE

    // bytes_to_write = ((remaining * 4) + 2) / 3
    __ li(three, 3);
    __ rlwinm(bytes_to_write, remaining, 2, 0, 29); // remaining * 4
    __ addi(bytes_to_write, bytes_to_write, 2);
    __ divwu(bytes_to_write, bytes_to_write, three);

    __ cmpwi(CR7, bytes_to_write, 16);
    __ ble_predict_taken(CR7, le_16_to_write);
    __ stxv(expanded->to_vsr(), 0, out);

    // We've processed 12 of the 13-15 data bytes, so advance the pointers,
    // and do one final pass for the remaining 1-3 bytes.
    __ addi(in, in, 12);
    __ addi(out, out, 16);
    __ subi(remaining, remaining, 12);
    __ subi(bytes_to_write, bytes_to_write, 16);
    __ rldicr(tmp1, bytes_to_write, 56, 7);
    __ lxvl(input->to_vsr(), in, tmp1);

    ENCODE_CORE

    __ bind(le_16_to_write);
    // shift bytes_to_write into the upper 8 bits of t1 for use by stxvl
    __ rldicr(tmp1, bytes_to_write, 56, 7);
    __ stxvl(expanded->to_vsr(), out, tmp1);
    __ add(out, out, bytes_to_write);

    __ li(pad_char, '=');
    __ rlwinm_(modulo_chars, bytes_to_write, 0, 30, 31); // bytes_to_write % 4, set CR0
    // Examples:
    //    remaining  bytes_to_write  modulo_chars  num pad chars
    //        0            0               0            0
    //        1            2               2            2
    //        2            3               3            1
    //        3            4               0            0
    //        4            6               2            2
    //        5            7               3            1
    //        ...
    //       12           16               0            0
    //       13           18               2            2
    //       14           19               3            1
    //       15           20               0            0
    __ beq(CR0, no_pad);
    __ cmpwi(CR7, modulo_chars, 3);
    __ beq(CR7, one_pad_char);

    // two pad chars
    __ stb(pad_char, out);
    __ addi(out, out, 1);

    __ bind(one_pad_char);
    __ stb(pad_char, out);

    __ bind(no_pad);

    __ blr();
    return start;
  }

#endif // VM_LITTLE_ENDIAN

void generate_lookup_secondary_supers_table_stub() {
    StubGenStubId stub_id = StubGenStubId::lookup_secondary_supers_table_id;
    StubCodeMark mark(this, stub_id);

    const Register
      r_super_klass  = R4_ARG2,
      r_array_base   = R3_ARG1,
      r_array_length = R7_ARG5,
      r_array_index  = R6_ARG4,
      r_sub_klass    = R5_ARG3,
      r_bitmap       = R11_scratch1,
      result         = R8_ARG6;

    for (int slot = 0; slot < Klass::SECONDARY_SUPERS_TABLE_SIZE; slot++) {
      StubRoutines::_lookup_secondary_supers_table_stubs[slot] = __ pc();
      __ lookup_secondary_supers_table_const(r_sub_klass, r_super_klass,
                                             r_array_base, r_array_length, r_array_index,
                                             r_bitmap, result, slot);
      __ blr();
    }
  }

  // Slow path implementation for UseSecondarySupersTable.
  address generate_lookup_secondary_supers_table_slow_path_stub() {
    StubGenStubId stub_id = StubGenStubId::lookup_secondary_supers_table_slow_path_id;
    StubCodeMark mark(this, stub_id);

    address start = __ pc();
    const Register
      r_super_klass  = R4_ARG2,
      r_array_base   = R3_ARG1,
      temp1          = R7_ARG5,
      r_array_index  = R6_ARG4,
      r_bitmap       = R11_scratch1,
      result         = R8_ARG6;

    __ lookup_secondary_supers_table_slow_path(r_super_klass, r_array_base, r_array_index, r_bitmap, result, temp1);
    __ blr();

    return start;
  }

  address generate_cont_thaw(StubGenStubId stub_id) {
    if (!Continuations::enabled()) return nullptr;

    Continuation::thaw_kind kind;
    bool return_barrier;
    bool return_barrier_exception;

    switch (stub_id) {
    case cont_thaw_id:
      kind = Continuation::thaw_top;
      return_barrier = false;
      return_barrier_exception = false;
      break;
    case cont_returnBarrier_id:
      kind = Continuation::thaw_return_barrier;
      return_barrier = true;
      return_barrier_exception = false;
      break;
    case cont_returnBarrierExc_id:
      kind = Continuation::thaw_return_barrier_exception;
      return_barrier = true;
      return_barrier_exception = true;
      break;
    default:
      ShouldNotReachHere();
    }
    StubCodeMark mark(this, stub_id);

    Register tmp1 = R10_ARG8;
    Register tmp2 = R9_ARG7;
    Register tmp3 = R8_ARG6;
    Register nvtmp = R15_esp;   // nonvolatile tmp register
    FloatRegister nvftmp = F20; // nonvolatile fp tmp register

    address start = __ pc();

    if (kind == Continuation::thaw_top) {
      __ clobber_nonvolatile_registers(); // Except R16_thread and R29_TOC
    }

    if (return_barrier) {
      __ mr(nvtmp, R3_RET); __ fmr(nvftmp, F1_RET); // preserve possible return value from a method returning to the return barrier
      DEBUG_ONLY(__ ld_ptr(tmp1, _abi0(callers_sp), R1_SP);)
      __ ld_ptr(R1_SP, JavaThread::cont_entry_offset(), R16_thread);
#ifdef ASSERT
      __ ld_ptr(tmp2, _abi0(callers_sp), R1_SP);
      __ cmpd(CR0, tmp1, tmp2);
      __ asm_assert_eq(FILE_AND_LINE ": callers sp is corrupt");
#endif
    }
#ifdef ASSERT
    __ ld_ptr(tmp1, JavaThread::cont_entry_offset(), R16_thread);
    __ cmpd(CR0, R1_SP, tmp1);
    __ asm_assert_eq(FILE_AND_LINE ": incorrect R1_SP");
#endif

    __ li(R4_ARG2, return_barrier ? 1 : 0);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, Continuation::prepare_thaw), R16_thread, R4_ARG2);

#ifdef ASSERT
    DEBUG_ONLY(__ ld_ptr(tmp1, JavaThread::cont_entry_offset(), R16_thread));
    DEBUG_ONLY(__ cmpd(CR0, R1_SP, tmp1));
    __ asm_assert_eq(FILE_AND_LINE ": incorrect R1_SP");
#endif

    // R3_RET contains the size of the frames to thaw, 0 if overflow or no more frames
    Label thaw_success;
    __ cmpdi(CR0, R3_RET, 0);
    __ bne(CR0, thaw_success);
    __ load_const_optimized(tmp1, (SharedRuntime::throw_StackOverflowError_entry()), R0);
    __ mtctr(tmp1); __ bctr();
    __ bind(thaw_success);

    __ addi(R3_RET, R3_RET, frame::native_abi_reg_args_size); // Large abi required for C++ calls.
    __ neg(R3_RET, R3_RET);
    // align down resulting in a smaller negative offset
    __ clrrdi(R3_RET, R3_RET, exact_log2(frame::alignment_in_bytes));
    DEBUG_ONLY(__ mr(tmp1, R1_SP);)
    __ resize_frame(R3_RET, tmp2);  // make room for the thawed frames

    __ li(R4_ARG2, kind);
    __ call_VM_leaf(Continuation::thaw_entry(), R16_thread, R4_ARG2);
    __ mr(R1_SP, R3_RET); // R3_RET contains the SP of the thawed top frame

    if (return_barrier) {
      // we're now in the caller of the frame that returned to the barrier
      __ mr(R3_RET, nvtmp); __ fmr(F1_RET, nvftmp); // restore return value (no safepoint in the call to thaw, so even an oop return value should be OK)
    } else {
      // we're now on the yield frame (which is in an address above us b/c rsp has been pushed down)
      __ li(R3_RET, 0); // return 0 (success) from doYield
    }

    if (return_barrier_exception) {
      Register ex_pc = R17_tos;   // nonvolatile register
      __ ld(ex_pc, _abi0(lr), R1_SP); // LR
      __ mr(nvtmp, R3_RET); // save return value containing the exception oop
      // The thawed top frame has got a frame::java_abi. This is not sufficient for the runtime call.
      __ push_frame_reg_args(0, tmp1);
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::exception_handler_for_return_address), R16_thread, ex_pc);
      __ mtlr(R3_RET); // the exception handler
      __ pop_frame();
      // See OptoRuntime::generate_exception_blob for register arguments
      __ mr(R3_ARG1, nvtmp); // exception oop
      __ mr(R4_ARG2, ex_pc); // exception pc
    } else {
      // We're "returning" into the topmost thawed frame; see Thaw::push_return_frame
      __ ld(R0, _abi0(lr), R1_SP); // LR
      __ mtlr(R0);
    }
    __ blr();

    return start;
  }

  address generate_cont_thaw() {
    return generate_cont_thaw(StubGenStubId::cont_thaw_id);
  }

  // TODO: will probably need multiple return barriers depending on return type

  address generate_cont_returnBarrier() {
    return generate_cont_thaw(StubGenStubId::cont_returnBarrier_id);
  }

  address generate_cont_returnBarrier_exception() {
    return generate_cont_thaw(StubGenStubId::cont_returnBarrierExc_id);
  }

  address generate_cont_preempt_stub() {
    if (!Continuations::enabled()) return nullptr;
    StubGenStubId stub_id = StubGenStubId::cont_preempt_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    __ clobber_nonvolatile_registers(); // Except R16_thread and R29_TOC

    __ reset_last_Java_frame(false /*check_last_java_sp*/);

    // Set sp to enterSpecial frame, i.e. remove all frames copied into the heap.
    __ ld_ptr(R1_SP, JavaThread::cont_entry_offset(), R16_thread);

    Label preemption_cancelled;
    __ lbz(R11_scratch1, in_bytes(JavaThread::preemption_cancelled_offset()), R16_thread);
    __ cmpwi(CR0, R11_scratch1, 0);
    __ bne(CR0, preemption_cancelled);

    // Remove enterSpecial frame from the stack and return to Continuation.run() to unmount.
    SharedRuntime::continuation_enter_cleanup(_masm);
    __ pop_frame();
    __ restore_LR(R11_scratch1);
    __ blr();

    // We acquired the monitor after freezing the frames so call thaw to continue execution.
    __ bind(preemption_cancelled);
    __ li(R11_scratch1, 0); // false
    __ stb(R11_scratch1, in_bytes(JavaThread::preemption_cancelled_offset()), R16_thread);
    int simm16_offs = __ load_const_optimized(R11_scratch1, ContinuationEntry::thaw_call_pc_address(), R0, true);
    __ ld(R11_scratch1, simm16_offs, R11_scratch1);
    __ mtctr(R11_scratch1);
    __ bctr();

    return start;
  }

  // exception handler for upcall stubs
  address generate_upcall_stub_exception_handler() {
    StubGenStubId stub_id = StubGenStubId::upcall_stub_exception_handler_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    // Native caller has no idea how to handle exceptions,
    // so we just crash here. Up to callee to catch exceptions.
    __ verify_oop(R3_ARG1);
    __ load_const_optimized(R12_scratch2, CAST_FROM_FN_PTR(uint64_t, UpcallLinker::handle_uncaught_exception), R0);
    __ call_c(R12_scratch2);
    __ should_not_reach_here();

    return start;
  }

  // load Method* target of MethodHandle
  // R3_ARG1 = jobject receiver
  // R19_method = result Method*
  address generate_upcall_stub_load_target() {

    StubGenStubId stub_id = StubGenStubId::upcall_stub_load_target_id;
    StubCodeMark mark(this, stub_id);
    address start = __ pc();

    __ resolve_global_jobject(R3_ARG1, R22_tmp2, R23_tmp3, MacroAssembler::PRESERVATION_FRAME_LR_GP_FP_REGS);
    // Load target method from receiver
    __ load_heap_oop(R19_method, java_lang_invoke_MethodHandle::form_offset(), R3_ARG1,
                     R22_tmp2, R23_tmp3, MacroAssembler::PRESERVATION_FRAME_LR_GP_FP_REGS, IS_NOT_NULL);
    __ load_heap_oop(R19_method, java_lang_invoke_LambdaForm::vmentry_offset(), R19_method,
                     R22_tmp2, R23_tmp3, MacroAssembler::PRESERVATION_FRAME_LR_GP_FP_REGS, IS_NOT_NULL);
    __ load_heap_oop(R19_method, java_lang_invoke_MemberName::method_offset(), R19_method,
                     R22_tmp2, R23_tmp3, MacroAssembler::PRESERVATION_FRAME_LR_GP_FP_REGS, IS_NOT_NULL);
    __ ld(R19_method, java_lang_invoke_ResolvedMethodName::vmtarget_offset(), R19_method);
    __ std(R19_method, in_bytes(JavaThread::callee_target_offset()), R16_thread); // just in case callee is deoptimized

    __ blr();

    return start;
  }

  // Initialization
  void generate_initial_stubs() {
    // Generates all stubs and initializes the entry points

    // Entry points that exist in all platforms.
    // Note: This is code that could be shared among different platforms - however the
    // benefit seems to be smaller than the disadvantage of having a
    // much more complicated generator structure. See also comment in
    // stubRoutines.hpp.

    StubRoutines::_forward_exception_entry          = generate_forward_exception();
    StubRoutines::_call_stub_entry                  = generate_call_stub(StubRoutines::_call_stub_return_address);
    StubRoutines::_catch_exception_entry            = generate_catch_exception();

    if (UnsafeMemoryAccess::_table == nullptr) {
      UnsafeMemoryAccess::create_table(8 + 4); // 8 for copyMemory; 4 for setMemory
    }

    // CRC32 Intrinsics.
    if (UseCRC32Intrinsics) {
      StubRoutines::_crc_table_adr = StubRoutines::ppc::generate_crc_constants(REVERSE_CRC32_POLY);
      StubRoutines::_updateBytesCRC32 = generate_CRC32_updateBytes(StubGenStubId::updateBytesCRC32_id);
    }

    // CRC32C Intrinsics.
    if (UseCRC32CIntrinsics) {
      StubRoutines::_crc32c_table_addr = StubRoutines::ppc::generate_crc_constants(REVERSE_CRC32C_POLY);
      StubRoutines::_updateBytesCRC32C = generate_CRC32_updateBytes(StubGenStubId::updateBytesCRC32C_id);
    }

    if (VM_Version::supports_float16()) {
      // For results consistency both intrinsics should be enabled.
      StubRoutines::_hf2f = generate_float16ToFloat();
      StubRoutines::_f2hf = generate_floatToFloat16();
    }
  }

  void generate_continuation_stubs() {
    // Continuation stubs:
    StubRoutines::_cont_thaw          = generate_cont_thaw();
    StubRoutines::_cont_returnBarrier = generate_cont_returnBarrier();
    StubRoutines::_cont_returnBarrierExc = generate_cont_returnBarrier_exception();
    StubRoutines::_cont_preempt_stub  = generate_cont_preempt_stub();
  }

  void generate_final_stubs() {
    // Generates all stubs and initializes the entry points

    // support for verify_oop (must happen after universe_init)
    StubRoutines::_verify_oop_subroutine_entry             = generate_verify_oop();

    // nmethod entry barriers for concurrent class unloading
    StubRoutines::_method_entry_barrier = generate_method_entry_barrier();

    // arraycopy stubs used by compilers
    generate_arraycopy_stubs();

#ifdef COMPILER2
    if (UseSecondarySupersTable) {
      StubRoutines::_lookup_secondary_supers_table_slow_path_stub = generate_lookup_secondary_supers_table_slow_path_stub();
      if (!InlineSecondarySupersTest) {
        generate_lookup_secondary_supers_table_stub();
      }
    }
#endif // COMPILER2

    StubRoutines::_upcall_stub_exception_handler = generate_upcall_stub_exception_handler();
    StubRoutines::_upcall_stub_load_target = generate_upcall_stub_load_target();
  }

  void generate_compiler_stubs() {
#if COMPILER2_OR_JVMCI

#ifdef COMPILER2
    if (UseMultiplyToLenIntrinsic) {
      StubRoutines::_multiplyToLen = generate_multiplyToLen();
    }
    if (UseSquareToLenIntrinsic) {
      StubRoutines::_squareToLen = generate_squareToLen();
    }
    if (UseMulAddIntrinsic) {
      StubRoutines::_mulAdd = generate_mulAdd();
    }
    if (UseMontgomeryMultiplyIntrinsic) {
      StubRoutines::_montgomeryMultiply
        = CAST_FROM_FN_PTR(address, SharedRuntime::montgomery_multiply);
    }
    if (UseMontgomerySquareIntrinsic) {
      StubRoutines::_montgomerySquare
        = CAST_FROM_FN_PTR(address, SharedRuntime::montgomery_square);
    }
#endif

    // data cache line writeback
    if (VM_Version::supports_data_cache_line_flush()) {
      StubRoutines::_data_cache_writeback = generate_data_cache_writeback();
      StubRoutines::_data_cache_writeback_sync = generate_data_cache_writeback_sync();
    }

    if (UseGHASHIntrinsics) {
      StubRoutines::_ghash_processBlocks = generate_ghash_processBlocks();
    }

    if (UseAESIntrinsics) {
      StubRoutines::_aescrypt_encryptBlock = generate_aescrypt_encryptBlock();
      StubRoutines::_aescrypt_decryptBlock = generate_aescrypt_decryptBlock();
    }

    if (UseSHA256Intrinsics) {
      StubRoutines::_sha256_implCompress   = generate_sha256_implCompress(StubGenStubId::sha256_implCompress_id);
      StubRoutines::_sha256_implCompressMB = generate_sha256_implCompress(StubGenStubId::sha256_implCompressMB_id);
    }
    if (UseSHA512Intrinsics) {
      StubRoutines::_sha512_implCompress   = generate_sha512_implCompress(StubGenStubId::sha512_implCompress_id);
      StubRoutines::_sha512_implCompressMB = generate_sha512_implCompress(StubGenStubId::sha512_implCompressMB_id);
    }

#ifdef VM_LITTLE_ENDIAN
    // Currently supported on PPC64LE only
    if (UseBASE64Intrinsics) {
      StubRoutines::_base64_decodeBlock = generate_base64_decodeBlock();
      StubRoutines::_base64_encodeBlock = generate_base64_encodeBlock();
    }
#endif
#endif // COMPILER2_OR_JVMCI
  }

 public:
  StubGenerator(CodeBuffer* code, StubGenBlobId blob_id) : StubCodeGenerator(code, blob_id) {
    switch(blob_id) {
    case initial_id:
      generate_initial_stubs();
      break;
     case continuation_id:
      generate_continuation_stubs();
      break;
    case compiler_id:
      generate_compiler_stubs();
      break;
    case final_id:
      generate_final_stubs();
      break;
    default:
      fatal("unexpected blob id: %d", blob_id);
      break;
    };
  }
};

void StubGenerator_generate(CodeBuffer* code, StubGenBlobId blob_id) {
  StubGenerator g(code, blob_id);
}

