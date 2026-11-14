// SPDX-License-Identifier: Apache-2.0
#include "console.h"

#include "FreeRTOS.h"
#include "bflb_irq.h"
#include "bflb_uart.h"
#include "semphr.h"
static struct bflb_device_s *uart;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t tx_lock;
static uint8_t rx[8192];
static volatile uint32_t head, tail;
static void irq(int n, void *arg) {
    (void)n;
    (void)arg;
    int c;
    while ((c = bflb_uart_getchar(uart)) >= 0) {
        uint32_t next = (head + 1) % sizeof(rx);
        if (next != tail) {
            rx[head] = c;
            head = next;
        }
    }
    bflb_uart_int_clear(uart, UART_INTCLR_RTO | UART_INTCLR_RX_END);
}
void mpx_console_init(void) {
    uart = bflb_device_get_by_name("uart0");
    tx_lock = xSemaphoreCreateMutexStatic(&mutex_storage);
    bflb_irq_attach(uart->irq_num, irq, NULL);
    bflb_uart_rxint_mask(uart, false);
    bflb_irq_enable(uart->irq_num);
}
int mpx_console_read(void) {
    if (head == tail) return -1;
    int c = rx[tail];
    tail = (tail + 1) % sizeof(rx);
    return c;
}
void mpx_console_line(const char *s) {
    if (!uart) return;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    for (; *s; ++s) bflb_uart_putchar(uart, *s);
    bflb_uart_putchar(uart, '\n');
    xSemaphoreGive(tx_lock);
}

void mpx_console_frame(const char *header, const void *data, size_t size, const char *tail) {
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    for (; *header; ++header) bflb_uart_putchar(uart, *header);
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) bflb_uart_putchar(uart, p[i]);
    for (; *tail; ++tail) bflb_uart_putchar(uart, *tail);
    xSemaphoreGive(tx_lock);
}
