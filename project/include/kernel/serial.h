/* 16550 UART logging on COM1 (port 0x3F8).
 *
 * This is the only output channel that survives ExitBootServices, which is why
 * the research document concluded that serial logging and GDB are not
 * alternatives but complements. Every step of the flow prints one line, so the
 * point of failure is visible immediately in the QEMU -serial stdio output.
 *
 * The implementation lives in kernel/dev/serial.c and is compiled into *both*
 * halves of the project - two objects from one source, one per ABI. It is the
 * only file that is, and the reason is that the bootloader's own console dies
 * at ExitBootServices while this keeps working: it talks to the UART through
 * port I/O, owing nothing to the firmware.
 *
 * The header lives under include/kernel/ rather than next to its .c file so
 * that both halves reach it the same way, as <kernel/serial.h>.
 */
#ifndef NYRF_SERIAL_H
#define NYRF_SERIAL_H

#include <stdint.h>

/* Configures the UART for 115200 8N1. Idempotent, and called once by each
 * half - the kernel cannot assume what state it inherited. */
void serial_init(void);

/* Blocking, one byte at a time; '\n' is expanded to CR LF. There is no
 * buffering anywhere, which is the point: a line is on the wire before the
 * next statement runs, so the log never loses the last thing that happened
 * before a hang. */
void serial_putc(char c);
void serial_puts(const char *s);

/* Fixed sixteen digits, 0x-prefixed, leading zeros kept - so addresses line up
 * in a column in the log. */
void serial_put_hex(uint64_t value);
void serial_put_dec(uint64_t value);

/* Tiny printf substitute. Supported specifiers: %s, %x (0x-prefixed 64-bit),
 * %u (unsigned decimal), %c, %%. Anything else is printed verbatim.
 *
 * No width, precision or padding. Note that %x and %u each consume a full
 * uint64_t: varargs carry no type information and the compiler cannot check
 * these format strings, so every call site casts its argument explicitly.
 * Passing a 32-bit value unpromoted prints garbage in the high half. */
void serial_printf(const char *fmt, ...);

#endif /* NYRF_SERIAL_H */
