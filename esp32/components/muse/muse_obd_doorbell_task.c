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

#include "muse_obd_log.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_chat.h"
#include "muse_obd_doorbell.h"

/*
 * The log doorbell's IDF side (OBD-LOG-DOORBELL-TASK): a watcher that lists
 * the trip log's stores once a second, feeds muse_obd_doorbell.h's decision
 * the chat session's ready flag, and hands an announcement to the session as
 * one text turn. The same listing backs Settings' "Send logs to Muse" row
 * (muse_obd_doorbell_ring, and the pending count it shows). Stored trips
 * only: this runs with Car mode on or off and never touches the car side.
 */

static const char *TAG = "muse_obd_log";

#define DOORBELL_POLL_MS 1000   /* session transitions and closed trips ring within this */

static muse_doorbell_t s_doorbell;
static SemaphoreHandle_t s_doorbell_lock;   /* one listing/sending at a time: watcher and Settings */
static portMUX_TYPE s_doorbell_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_doorbell_pending_count;
static uint32_t s_doorbell_pending_bytes;

/* One listing of the stores, the decision over it, and the send if it says
 * so. True when an announcement was handed to the chat session (which frees
 * the string on every path). */
static bool doorbell_send(void)
{
    if (!s_doorbell_lock || xSemaphoreTake(s_doorbell_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool sent = false;
    int n = muse_obd_log_list(NULL, 0);
    muse_obd_log_entry_t *list = n > 0 ? heap_caps_malloc(sizeof(*list) * (size_t)n, MALLOC_CAP_SPIRAM) : NULL;
    muse_log_trip_t *pending = n > 0 ? heap_caps_malloc(sizeof(*pending) * (size_t)n, MALLOC_CAP_SPIRAM) : NULL;
    char *line = heap_caps_malloc(MUSE_DOORBELL_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (n >= 0 && (n == 0 || (list && pending && line))) {
        int got = n > 0 ? muse_obd_log_list(list, n) : 0;
        if (got >= 0) {
            if (got > n) {
                got = n;   /* a trip closed in between: the next ring lists it */
            }
            uint32_t bytes = 0;
            for (int i = 0; i < got; i++) {
                pending[i] = list[i].trip;
                bytes += list[i].trip.bytes;
            }
            taskENTER_CRITICAL(&s_doorbell_cache_lock);
            s_doorbell_pending_count = got;
            s_doorbell_pending_bytes = bytes;
            taskEXIT_CRITICAL(&s_doorbell_cache_lock);
            /* muse_hatch_ready() is the session being up, Muse configured
             * included; losing it ends the connection in the decision. */
            muse_doorbell_set_session_ready(&s_doorbell, muse_hatch_ready());
            if (muse_doorbell_announce(&s_doorbell, pending, got, line, MUSE_DOORBELL_LINE_MAX)) {
                char *text = strdup(line);
                if (text) {
                    muse_hatch_text_turn(text);   /* takes the string */
                    sent = true;
                    ESP_LOGI(TAG, "doorbell: announced stored trips to Muse");
                }
            }
        }
    }
    free(list);
    free(pending);
    free(line);
    xSemaphoreGive(s_doorbell_lock);
    return sent;
}

static void doorbell_task(void *arg)
{
    (void)arg;
    for (;;) {
        doorbell_send();
        vTaskDelay(pdMS_TO_TICKS(DOORBELL_POLL_MS));
    }
}

void muse_obd_doorbell_start(void)
{
    if (s_doorbell_lock) {
        return;
    }
    muse_doorbell_init(&s_doorbell);
    s_doorbell_lock = xSemaphoreCreateMutex();
    if (!s_doorbell_lock
        || xTaskCreate(doorbell_task, "muse_obd_bell", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "doorbell failed to start");
    }
}

muse_doorbell_result_t muse_obd_doorbell_ring(void)
{
    if (doorbell_send()) {
        return MUSE_DOORBELL_SENT;
    }
    uint32_t bytes = 0;
    int pending = muse_obd_doorbell_pending(&bytes);
    (void)bytes;
    if (pending <= 0) {
        return MUSE_DOORBELL_NO_LOGS;
    }
    return muse_hatch_ready() ? MUSE_DOORBELL_ALREADY_SENT : MUSE_DOORBELL_NOT_CONNECTED;
}

int muse_obd_doorbell_pending(uint32_t *total_bytes)
{
    taskENTER_CRITICAL(&s_doorbell_cache_lock);
    int count = s_doorbell_pending_count;
    uint32_t bytes = s_doorbell_pending_bytes;
    taskEXIT_CRITICAL(&s_doorbell_cache_lock);
    if (total_bytes) {
        *total_bytes = bytes;
    }
    return count;
}
