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
 * Which source branch built this firmware, so a flashed gadget can say whether
 * it came from main, the OBD branch or something else. The branch is stamped
 * at build time from the checkout's .git/HEAD (CMakeLists.txt defines
 * MUSE_GIT_BRANCH); it is not a runtime or persisted setting, and nothing in
 * NVS or Kconfig changes it.
 */

/* The checkout's branch, "detached" for a detached HEAD, or "unknown". */
const char *muse_build_branch(void);
