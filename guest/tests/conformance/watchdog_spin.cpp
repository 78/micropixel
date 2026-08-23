#include <stdint.h>

int main() {
    volatile uint32_t value = 1U;
    for (;;) {
        value = value * 1664525U + 1013904223U;
    }
}
