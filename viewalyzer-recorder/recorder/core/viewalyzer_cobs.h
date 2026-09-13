/**
 * @file viewalyzer_cobs.h
 * @brief COBS (Consistent Overhead Byte Stuffing) encoder for ViewAlyzer UDP transport.
 *
 * Copyright 2025-2026 BKPT, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VIEWALYZER_COBS_H
#define VIEWALYZER_COBS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * COBS-encode a packet and append a 0x00 frame delimiter.
 *
 * The encoded output is guaranteed to contain no 0x00 bytes except
 * for the final delimiter. Overhead is ~1 byte per 254 input bytes
 * plus the trailing 0x00.
 *
 * @param input     Raw packet bytes to encode.
 * @param in_len    Length of input in bytes.
 * Input and output must not overlap. A NULL input is valid only for an
 * empty packet. Capacity must cover the worst case even for inputs that
 * would encode to fewer bytes.
 *
 * @param output    Buffer to write encoded bytes into.
 * @param capacity  Available output bytes, including the delimiter.
 * @return          Bytes written, including the delimiter, or 0 for invalid
 *                  arguments, size overflow, or insufficient capacity.
 *                  On failure the output is untouched.
 */
size_t va_cobs_encode(const uint8_t *input, size_t in_len, uint8_t *output,
                      size_t capacity);

/**
 * Returns the worst-case encoded length for a given input length.
 * Use this to size your output buffer. Returns 0 on size_t overflow.
 */
static inline size_t va_cobs_max_encoded_len(size_t in_len)
{
    /* overhead: 1 byte per 254 input bytes + 1 code byte + 1 delimiter */
    size_t overhead = (in_len / 254) + 2;
    if (in_len > SIZE_MAX - overhead)
        return 0;
    return in_len + overhead;
}

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_COBS_H */
