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
 * Source revision: OBDb/Honda-S2000 3f1cf3385de2fa11a210e057197475f883ec7b11.
 * Real replies to DIDs 0x2610 and 0x2663 from a 2007 Honda S2000's engine
 * ECU, as CAN frames with their 29-bit IDs (18DAF110), byte for byte from
 * OBDb's Honda-S2000 test cases (tests/test_cases/2007/commands/
 * DA10.10.222610.yaml and DA10.10.222663.yaml; case numbers count from 0 in
 * file order). Every case in both files is this shape: a first frame of
 * 10 39 (57 bytes: 62 26 xx and 54 data bytes) and eight consecutive frames.
 * Feed them to vtec_did_frames.h's elm_from_frames().
 */

/* 2610 case 0: A/C clutch 0, generator 23.52941 % (data byte 23 = 0x3C). */
static const char *const S2000_2610_AC_OFF[] = {
    "18DAF1101039622610FFFDF0", "18DAF1102161E0000000003D", "18DAF110221F826753946594",
    "18DAF1102364000000807C3C", "18DAF1102400000000008000", "18DAF110250F08000000001F",
    "18DAF1102682000000000000", "18DAF1102700000000000000", "18DAF1102800005555555555",
};

/* 2610 case 33: A/C clutch 1 (data byte 32 = 0x03), generator 45.4902 % (byte 23 = 0x74). */
static const char *const S2000_2610_AC_ON[] = {
    "18DAF1101039622610FFFDF0", "18DAF1102161E00072186D4A", "18DAF11022227E764C544293",
    "18DAF110236453CD00BC7D74", "18DAF1102404910000008000", "18DAF110250F03000000003C",
    "18DAF110266800C900000000", "18DAF1102700000000000000", "18DAF1102800005555555555",
};

/* 2663 case 0: no misfires on any cylinder. */
static const char *const S2000_2663_NONE[] = {
    "18DAF1101039622663F3FFFC", "18DAF110213FF0E000000F00", "18DAF110220000F3AD000102",
    "18DAF1102300910000000000", "18DAF1102400000000FFFFFF", "18DAF11025FF000000000000",
    "18DAF1102600000000FFFFFF", "18DAF11027FF000010000000", "18DAF1102800005555555555",
};

/* 2663 case 9: cylinder 1 has one (data bytes 34-35 = 00 01). */
static const char *const S2000_2663_CYL1[] = {
    "18DAF1101039622663F3FFFC", "18DAF110213FF0E000040F0D", "18DAF110220000F3AD000102",
    "18DAF1102300910000010000", "18DAF1102400010000FFFFFF", "18DAF11025FF02F900010000",
    "18DAF1102600000000FFFFFF", "18DAF11027FF016910000000", "18DAF1102800005555555555",
};

/* 2663 case 7: cylinder 2 has one (data bytes 36-37). */
static const char *const S2000_2663_CYL2[] = {
    "18DAF1101039622663F3FFFC", "18DAF110213FF0E000020F02", "18DAF110220000F3AD000102",
    "18DAF1102300910000000002", "18DAF1102400000000FFFFFF", "18DAF11025FF051000000001",
    "18DAF1102600000000FFFFFF", "18DAF11027FF006010000000", "18DAF1102800005555555555",
};

/* 2663 case 8: cylinder 3 has one (data bytes 38-39). */
static const char *const S2000_2663_CYL3[] = {
    "18DAF1101039622663F3FFFC", "18DAF110213FF0E000030F0D", "18DAF110220000F3AD000102",
    "18DAF1102300910000010000", "18DAF1102400010000FFFFFF", "18DAF11025FF06DA00000000",
    "18DAF1102600010000FFFFFF", "18DAF11027FF009A10000000", "18DAF1102800005555555555",
};

/* 2663 case 5: cylinder 4 has one (data bytes 40-41). */
static const char *const S2000_2663_CYL4[] = {
    "18DAF1101039622663F3FFFC", "18DAF110213FF0E000010F08", "18DAF110220000F3AD000102",
    "18DAF1102300910000000000", "18DAF1102400000001FFFFFF", "18DAF11025FF007000000000",
    "18DAF1102600000001FFFFFF", "18DAF11027FF007010000000", "18DAF1102800005555555555",
};

#define S2000_DID_FRAMES 9   /* every one of the above: a first frame and eight more */
