# SPDX-License-Identifier: Apache-2.0
"""Compile the byte-stack wrapper against a native 16/32-bit FreeRTOS interface."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TaskStackBounds(unittest.TestCase):
    def test_native_depth_bounds(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler, 'A C compiler is required for stack conversion checks')
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root/'freertos').mkdir()
            # Bypass only the wrapper's FreeRTOS include; task.h below models the
            # native API. The actual conversion is included from the source tree.
            (root/'freertos/FreeRTOS.h').write_text('#include <stddef.h>\n#include <stdint.h>\n')
            (root/'task.h').write_text("""
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY -1
#ifndef configSTACK_DEPTH_TYPE
#define configSTACK_DEPTH_TYPE uint16_t
#endif
static size_t last_depth;
static unsigned calls;
static BaseType_t xTaskCreate(TaskFunction_t f, const char *n,
    configSTACK_DEPTH_TYPE depth, void *a, UBaseType_t p, TaskHandle_t *t) {
    (void)f; (void)n; (void)a; (void)p; (void)t;
    last_depth = depth; ++calls; return pdPASS;
}
static UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 7; }
""")
            source = root/'stack.c'
            source.write_text("""
#include <assert.h>
#include <limits.h>
#include """ + '"' + str(ROOT/'common/include/freertos/task.h') + '"' + """
static BaseType_t create(uint32_t bytes, TaskHandle_t *task) {
    return xTaskCreatePinnedToCore(NULL, "test", bytes, NULL, 1, task, 0);
}
int main(void) {
    TaskHandle_t task = (void *)1;
    assert(create(1, NULL) == pdPASS && last_depth == 1);
    assert(create(4, NULL) == pdPASS && last_depth == 1);
    assert(create(5, NULL) == pdPASS && last_depth == 2);
    assert(mpx_stack_free_bytes(NULL) == 28);
    const uint64_t maximum = (configSTACK_DEPTH_TYPE)-1;
    if (maximum * sizeof(StackType_t) <= UINT32_MAX) {
        uint32_t max_bytes = (uint32_t)(maximum * sizeof(StackType_t));
        assert(create(max_bytes - 3, NULL) == pdPASS && last_depth == maximum);
        assert(create(max_bytes, NULL) == pdPASS && last_depth == maximum);
        unsigned before = calls;
        assert(create(max_bytes + 1, &task) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
        assert(task == NULL && calls == before);
        assert(create(257 * 1024, NULL) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
        assert(create(UINT32_MAX, NULL) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
        assert(calls == before);
    } else {
        assert(create(UINT32_MAX, NULL) == pdPASS && last_depth == UINT32_C(1073741824));
    }
    unsigned before = calls;
    assert(create(0, NULL) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY && calls == before);
}
""")
            for depth in ('uint16_t', 'uint32_t'):
                with self.subTest(native_depth=depth):
                    binary = root/'stack'
                    result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                             '-DconfigSTACK_DEPTH_TYPE='+depth, '-I', str(root),
                                             str(source), '-o', str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    subprocess.run([str(binary)], check=True)
