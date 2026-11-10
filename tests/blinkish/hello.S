// The first x86-64 guest blinkish runs: no libc, no loader, no TLS.
// write(1, msg, len) then exit_group(0), by the x86-64 syscall numbers.
    .globl _start
    .text
_start:
    mov $1, %eax            // write
    mov $1, %edi
    lea msg(%rip), %rsi
    mov $len, %edx
    syscall
    mov $231, %eax          // exit_group
    xor %edi, %edi
    syscall
    .section .rodata
msg: .ascii "hello from x86-64\n"
    .set len, . - msg
