// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "platform_api_vmcore.h"
#include "wasm_export.h"

static void require(bool condition, const char *operation) {
    if (!condition) {
        printf("MicroPixel runtime smoke FAIL: %s\n", operation);
        abort();
    }
}

static void runtime_smoke(void *argument) {
    (void)argument;
    void *memory = heap_caps_aligned_alloc(64, 256, MALLOC_CAP_SPIRAM);
    require(memory && ((uintptr_t)memory % 64U == 0), "PSRAM alignment");
    memset(memory, 0x5a, 256);
    void *grown = heap_caps_realloc(memory, 512, MALLOC_CAP_SPIRAM);
    require(grown != NULL, "PSRAM realloc");
    for (size_t i = 0; i < 256; ++i) require(((uint8_t *)grown)[i] == 0x5a, "realloc contents");
    heap_caps_free(grown);

    korp_mutex mutex = OS_THREAD_MUTEX_INITIALIZER;
    require(os_mutex_lock(&mutex) == 0, "lazy mutex");
    require(os_mutex_lock(&mutex) == 0, "recursive mutex");
    require(os_mutex_unlock(&mutex) == 0, "recursive unlock");
    require(os_mutex_unlock(&mutex) == 0 && os_mutex_destroy(&mutex) == 0, "mutex cleanup");
    require(os_thread_get_stack_boundary() != NULL, "stack boundary");

    uint8_t *mapping = os_mmap(NULL, 4096, MMAP_PROT_READ | MMAP_PROT_WRITE, MMAP_MAP_NONE, -1);
    require(mapping != NULL, "anonymous mapping");
    for (size_t i = 0; i < 4096; ++i) require(mapping[i] == 0, "zero-filled mapping");
    os_munmap(mapping, 4096);

    RuntimeInitArgs args = {0};
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = os_malloc;
    args.mem_alloc_option.allocator.realloc_func = os_realloc;
    args.mem_alloc_option.allocator.free_func = os_free;
    require(wasm_runtime_full_init(&args), "WAMR init");
    uint8_t invalid_aot[8] = {0};
    char error[128] = {0};
    require(wasm_runtime_load(invalid_aot, sizeof(invalid_aot), error, sizeof(error)) == NULL && error[0],
            "reject invalid module");
    wasm_runtime_destroy();
    require(wasm_runtime_full_init(&args), "WAMR reinit");
    wasm_runtime_destroy();
    puts("MicroPixel FreeRTOS/WAMR runtime smoke PASS");
    vTaskDelete(NULL);
}

int main(void) {
    board_init();
    require(xTaskCreatePinnedToCore(runtime_smoke, "mpx_smoke", 8192, NULL, 3, NULL, 0) == pdPASS, "runtime task");
    vTaskStartScheduler();
    abort();
}
