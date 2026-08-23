#include <stdint.h>

__attribute__((import_module("missing"), import_name("forbidden_capability"))) int32_t forbidden_capability(void);

__attribute__((export_name("__micropixel_start"))) int32_t micropixel_start(void) { return forbidden_capability(); }
