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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ElevenLabs voices Settings › Voice offers, from
 * CONFIG_MUSE_ELEVENLABS_VOICES: "Name=voice_id;Name=voice_id", which
 * tools/muse/secrets.py builds from secrets/elevenlabs_voices.
 */

#define MUSE_VOICES_MAX 8
#define MUSE_VOICE_NAME_MAX 23
#define MUSE_VOICE_ID_MAX 39

typedef struct {
    char name[MUSE_VOICE_NAME_MAX + 1];
    char id[MUSE_VOICE_ID_MAX + 1];
} muse_voice_t;

/*
 * Fills out[] with up to max entries and returns how many. Spaces around names
 * and IDs are dropped; an entry without a name, or with an ID that is empty,
 * too long or not letters and digits, is skipped. A long name is cut at a
 * character boundary.
 */
int muse_voices_parse(const char *spec, muse_voice_t *out, int max);

#ifdef __cplusplus
}
#endif
