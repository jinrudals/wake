/*
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _DEFAULT_SOURCE
#define _ISOC11_SOURCE

#include <stdlib.h>

#include "aligned_alloc.h"

void *my_aligned_alloc(size_t alignment, size_t size) {
#ifdef __EMSCRIPTEN__
    return malloc(size);
#else
    return aligned_alloc(alignment, size); // This is a C11 feature
#endif
}
