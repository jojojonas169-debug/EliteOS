/* System call entry setup and dispatch (filled in by the userland layer). */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>

void syscall_init_this(void)
{
    wrmsr(MSR_STAR, ((uint64_t)0x18 << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | (1u << 10) | (1u << 8) | (1u << 18));   /* IF DF TF AC */
}

void syscall_dispatch(struct regs *r)
{
    r->rax = (uint64_t)-38;   /* ENOSYS */
}
