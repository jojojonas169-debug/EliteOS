/* User processes (stub, replaced by the userland layer). */
#include <kernel.h>
#include <cpu.h>
#include <sched.h>
#include <dev.h>

void proc_fault(struct regs *r)
{
    panic_regs(r, "user fault");
}

void proc_exit(int code)
{
    thread_current()->proc->exited = true;
    thread_exit(code);
}

void proc_reap(thread_t *t) { UNUSED(t); }
