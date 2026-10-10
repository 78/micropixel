// SPDX-License-Identifier: Apache-2.0
#pragma once

// Force-included ahead of the board sources so the RTC policy test owns "now".
// The Host cannot call settimeofday without privileges, and the policy has to be
// exercised from an untrustworthy clock as well, so both calls are redirected to
// the test's own clock. The real declarations are parsed first: only code
// compiled after this header sees the redirect.
#include <sys/time.h>

#include <ctime>

std::time_t MicropixelTestNow();
int MicropixelTestSetTimeOfDay(const struct timeval* value);
void MicropixelTestSetNow(std::time_t seconds);
void MicropixelTestResetClock();

#define time(value) MicropixelTestNow()
#define settimeofday(value, zone) MicropixelTestSetTimeOfDay(value)
