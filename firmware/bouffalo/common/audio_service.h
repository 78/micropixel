// SPDX-License-Identifier: Apache-2.0
#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wave player owns global hardware state and must only be initialized once. */
bool audio_service_init(void);

/* Stop playback and mute the retained SDK codec instance. */
void audio_service_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
