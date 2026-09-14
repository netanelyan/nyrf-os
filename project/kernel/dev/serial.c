/* 16550 UART driver. See include/kernel/serial.h for why this file is the
 * project's lifeline rather than a convenience.
 *
 * Everything here is polled and interrupt-free, which is what makes it usable
 * from both halves of the POC and at any point in the boot: it needs no IDT,
 * no timer, no allocator and no firmware. It is the one driver that works
 * before and after ExitBootServices alike.
 */

#include <kernel/serial.h>

#include <stdarg.h>

/* The first serial port's I/O base address. Fixed by PC convention since the
 * original IBM PC, and QEMU honours it - which is why -serial stdio works
 * without any configuration on either side. */
#define COM1 0x3F8

/* 16550 register offsets from the base port.
 *
 * The UART has more registers than it has addresses, so two of them are
 * multiplexed: setting the DLAB bit in the line control register re-points
 * offsets 0 and 1 at the baud rate divisor. That is the whole reason
 * serial_init() below looks like a sequence of magic numbers - it is toggling
 * DLAB to reach a second bank of registers and back. */
#define UART_DATA        0 /* also divisor low byte when DLAB is set  */
#define UART_INT_ENABLE  1 /* also divisor high byte when DLAB is set */
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

/* Line status bit 5: the transmit holding register is empty, i.e. it is safe
 * to write another byte. Polling this is the entire flow control scheme. */
#define LSR_TX_EMPTY 0x20

/* Port I/O lives in its own address space, unreachable from C - a pointer
 * cannot name it - so these two instructions have to be written by hand.
 *
 * The constraints: "a" forces the value into AL, which is the only register
 * outb can source from. "Nd" means DX, or an immediate if the port number is a
 * constant below 256 - the N half never applies here since COM1 is 0x3F8, but
 * it is the idiomatic constraint and costs nothing.
 *
 * volatile is essential: these have no return value the compiler can see, so
 * without it they would be optimised away entirely as dead code. */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Puts the UART into 115200 8N1 with FIFOs on. Safe to call twice - which it
 * is, once by the bootloader and again by the kernel, because the kernel
 * cannot assume anything about the state it inherited. */
void serial_init(void)
{
    /* Interrupts off first, so a half-configured UART cannot raise one while
     * there is no IDT to catch it. */
    outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts, we poll        */

    /* Bit 7 is DLAB. With it set, the next two writes land on the divisor
     * rather than on the data and interrupt registers. */
    outb(COM1 + UART_LINE_CTRL, 0x80);  /* DLAB on: divisor is visible   */

    /* Baud = 115200 / divisor, so divisor 1 is the maximum rate. The high
     * byte is the second write, and zero. Fast is what we want: every byte is
     * a busy-wait, so a slower rate would cost real time in the boot path. */
    outb(COM1 + UART_DATA, 0x01);       /* divisor 1 -> 115200 baud      */
    outb(COM1 + UART_INT_ENABLE, 0x00);

    /* DLAB back off, and the line format in the low bits: 0x03 is bits 1:0 =
     * 11 for 8 data bits, bit 2 clear for one stop bit, bits 5:3 clear for no
     * parity. Conventionally written "8N1". */
    outb(COM1 + UART_LINE_CTRL, 0x03);  /* DLAB off, 8 bits, no parity   */

    /* 0xC7 = FIFOs enabled (bit 0), both queues cleared (bits 1 and 2), and a
     * 14-byte interrupt trigger level (bits 7:6). The trigger level is
     * irrelevant while polling; clearing the FIFOs is not, since it discards
     * whatever the firmware left behind. */
    outb(COM1 + UART_FIFO_CTRL, 0xC7);  /* enable and clear both FIFOs   */

    /* 0x0B = DTR (bit 0) and RTS (bit 1), the two "I am here and ready"
     * modem lines, plus OUT2 (bit 3). On a PC, OUT2 gates the UART's
     * interrupt line onto the bus - harmless while we poll, but it is what
     * every real driver sets, and leaving it out would bite later. */
    outb(COM1 + UART_MODEM_CTRL, 0x0B); /* DTR, RTS, OUT2                */
}

/* Sends one byte, blocking until the UART can take it.
 *
 * The newline translation is here rather than in the callers so that no format
 * string anywhere needs to care: a terminal wants CR LF, and a bare LF would
 * produce the staircase effect in the QEMU log. */
void serial_putc(char c)
{
    if (c == '\n') {
        serial_putc('\r');
    }
    while ((inb(COM1 + UART_LINE_STATUS) & LSR_TX_EMPTY) == 0) {
        /* wait for the transmit holding register to drain */
    }
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s != '\0') {
        serial_putc(*s++);
    }
}

/* Prints a full 16 digits, leading zeros included, and never suppresses them.
 * Fixed width is deliberate: addresses in the log line up in a column, so a
 * value that is wrong by a nibble is visible without reading the digits.
 *
 * The buffer is filled backwards because the arithmetic yields the *lowest*
 * digit first, then printed forwards. Extracting straight to the port would
 * emit the number reversed. */
void serial_put_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[16];

    serial_puts("0x");
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[value & 0xF];
        value >>= 4;
    }
    for (int i = 0; i < 16; i++) {
        serial_putc(buf[i]);
    }
}

/* Same reversal problem, same fix - but here leading zeros are suppressed,
 * which makes zero a special case: the loop below would print nothing at all
 * for it. 21 bytes because 2^64-1 is 20 digits.
 *
 * Note there is no division helper to worry about: the compiler lowers a 64-bit
 * divide by 10 into a multiply on x86_64, so this needs no libgcc. */
void serial_put_dec(uint64_t value)
{
    char buf[21];
    int i = 0;

    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

/* The whole formatter. No width, no precision, no padding - just enough to
 * print the handful of numbers and strings the boot log needs.
 *
 * Two things to know when calling it. First, varargs carry no type
 * information, so %x and %u read exactly a uint64_t off the argument list:
 * passing a uint32_t leaves the high half undefined, which is why every call
 * site in this project casts explicitly. Second, the compiler cannot check
 * these format strings, since the attribute that would enable it names a libc
 * this build has no link to - the casts are the only safety net there is.
 */
void serial_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            serial_putc(*p);
            continue;
        }
        /* *++p consumes the specifier character, so the loop's own p++ then
         * moves past it. */
        switch (*++p) {
        case 's': serial_puts(va_arg(ap, const char *));   break;
        case 'x': serial_put_hex(va_arg(ap, uint64_t));    break;
        case 'u': serial_put_dec(va_arg(ap, uint64_t));    break;
        /* %c takes an int, not a char: anything smaller is promoted to int on
         * its way through the varargs, and asking for a char would read the
         * wrong size. */
        case 'c': serial_putc((char)va_arg(ap, int));      break;
        case '%': serial_putc('%');                        break;
        /* A '%' as the last character of the string. Without this case, *++p
         * would have stepped onto the terminator and the loop would read past
         * the end of the format string. */
        case '\0': va_end(ap); return;
        /* Unknown specifier: echo it rather than swallow it, so a typo shows
         * up in the log instead of silently eating an argument. */
        default:  serial_putc('%'); serial_putc(*p);       break;
        }
    }

    va_end(ap);
}
