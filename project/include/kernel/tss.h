/* Task State Segment.
 *
 * The name is a fossil. On the 386 the TSS held a whole task's register state
 * and the CPU could switch tasks by swapping it; long mode dropped hardware
 * task switching entirely, and what survives is a small table of stack
 * pointers. Two kinds:
 *
 *   RSP0-2   the stack to switch to when an interrupt raises the privilege
 *            level, i.e. when something in ring 3 faults. Unused here, since
 *            there is no userspace yet.
 *   IST1-7   the Interrupt Stack Table: seven stacks a gate can name
 *            unconditionally, so that the CPU switches to a known-good stack
 *            no matter what state the interrupted code was in.
 *
 * IST is the reason this file exists now rather than alongside userspace. A
 * double fault means the CPU faulted while trying to deliver another fault,
 * and the commonest cause is a stack that is no longer usable - overflowed,
 * unmapped, or pointing somewhere absurd. Delivering that double fault on the
 * same broken stack fails again, which is a triple fault: no handler runs, the
 * machine simply resets, and the log ends mid-line with no explanation. An IST
 * stack breaks that chain, and turns the worst failure mode in the kernel into
 * a readable register dump.
 *
 * The TSS is reached through a descriptor in the GDT, which is why gdt_init()
 * has to run before tss_init().
 */
#ifndef NYRF_TSS_H
#define NYRF_TSS_H

#include <stdint.h>

/* Which IST slot the double fault gate uses.
 *
 * IST slots are numbered from 1 in the IDT gate, because 0 there means "do not
 * switch stacks at all". The array inside the TSS is indexed from 0, so this
 * constant is one greater than its own index - a discrepancy worth naming
 * rather than rediscovering. */
#define TSS_IST_DOUBLE_FAULT 1

/* Fills the TSS, installs its descriptor into the GDT, and loads it with ltr.
 * Called from idt_init() rather than from kernel_main, because the only thing
 * that currently depends on it is the IST reference in gate 8. */
void tss_init(void);

#endif /* NYRF_TSS_H */
