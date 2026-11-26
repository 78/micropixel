// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "board.h"
#include "mm.h"
#include "mpx_board.h"
#include "task.h"

extern void __libc_init_array(void);
extern void micropixel_host_start(void *argument);

void vApplicationMallocFailedHook(void) {
    printf("[micropixel] allocation failed task=%s SRAM=%u PSRAM=%u\r\n", pcTaskGetName(NULL),
           (unsigned)kfree_size(MM_HEAP_OCRAM_0), (unsigned)kfree_size(MM_HEAP_PSRAM_0));
    abort();
}

int main(void) {
    board_init();
    /* SDK startup leaves C++ static initialization to the application. */
    __libc_init_array();
    printf("\r\nMicroPixel Desktop / " MPX_BOARD_NAME "\r\n");
    if (xTaskCreate(micropixel_host_start, "mpx_host", 6144, NULL, 4, NULL) != pdPASS) {
        vApplicationMallocFailedHook();
    }
    vTaskStartScheduler();
    for (;;) {
    }
}
