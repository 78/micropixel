// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../firmware_stubs/freertos/task.h"
inline void taskYIELD() { std::this_thread::yield(); }
