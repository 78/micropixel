// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
/* Both profiles run one FreeRTOS scheduler; BL618DG NP is disabled. */
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(...) vTaskEnterCritical()
#define portEXIT_CRITICAL(...) vTaskExitCritical()
