/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "bflb_l1c.h"
#include "bflb_mtimer.h"
#include "esp_heap_caps.h"
#include "mm.h"
#include "platform_api_extension.h"
#include "platform_api_vmcore.h"

int bh_platform_init(void) { return 0; }
void bh_platform_destroy(void) {}
void *os_malloc(unsigned size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void *os_realloc(void *memory, unsigned size) { return heap_caps_realloc(memory, size, MALLOC_CAP_SPIRAM); }
void os_free(void *memory) { heap_caps_free(memory); }
int os_vprintf(const char *format, va_list args) { return vprintf(format, args); }
int os_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}
uint64 os_time_get_boot_us(void) { return bflb_mtimer_get_time_us(); }
uint64 os_time_thread_cputime_us(void) { return os_time_get_boot_us(); }
korp_tid os_self_thread(void) { return xTaskGetCurrentTaskHandle(); }
uint8 *os_thread_get_stack_boundary(void) {
    TaskStatus_t status;
    vTaskGetInfo(NULL, &status, pdFALSE, eInvalid);
    return (uint8 *)status.pxStackBase;
}
void os_thread_jit_write_protect_np(bool enabled) { (void)enabled; }
int os_thread_env_init(void) { return 0; }
void os_thread_env_destroy(void) {}
bool os_thread_env_inited(void) { return true; }
int os_mutex_init(korp_mutex *mutex) {
    *mutex = xSemaphoreCreateRecursiveMutex();
    return *mutex ? 0 : -1;
}
int os_recursive_mutex_init(korp_mutex *mutex) { return os_mutex_init(mutex); }
int os_mutex_destroy(korp_mutex *mutex) {
    if (*mutex) vSemaphoreDelete(*mutex);
    *mutex = NULL;
    return 0;
}
int os_mutex_lock(korp_mutex *mutex) {
    /* WAMR locks its static runtime mutex before bh_platform_init(). */
    if (!*mutex) {
        taskENTER_CRITICAL();
        if (!*mutex) *mutex = xSemaphoreCreateRecursiveMutex();
        taskEXIT_CRITICAL();
        if (!*mutex) return -1;
    }
    return xSemaphoreTakeRecursive(*mutex, portMAX_DELAY) == pdTRUE ? 0 : -1;
}
int os_mutex_unlock(korp_mutex *mutex) { return xSemaphoreGiveRecursive(*mutex) == pdTRUE ? 0 : -1; }
int os_usleep(uint32 usec) {
    uint64_t ticks = ((uint64_t)usec * configTICK_RATE_HZ + 999999U) / 1000000U;
    vTaskDelay(ticks > portMAX_DELAY - 1U ? portMAX_DELAY - 1U : (TickType_t)ticks);
    return 0;
}
void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file) {
    (void)hint;
    (void)prot;
    (void)file;
    if (flags & MMAP_MAP_FIXED) return NULL;
    void *memory = heap_caps_aligned_alloc(64U, size, MALLOC_CAP_SPIRAM);
    /* Anonymous mappings must start at zero, including Guest .bss. */
    if (memory) memset(memory, 0, size);
    return memory;
}
void os_munmap(void *memory, size_t size) {
    (void)size;
    heap_caps_free(memory);
}
/* The target has no per-mapping protection. WAMR must keep software bounds
 * checks enabled; this hook does not establish a hardware isolation boundary. */
int os_mprotect(void *memory, size_t size, int prot) {
    (void)memory;
    (void)size;
    (void)prot;
    return 0;
}
void *os_mremap(void *memory, size_t old_size, size_t new_size) {
    /* WAMR reserves max linear memory for PINNED_MEMORY Guests. Keep
     * in-flight Guest surface pointers valid while exposing more pages. */
    if (memory && new_size <= kmalloc_size(memory)) {
        if (new_size > old_size) memset((uint8_t *)memory + old_size, 0, new_size - old_size);
        return memory;
    }
    return os_mremap_slow(memory, old_size, new_size);
}
void os_dcache_flush(void) { bflb_l1c_dcache_clean_all(); }
void os_icache_flush(void *start, size_t size) {
    bflb_l1c_dcache_clean_range(start, size);
    __asm__ volatile("fence.i" ::: "memory");
}
