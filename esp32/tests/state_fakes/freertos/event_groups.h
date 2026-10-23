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
typedef void *EventGroupHandle_t;
typedef struct { int unused; } StaticEventGroup_t;
typedef uint32_t EventBits_t;
#define BIT0 1u
#define BIT1 2u
static inline EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *b) { return b; }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t c, BaseType_t a, TickType_t t)
{
    (void)g, (void)c, (void)a, (void)t;
    return b;
}
