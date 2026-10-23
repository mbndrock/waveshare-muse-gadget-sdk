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

/*
 * Real replies to DID 0x2612 from a 2007 Honda S2000's engine ECU, as CAN
 * frames with their 29-bit IDs (18DAF110), from OBDb's Honda-S2000 test
 * cases (tests/test_cases/2007/commands/DA10.10.222612.yaml, cases 0, 1 and
 * 15; case 15 is the "VTEC on" sample the research report quotes). The ELM
 * prints these differently with headers off (ATH0) and CAN formatting on:
 * elm_from_frames() turns them into that.
 */

#include <stdio.h>
#include <string.h>

/* Case 0: solenoid off, pressure switch on (data byte 21 = 0x02), byte 23 = 0x00. */
static const char *const S2000_2007_OFF[] = {
    "18DAF1101039622612FFFFC0", "18DAF11021001E00180B0000", "18DAF11022244F01D901E81F",
    "18DAF110234C01EB03020100", "18DAF1102428000000000000", "18DAF1102500000000000000",
    "18DAF1102602FFFF07000001", "18DAF1102700000000000000", "18DAF1102800005555555555",
};

/* Case 1: solenoid on, pressure switch off (byte 21 = 0x01), byte 23 = 0x01. */
static const char *const S2000_2007_ON[] = {
    "18DAF1101039622612FFFFC0", "18DAF11021001E00180B0000", "18DAF110222F5905DF05E41F",
    "18DAF110234C05AE03010101", "18DAF1102428000000000000", "18DAF1102500000000000000",
    "18DAF1102602FFFFA0000005", "18DAF1102700000000000000", "18DAF1102800005555555555",
};

/* Case 15: the report's sample ("ff ff c0 00 1e 00 c2 5f 44 19 ... 03 [01] 01 01 28"). */
static const char *const S2000_2007_ON_REPORT[] = {
    "18DAF1101039622612FFFFC0", "18DAF11021001E00C25F4419", "18DAF11022BFCD3CE13CE11F",
    "18DAF110234C061603010101", "18DAF1102428000000000000", "18DAF1102500000000000000",
    "18DAF1102602FFFF91000004", "18DAF1102700000000000000", "18DAF1102800005555555555",
};

#define S2000_FRAMES 9

/*
 * The ELM327's print of a multi-frame reply with headers off: the ISO-TP
 * length (from the first frame's PCI) as three hex digits on a line, then
 * each frame's data as "N:" and hex, "0:" carrying the first frame's six
 * bytes and the rest seven each (padding and all), then the prompt. `spaces`
 * puts a space after each byte, as with ATS1. `frames` may be fewer than
 * all, to cut a reply short. Returns out.
 */
static char *elm_from_frames(char *out, size_t cap, const char *const *raw, int frames, bool spaces)
{
    size_t n = 0;
    out[0] = '\0';
#define ELM_PUT(...) do { \
        int w_ = snprintf(out + n, cap - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= cap - n) return out; \
        n += (size_t)w_; \
    } while (0)
    /* 8 hex digits of CAN ID, then the PCI: 1L LL for the first frame. */
    const char *first = raw[0] + 8;
    ELM_PUT("%c%c%c\r", first[1], first[2], first[3]);
    for (int f = 0; f < frames; f++) {
        const char *data = raw[f] + 8 + (f == 0 ? 4 : 2);
        ELM_PUT("%X:", (unsigned)(f & 0xF));
        for (size_t i = 0; data[i] && data[i + 1]; i += 2) {
            ELM_PUT(spaces ? " %c%c" : "%c%c", data[i], data[i + 1]);
        }
        ELM_PUT("\r");
    }
    ELM_PUT("\r>");
#undef ELM_PUT
    return out;
}
