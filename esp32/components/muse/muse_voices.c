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

#include "muse_voices.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* [*b, *e) without the spaces at either end. */
static void trim(const char **b, const char **e)
{
    while (*b < *e && isspace((unsigned char)**b)) {
        (*b)++;
    }
    while (*e > *b && isspace((unsigned char)(*e)[-1])) {
        (*e)--;
    }
}

int muse_voices_parse(const char *spec, muse_voice_t *out, int max)
{
    int n = 0;
    const char *p = spec ? spec : "";
    while (*p && n < max) {
        const char *end = strchr(p, ';');
        if (!end) {
            end = p + strlen(p);
        }
        const char *eq = memchr(p, '=', end - p);
        if (eq) {
            const char *nb = p, *ne = eq, *ib = eq + 1, *ie = end;
            trim(&nb, &ne);
            trim(&ib, &ie);
            size_t id_len = ie - ib;
            bool ok = ne > nb && id_len > 0 && id_len <= MUSE_VOICE_ID_MAX;
            for (const char *c = ib; ok && c < ie; c++) {
                ok = isalnum((unsigned char)*c);
            }
            if (ok) {
                size_t name_len = ne - nb;
                if (name_len > MUSE_VOICE_NAME_MAX) {
                    name_len = MUSE_VOICE_NAME_MAX;
                    while (name_len && ((unsigned char)nb[name_len] & 0xC0) == 0x80) {
                        name_len--;   /* don't split a UTF-8 character */
                    }
                }
                memcpy(out[n].name, nb, name_len);
                out[n].name[name_len] = '\0';
                memcpy(out[n].id, ib, id_len);
                out[n].id[id_len] = '\0';
                n++;
            }
        }
        p = *end ? end + 1 : end;
    }
    return n;
}
