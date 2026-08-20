#include "serial.h"

#include <stdarg.h>

#define COM1 0x3F8

/* 16550 register offsets from the base port. */
#define UART_DATA        0 /* also divisor low byte when DLAB is set  */
#define UART_INT_ENABLE  1 /* also divisor high byte when DLAB is set */
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

#define LSR_TX_EMPTY 0x20

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

void serial_init(void)
{
    outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts, we poll        */
    outb(COM1 + UART_LINE_CTRL, 0x80);  /* DLAB on: divisor is visible   */
    outb(COM1 + UART_DATA, 0x01);       /* divisor 1 -> 115200 baud      */
    outb(COM1 + UART_INT_ENABLE, 0x00);
    outb(COM1 + UART_LINE_CTRL, 0x03);  /* DLAB off, 8 bits, no parity   */
    outb(COM1 + UART_FIFO_CTRL, 0xC7);  /* enable and clear both FIFOs   */
    outb(COM1 + UART_MODEM_CTRL, 0x0B); /* DTR, RTS, OUT2                */
}

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

void serial_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            serial_putc(*p);
            continue;
        }
        switch (*++p) {
        case 's': serial_puts(va_arg(ap, const char *));   break;
        case 'x': serial_put_hex(va_arg(ap, uint64_t));    break;
        case 'u': serial_put_dec(va_arg(ap, uint64_t));    break;
        case 'c': serial_putc((char)va_arg(ap, int));      break;
        case '%': serial_putc('%');                        break;
        case '\0': va_end(ap); return;
        default:  serial_putc('%'); serial_putc(*p);       break;
        }
    }

    va_end(ap);
}
