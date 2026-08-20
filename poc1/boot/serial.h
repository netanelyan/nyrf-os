/* 16550 UART logging on COM1 (port 0x3F8).
 *
 * This is the only output channel that survives ExitBootServices, which is why
 * the research document concluded that serial logging and GDB are not
 * alternatives but complements. Every step of the flow prints one line, so the
 * point of failure is visible immediately in the QEMU -serial stdio output.
 */
#ifndef NYRF_SERIAL_H
#define NYRF_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex(uint64_t value);
void serial_put_dec(uint64_t value);

/* Tiny printf substitute. Supported specifiers: %s, %x (0x-prefixed 64-bit),
 * %u (unsigned decimal), %c, %%. Anything else is printed verbatim. */
void serial_printf(const char *fmt, ...);

#endif /* NYRF_SERIAL_H */
