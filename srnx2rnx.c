/** srnx2rnx.c - Succinct RINEX decompressor.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* SRNX reading functions:
 * - Open file.
 * - Read RINEX header.
 * - Decompress all epochs.
 * - Retrieve list of satellites observed.
 * - Decompress a given satellite's signals.
 *   - Select signals by name and/or index.
 *   - Conditionally omit the LLI and/or SSI.
 * - Read a signal value-by-value.
 * - Read next special event record (?).
 *
 * Internal state:
 * - RINEX header version (2 vs 3).
 * - Cached signal directory (two offsets, arrays of SV name + offset).
 * - For each satellite system, number and names of signals.
 * - For per-signal reader: Cache of up to 256 observation values,
 *   along with SSI and LLI.  Size of, and index into, array.  File
 *   offset of next block.  Delta decoder state.
 * - For event reader: Buffer of event contents, file offset of next
 *   block to consider.
 *
 * Decompression algorithm:
 * - Load and decompress epochs.
 * - Load the next special event record.
 * - For each epoch:
 *   - If epoch RLE count > 0, increment epoch timestamp, else read next
 *     epoch RLE run.
 *   - Emit any special events before this record.
 *   - Find satellites with observations in this epoch, along with
 *     their observation values.
 *   - Emit the epoch record.
 */

int main(int argc, char *argv[])
{
    struct srnx_reader *srnx;
    int err;

    return EXIT_FAILURE;
}
