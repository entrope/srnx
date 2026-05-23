/** rinex_load.h - Whole-file loader for RINEX observation files.
 * Copyright 2024 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(RINEX_LOAD_H_6A681D3E_5B11_4202_B4C8_AF41A4871BB9)
#define RINEX_LOAD_H_6A681D3E_5B11_4202_B4C8_AF41A4871BB9

#include "rinex/rinex.h"
#include <stddef.h>

/* rinex_data is the main data structure for this header file.
 *
 * It holds the following data:
 *  - TLE header as text with '\n' line terminators (`file_header`).
 *  - An array of timestamps corresponding to the observation epochs (`epoch`).
 *  - An array of satellite structures (`sv`) containing:
 *    - An RLE-compressed array of timestamp index ranges (first, last)
 *      where the satellite was observed.
 *    - A per-signal array of indexes in the observation arrays (`obs`,
 *      `lli` and `ssi`) where the signal's observations start.
 *  - Three arrays of observation data: `obs`, `lli` and `ssi`.
 *    - `obs` holds numerical observations with the unit representing
 *      0.001 m or m/s (for range and Doppler observations respectively).
 *      INT64_MIN is reserved to indicate a missing observation in an
 *      epoch where some other signal was observed from the satellite.
 *    - `lli` holds the loss-of-lock (LLI) indicator as an ASCII character.
 *    - `ssi` holds the signal-strength (SSI) indicator as an ASCII character.
 */

/** rinex_range holds a pair of indices in some other array. */
struct rinex_range
{
    /** start is the inclusive index of the start of the range. */
    int start;

    /** end is the exclusive index of the end of the range. */
    int end;
};

/** rinex_system_data holds metadata about satellites and observables
 * for a single constellation (satellite system).
 */
struct rinex_system_data
{
    /** obs is an array of observation codes for this constellation.
     * RINEX v2 observation codes are like { 'L', '1', 0, 0 };
     * RINEX v3/v4 observation codes are like { 'C', '1', 'C', 0 }.
     * The length is given by \a n_obs.
     */
    char (*obs)[4];

    /** n_obs is the number of observations for this constellation. */
    int n_obs;

    /** sv identifies the range within \a rinex_data.sv that is used
     * by satellites in this constellation.
     */
    struct rinex_range sv;
};

/** rinex_satellite_data holds metadata about observations from a
 * single satellite.
 */
struct rinex_satellite_data
{
    /** when identifies which epochs have observations for this satellite.
     * Invariant: `obs_used == sum(when.end - when.start)`
     */
    struct rinex_range *when;

    /** when_used is the number of entries in #when. */
    int when_used;

    /** when_alloc is the capacity of #when. */
    int when_alloc;

    /** id holds the NUL-terminated name of the satellite.
     * For example, GPS PRN 1 would be indicated as { 'G', '0', '1', 0 }.
     */
    char id[4];

    /** obs_used indicates the number of epochs where this satellite
     * was observed (for any signal).
     */
    int obs_used;

    /** obs_alloc is the capacity for each observed signal. */
    int obs_alloc;

    /** start holds the starting observation index for each observable.
     * The length of start[] is given by \a rinex_data.sys[i].n_obs,
     * where `i == id[0] & 31`.
     * Negative indicates that this observable was never observed.
     * The number of actual observations is #obs_used.
     * The capacity of each block is #obs_alloc.
     */
    int start[1];
};

/** rinex_event holds one special event record (epoch flag 2-5). */
struct rinex_event
{
    /** epoch_index is the number of observation epochs before this event. */
    int epoch_index;

    /** text_len is the number of bytes in \a text. */
    int text_len;

    /** text holds the raw event lines (epoch header + data), '\n'-terminated. */
    char *text;
};

/** rinex_data is an in-memory, columnar representation of the data
 * from a RINEX file.
 */
struct rinex_data
{
    /** error holds a formatted error message with epoch context.
     * Populated by helper functions (e.g. rnx_load_grow_system) and
     * by rinex_load() itself when an error occurs.
     */
    char error[256];

