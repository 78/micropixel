// SPDX-License-Identifier: Apache-2.0
// No Guest SDK/Service imports: exercise the pinned AOT loader and memory hooks.
double f64_add(double left, double right) { return left + right; }
double f64_div(double left, double right) { return left / right; }
unsigned grow(unsigned pages) { return __builtin_wasm_memory_grow(0, pages); }
