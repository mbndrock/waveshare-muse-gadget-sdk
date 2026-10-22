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

#include "muse_settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"

static const char *TAG = "muse_settings";

#define NS "muse"
#define DEFAULT_HOST "hatch.metaaivm.com"

#if CONFIG_MUSE_HATCH
#define VOICES CONFIG_MUSE_ELEVENLABS_VOICES
#define DEFAULT_VOICE CONFIG_MUSE_ELEVENLABS_VOICE_ID
#else
#define VOICES ""
#define DEFAULT_VOICE ""
#endif

static struct {
    uint8_t volume;
    bool speaker_on;
    bool pushes_on;
    bool touch_volume_on;
    uint8_t mic_gain;
    uint8_t brightness;
    uint16_t sleep_s;
    bool wifi_on;
    bool ble_on;
#if CONFIG_MUSE_OBD_VTEC
    bool obd_on;
#endif
    char ssid[MUSE_SSID_MAX + 1];
    char pass[MUSE_PASS_MAX + 1];
    char host[MUSE_HOST_MAX + 1];
    char vm[MUSE_VM_MAX + 1];
    char token[MUSE_TOKEN_MAX + 1];
    int voice;
} s = {
    .volume = CONFIG_MUSE_DEFAULT_VOLUME,
    .speaker_on = true,
    .touch_volume_on = true,
    .mic_gain = 30,
    .brightness = 100,
    .sleep_s = 120,
    .wifi_on = true,
#if CONFIG_MUSE_OBD_VTEC
    .obd_on = true,
#endif
    .host = DEFAULT_HOST,
};

/* Fixed once loaded, so other tasks read them unlocked. */
static muse_voice_t s_voices[MUSE_VOICES_MAX];
static int s_nvoices;

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static muse_setting_cb_t s_listener;

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Missing keys leave the default in place. */
static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    nvs_get_str(s_nvs, key, out, &n);
}

static void load_u8(const char *key, uint8_t *out)
{
    nvs_get_u8(s_nvs, key, out);
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_set_u8(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_str(const char *key, const char *v)
{
    nvs_set_str(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static int find_voice(const char *id)
{
    for (int i = 0; i < s_nvoices; i++) {
        if (!strcmp(s_voices[i].id, id)) {
            return i;
        }
    }
    return -1;
}

/* The list, else the one voice ID; the saved choice, else that ID, else the first. */
static void load_voices(void)
{
    s_nvoices = muse_voices_parse(VOICES, s_voices, MUSE_VOICES_MAX);
    if (!s_nvoices && DEFAULT_VOICE[0]) {
        strlcpy(s_voices[0].name, "Default", sizeof(s_voices[0].name));
        strlcpy(s_voices[0].id, DEFAULT_VOICE, sizeof(s_voices[0].id));
        s_nvoices = 1;
    }
    char id[MUSE_VOICE_ID_MAX + 1] = "";
    load_str("voice", id, sizeof(id));
    s.voice = find_voice(id);
    if (s.voice < 0) {
        s.voice = find_voice(DEFAULT_VOICE);
    }
    if (s.voice < 0) {
        s.voice = 0;
    }
}

static void notify(muse_setting_t what)
{
    if (s_listener) {
        s_listener(what);
    }
}

esp_err_t muse_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t b;
    load_u8("volume", &s.volume);
    if (nvs_get_u8(s_nvs, "speaker", &b) == ESP_OK) {
        s.speaker_on = b;
    }
    if (nvs_get_u8(s_nvs, "pushes", &b) == ESP_OK) {
        s.pushes_on = b;
    }
    if (nvs_get_u8(s_nvs, "touch_vol", &b) == ESP_OK) {
        s.touch_volume_on = b;
    }
    load_u8("mic_gain", &s.mic_gain);
    load_u8("bright", &s.brightness);
    nvs_get_u16(s_nvs, "sleep_s", &s.sleep_s);
    if (nvs_get_u8(s_nvs, "wifi_on", &b) == ESP_OK) {
        s.wifi_on = b;
    }
    if (nvs_get_u8(s_nvs, "ble_on", &b) == ESP_OK) {
        s.ble_on = b;
    }
#if CONFIG_MUSE_OBD_VTEC
    if (nvs_get_u8(s_nvs, "obd_on", &b) == ESP_OK) {
        s.obd_on = b;
    }
    ESP_LOGI(TAG, "OBD %s", s.obd_on ? "on" : "off");
#endif
    load_str("ssid", s.ssid, sizeof(s.ssid));
    load_str("pass", s.pass, sizeof(s.pass));
    load_str("host", s.host, sizeof(s.host));
    load_str("vm", s.vm, sizeof(s.vm));
    load_str("token", s.token, sizeof(s.token));
    load_voices();

    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    ESP_LOGI(TAG, "all messages %s", s.pushes_on ? "on" : "off");
    if (s_nvoices) {
        ESP_LOGI(TAG, "voice %s (%d of %d)", s_voices[s.voice].name, s.voice + 1, s_nvoices);
    }
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    return ESP_OK;
}

void muse_settings_set_listener(muse_setting_cb_t cb)
{
    s_listener = cb;
}

int muse_settings_volume(void) { return s.volume; }
bool muse_settings_speaker_on(void) { return s.speaker_on; }
bool muse_settings_pushes_on(void) { return s.pushes_on; }
bool muse_settings_touch_volume_on(void) { return s.touch_volume_on; }
int muse_settings_mic_gain(void) { return s.mic_gain; }
int muse_settings_brightness(void) { return s.brightness; }
int muse_settings_sleep_s(void) { return s.sleep_s; }
bool muse_settings_wifi_on(void) { return s.wifi_on; }
bool muse_settings_ble_on(void) { return s.ble_on; }
#if CONFIG_MUSE_OBD_VTEC
bool muse_settings_obd_on(void) { return s.obd_on; }
#endif
int muse_settings_voice_count(void) { return s_nvoices; }
int muse_settings_voice(void) { return s.voice; }

const char *muse_settings_voice_name(int i)
{
    return i >= 0 && i < s_nvoices ? s_voices[i].name : NULL;
}

const char *muse_settings_voice_id(void)
{
    return s_nvoices ? s_voices[s.voice].id : "";
}

/* Home Link owns the saved networks (this is the first); the local copy is only a fallback. */
void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1])
{
    if (muse_link_wifi_get(ssid, pass)) {
        return;
    }
    LOCKED({
        strlcpy(ssid, s.ssid, MUSE_SSID_MAX + 1);
        if (pass) {
            strlcpy(pass, s.pass, MUSE_PASS_MAX + 1);
        }
    });
}

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    LOCKED(strlcpy(out, s.host, MUSE_HOST_MAX + 1));
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    LOCKED(strlcpy(out, s.vm, MUSE_VM_MAX + 1));
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.token, MUSE_TOKEN_MAX + 1));
}

