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
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
typedef struct { int unused; } StaticSemaphore_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { return b; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s, (void)t; return 1; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
#define portMAX_DELAY 0xffffffffu
