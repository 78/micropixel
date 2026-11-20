// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "platform_api_vmcore.h"
#include "runtime_smoke_aot.h"
#include "wasm_export.h"

static void require(bool condition, const char *operation) {
    if (!condition) {
        printf("MicroPixel runtime smoke FAIL: %s\n", operation);
        abort();
    }
}

static void check_aot(void) {
    char error[128] = {0};
    wasm_module_t module = wasm_runtime_load(smoke_aot, sizeof(smoke_aot), error, sizeof(error));
    if (!module) printf("AOT load: %s\n", error);
    require(module != NULL, "valid RV32F AOT load");

    // This single task owns WAMR. A multi-task embedder must serialize the
    // process-global option and instantiation together, and restore the option.
    bool previous = wasm_runtime_get_linear_memory_reserve_max();
    wasm_runtime_set_linear_memory_reserve_max(true);
    require(wasm_runtime_get_linear_memory_reserve_max(), "enable maximum reservation");
    wasm_module_inst_t instance = wasm_runtime_instantiate(module, 8192, 0, error, sizeof(error));
    wasm_runtime_set_linear_memory_reserve_max(previous);
    if (!instance) printf("AOT instantiate: %s\n", error);
    require(instance != NULL, "AOT instantiate with maximum reservation");
    require(wasm_runtime_get_linear_memory_reserve_max() == previous, "restore reservation option");
    wasm_exec_env_t exec = wasm_runtime_create_exec_env(instance, 8192);
    require(exec != NULL, "execution environment");
    wasm_function_inst_t add = wasm_runtime_lookup_function(instance, "f64_add");
    wasm_function_inst_t divide = wasm_runtime_lookup_function(instance, "f64_div");
    wasm_function_inst_t grow = wasm_runtime_lookup_function(instance, "grow");
    require(add && divide && grow, "Guest exports");
    wasm_val_t inputs[2] = {{.kind = WASM_F64, .of.f64 = 1.25}, {.kind = WASM_F64, .of.f64 = 2.5}};
    wasm_val_t result = {0};
    require(wasm_runtime_call_wasm_a(exec, add, 1, &result, 2, inputs), "f64 add call");
    require(result.kind == WASM_F64 && result.of.f64 == 3.75, "f64 add result");
    inputs[0].of.f64 = 7.5;
    require(wasm_runtime_call_wasm_a(exec, divide, 1, &result, 2, inputs), "f64 division call");
    require(result.kind == WASM_F64 && result.of.f64 == 3.0, "f64 division result");
    puts("AOT f64: 1.25 + 2.5 = 3.75; 7.5 / 2.5 = 3.0 PASS");

    wasm_memory_inst_t memory = wasm_runtime_get_memory(instance, 0);
    require(memory && wasm_memory_get_cur_page_count(memory) == 1 && wasm_memory_get_max_page_count(memory) == 3,
            "Guest memory limits");
    uint8_t *base = wasm_runtime_addr_app_to_native(instance, 0);
    require(base != NULL, "linear memory base");
    base[32768] = 0x5a;
    base[65535] = 0xa5;
    for (uint32_t page = 1; page < 3; ++page) {
        uint32_t cells[1] = {1};
        require(wasm_runtime_call_wasm(exec, grow, 1, cells) && cells[0] == page, "memory.grow result");
        require(wasm_runtime_addr_app_to_native(instance, 0) == base, "stable linear memory address");
        require(wasm_memory_get_cur_page_count(memory) == page + 1, "visible page count");
        require(base[32768] == 0x5a && base[65535] == 0xa5, "memory.grow preserves contents");
        for (size_t i = page * 65536U; i < (page + 1U) * 65536U; ++i)
            require(base[i] == 0, "memory.grow zeroes new page");
    }
    uint32_t cells[1] = {1};
    require(wasm_runtime_call_wasm(exec, grow, 1, cells) && cells[0] == UINT32_MAX, "memory.grow limit");
    require(wasm_memory_get_cur_page_count(memory) == 3 && wasm_runtime_addr_app_to_native(instance, 0) == base &&
                base[65535] == 0xa5,
            "failed growth preserves memory");
    puts("AOT memory.grow: stable base, preserved data, zero pages, maximum rejection PASS");
    wasm_runtime_destroy_exec_env(exec);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
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
    check_aot();
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