    /** file_header contains a copy of the file header, with '\n' after
     * each line.
     */
    const char *file_header;

    /** epoch represents the time of observations in the file. */
    struct rinex_epoch *epoch;

    /** file_header_len is the number of bytes in \a file_header. */
    int file_header_len;

    /** epoch_used is the number of epochs in #epoch. */
    int epoch_used;

    /** epoch_alloc is the capacity of #epoch. */
    int epoch_alloc;

    /** rinex_version indicates the RINEX version of the file. */
    short rinex_version;

    /** interval indicates the interval between observations. */
    short interval;

    /** sv holds metadata for each satellite.
     * *sv[c+i] holds data for satellite i (1 <= i < N) from
     * constellation C, where c = sys[C&31] and C is one of the usual
     * constellation identifying characters ('G', 'E', 'R', ...).
     */
    struct rinex_satellite_data **sv;

    /** obs holds pseudorange information in millimeters.
     * obs[i], ssi[i] and lli[i] all correspond to one observation.
     *
     * The first RNX_OBS_RESERVED slots of this array are reserved.
     * obs[0] holds the total capacity.  obs[1] indexes to the start
     * of a list of free chunks, where the first two elements of each
     * chunk are the size and pointer-to-next.
     * sv[].start[] and sv[].obs_used/sv[].obs_alloc indicate which
     * parts of the array are in active use.
     */
    int64_t *obs;

    /** ssi holds signal strength indicators.
     * The used subset and capacity are the same as #obs.
     */
    char *ssi;

    /** lli holds loss-of-lock indicators.
     * The used subset and capacity are the same as #obs.
     */
    char *lli;

    /** sys identifies the range of satellites used by constellations. */
    struct rinex_system_data sys[32];

    /** event holds special event records encountered during loading. */
    struct rinex_event *event;

    /** event_used is the number of entries in \a event. */
    int event_used;

    /** event_alloc is the capacity of \a event. */
    int event_alloc;
};

/** rinex_load loads an entire RINEX file into memory.
 *
 * \param[in] stream Input stream to read from.  It should be opened and
 *   at the start of the file.
 * \param[out] out Receives a copy of the data.
 * \returns NULL on success, else an explanation of the failure.
 */
const char *rinex_load(struct rinex_stream *stream, struct rinex_data *out);

/** free_rinex_data deallocates the contents of \a data. */
void free_rinex_data(struct rinex_data *data);

/** Loads a RINEX or SRNX file by name.
 * Auto-detects format from file contents.
 *
 * \param[in] filename Path to the file.
 * \param[out] out Receives the loaded data.
 * \returns NULL on success, else an explanation of the failure.
 */
const char *rinex_load_file(const char *filename, struct rinex_data *out);

/** Formats a human-readable epoch timestamp into \a buf.
 *
 * \param[out] buf       Destination buffer (at least 64 bytes).
 * \param[in]  epoch     Epoch to format.
 * \returns Pointer to \a buf.
 */
char *rnx_format_epoch(char buf[], const struct rinex_epoch *epoch);

/** Initializes constellation metadata from the header in \a out. */
const char *rnx_data_init_cons(struct rinex_data *out);

/** Grows the satellite array to accommodate satellite \a svn in system \a sys_id. */
const char *rnx_load_grow_system(struct rinex_data *out, char sys_id, int svn);

/** Allocates a new rinex_satellite_data for satellite \a svn in system \a sys_id. */
const char *rnx_load_alloc_satellite(struct rinex_data *out, char sys_id, int svn);

/** (Re)allocates an observation block of size \a req in the obs/lli/ssi arrays.
 *
 * \param[in,out] out Container for the block.
 * \param[in] start Start offset of current block, or -1 for a new block.
 * \param[in] len Length of current block, or 0 if \a start == -1.
 * \param[in] req Number of elements to allocate.
 * \returns Non-negative start index on success, -1 on failure.
 */
int rnx_load_realloc_obs(struct rinex_data *out, int start, int len, int req);

#endif /* !defined(RINEX_LOAD_H_6A681D3E_5B11_4202_B4C8_AF41A4871BB9) */
