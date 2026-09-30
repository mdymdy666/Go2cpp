/*
 * Go2Cpp x86_64 context backend.
 *
 * The register layout and first-entry convention are derived from the
 * Apache-2.0 licensed coost/TBOX context implementation and the original
 * Boost.Context x86_64 layout.  This file intentionally exposes only the
 * Go2Cpp-prefixed ABI declared in detail/context_backend.hpp.
 *
 * The C++ FiberStack allocator supplies a page-aligned mmap region with a
 * PROT_NONE guard page. This assembly only switches registers and stack
 * pointers; allocation, guard protection, caching and lifetime stay in C++.
 */

#if defined(__linux__) && defined(__x86_64__)

    .text
    .globl go2cpp_context_make
    .type go2cpp_context_make,@function
go2cpp_context_make:
    # rdi = stack bottom, rsi = stack size, rdx = entry function.
    addq %rsi, %rdi
    movq %rdi, %rax
    andq $-16, %rax
    subq $56, %rax

    # Saved callee-saved registers: r12,r13,r14,r15,rbx,rbp.
    movq %rdx, 32(%rax)
    leaq go2cpp_context_entry(%rip), %rcx
    movq %rcx, 48(%rax)
    leaq go2cpp_context_end(%rip), %rcx
    movq %rcx, 40(%rax)
    ret

go2cpp_context_entry:
    # The context pointer is still in rax. SysV passes Transfer as
    # (context=rdi, data=rsi).
    movq %rax, %rdi
    pushq %rbp
    jmp *%rbx

go2cpp_context_end:
    # An entry function must return to JumpContext. Treat a violation as a
    # process exit instead of returning through an arbitrary stack address.
    xorl %edi, %edi
    call _exit@PLT
    ud2

    .size go2cpp_context_make,.-go2cpp_context_make

    .globl go2cpp_context_jump
    .type go2cpp_context_jump,@function
go2cpp_context_jump:
    # rdi = target context, rsi = data. Save the current callee-saved state.
    pushq %rbp
    pushq %rbx
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    movq %rsp, %rax
    movq %rdi, %rsp

    popq %r12
    popq %r13
    popq %r14
    popq %r15
    popq %rbx
    popq %rbp
    popq %r8

    # Return Transfer{saved_context, data} in rax/rdx.
    movq %rsi, %rdx
    jmp *%r8

    .size go2cpp_context_jump,.-go2cpp_context_jump

#else
    /* The target is only compiled on Linux x86_64. */
#endif

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