size_t muse_settings_hatch_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.token));
    return n;
}

void muse_settings_set_volume(int pct)
{
    s.volume = clampi(pct, 0, 100);
    save_u8("volume", s.volume);
    notify(MUSE_SETTING_VOLUME);
}

void muse_settings_set_speaker_on(bool on)
{
    s.speaker_on = on;
    save_u8("speaker", on);
    notify(MUSE_SETTING_SPEAKER);
}

void muse_settings_set_pushes_on(bool on)
{
    s.pushes_on = on;
    save_u8("pushes", on);
    notify(MUSE_SETTING_PUSHES);
}

void muse_settings_set_touch_volume_on(bool on)
{
    s.touch_volume_on = on;
    save_u8("touch_vol", on);
    notify(MUSE_SETTING_TOUCH_VOLUME);
}

void muse_settings_set_mic_gain(int db)
{
    s.mic_gain = clampi(db, 0, MUSE_MIC_GAIN_MAX);
    save_u8("mic_gain", s.mic_gain);
    notify(MUSE_SETTING_MIC_GAIN);
}

void muse_settings_set_brightness(int pct)
{
    s.brightness = clampi(pct, 10, 100);
    save_u8("bright", s.brightness);
    notify(MUSE_SETTING_BRIGHTNESS);
}

void muse_settings_set_sleep_s(int secs)
{
    s.sleep_s = clampi(secs, 0, 3600);
    nvs_set_u16(s_nvs, "sleep_s", s.sleep_s);
    nvs_commit(s_nvs);
    notify(MUSE_SETTING_SLEEP);
}

void muse_settings_set_wifi_on(bool on)
{
    s.wifi_on = on;
    save_u8("wifi_on", on);
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_ble_on(bool on)
{
    s.ble_on = on;
    save_u8("ble_on", on);
    notify(MUSE_SETTING_BLE);
}

#if CONFIG_MUSE_OBD_VTEC
void muse_settings_set_obd_on(bool on)
{
    s.obd_on = on;
    save_u8("obd_on", on);
    notify(MUSE_SETTING_OBD);
}
#endif

void muse_settings_set_voice(int i)
{
    if (i < 0 || i >= s_nvoices) {
        return;
    }
    s.voice = i;
    save_str("voice", s_voices[i].id);
    ESP_LOGI(TAG, "voice %s", s_voices[i].name);
    notify(MUSE_SETTING_VOICE);
}

void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    if (muse_link_wifi_set(ssid ? ssid : "", ssid && ssid[0] && pass ? pass : "")) {
        ESP_LOGI(TAG, "wifi network: %s", ssid && ssid[0] ? ssid : "(all forgotten)");
        notify(MUSE_SETTING_WIFI);
        return;
    }
    LOCKED({
        strlcpy(s.ssid, ssid ? ssid : "", sizeof(s.ssid));
        strlcpy(s.pass, s.ssid[0] && pass ? pass : "", sizeof(s.pass));
        save_str("ssid", s.ssid);
        save_str("pass", s.pass);
    });
    ESP_LOGI(TAG, "wifi network: %s", s.ssid[0] ? s.ssid : "(forgotten)");
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_hatch_host(const char *host)
{
    LOCKED({
        strlcpy(s.host, host && host[0] ? host : DEFAULT_HOST, sizeof(s.host));
        save_str("host", s.host);
    });
    notify(MUSE_SETTING_HATCH);
}

void muse_settings_set_hatch_vm(const char *vm)
{
    LOCKED({
        strlcpy(s.vm, vm ? vm : "", sizeof(s.vm));
        save_str("vm", s.vm);
    });
    notify(MUSE_SETTING_HATCH);
}

esp_err_t muse_settings_set_hatch_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.token + have, token, add);
            s.token[have + add] = '\0';
            save_str("token", s.token);
        }
    });
    if (err == ESP_OK) {
        notify(MUSE_SETTING_HATCH);
    }
    return err;
}
