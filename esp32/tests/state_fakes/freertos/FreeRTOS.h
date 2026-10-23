/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Host-test stand-in for muse_state.c (tests/muse_state_vtec_harness.c).
#pragma once
#include "sdkconfig.h"   /* as the real one brings it in */
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
