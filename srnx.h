/** srnx.h - Succinct RINEX reader API.
 * Copyright 2020 Michael Poole.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if !defined(SRNX_H_a2b6e4a7_3fda_4ba2_8ed1_67b906d55b2c)

#include <stddef.h>

#include "rinex_epoch.h"
/*#include "transpose.h"*/

/* Note that ALL memory (de-)allocations are performed by the library.
 * This is significant on platforms like Microsoft Windows, where the
 * library may use a different heap than the rest of the process.  This
 * implies that most pointers passed by address to the library must be
 * initialized to NULL before the first library call.
 */

/** srnx_reader represents a SRNX stream reader. */
struct srnx_reader;

/** srnx_obs_reader is used to read a particular `SOCD` chunk.
 * One SOCD chunk contains the observation data for a single (satellite,
 * observation code) pair.
 */
struct srnx_obs_reader;

/** Contains a RINEX satellite name. */
struct srnx_satellite_name
{
    /** Satellite name.
     *
     * The first three characters are the satellite name.
     * The fourth character may be assumed to be '\0'.  (Callers must
     * initialize it to '\0', and callees can ignore the actual value.)
     */
    char name[4];
};

/** Contains a RINEX observation code. */
struct srnx_obs_code
{
    /** Signal name.
     * 
     * The first two or three characters are the observation code.
     * For a RINEX 2.x file, the first two characters are used.
     * For a RINEX 3.x file, the first three characters are used.
     * Unused characters may be assumed to be '\0'.  (Callers must
     * iniitalize them to '\0', and calles can ignore the actual value.)
     */
    char name[4];
};

/** Deallocates an object allocated by the library.
 *
 * \param[in] ptr Pointer to dyanmically allocated object.
 */
void srnx_free(void *ptr);

/** Returns a text description of the error code. */
const char *srnx_strerror(int err);

/** Returns the line that generated the last error for a reader. */
int srnx_error_line(const struct srnx_reader *srnx);

/** Opens a new SRNX reader by file name.
 *
 * \param[in,out] p_srnx Pointer to SRNX reader object.  If this is not
 *   null on entry, the old object is destroyed.
 * \param[in] filename Name of SRNX file on a local filesystem.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_open(struct srnx_reader **p_srnx, const char filename[]);

/** Loads the RINEX header from a SRNX file.
 *
 * \param[in] srnx SRNX reader object.
 * \param[out] p_rhdr Receives pointer to RINEX header object.
 * \param[out] rhdr_len Receives number of bytes valid in \a *p_rhdr.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_header(
    struct srnx_reader *srnx,
    const char **p_rhdr,
    size_t *rhdr_len
);

/** Loads the RINEX eoch values from a SRNX file.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in,out] p_epoch Receives pointer to epoch structures.
 * \param[in,out] p_epochs_len Number of epochs valid in \a *epochs.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_epochs(
    struct srnx_reader *srnx,
    struct rinex_epoch **p_epoch,
    size_t *p_epochs_len
);

/* Loads the next special event from a SRNX file.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in,out] p_event Receives pointer to special event text.
 *   Must be initialized to \a NULL to (re-)start iteration over events.
 * \param[out] event_len Number of bytes valid at \a *p_event.
 * \param[out] epoch_event Receives index of "before epoch" counter.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_next_special_event(
    struct srnx_reader *srnx,
    const char **p_event,
    size_t *event_len,
    uint64_t *epoch_index
);

/** Retrieves the list of satellites observed in \a srnx.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in,out] p_name Receives a pointer to satellite names.
 * \param[out] p_names_len Receives number of names at \a p_names.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_satellites(
    struct srnx_reader *srnx,
    struct srnx_satellite_name **p_name,
    uint64_t *p_names_len
);

/** Loads available observation values for a given satellite, selected
 * by observation code name(s).
 *
 * \param[in] srnx SRNX reader object.
 * \param[in] name Name of the satellite to load.
 * \param[in] codes_len Number of observation codes to load.
 * \param[in] code Names of observation codes to load.  If any observation
 *   code is not known for the given satellite, this function fails.
 * \param[int,out] n_values Receives number of observations for each
 *   code.  Caller allocates \a n_sig elements.
 * \param[in,out] p_obs Receives a pointer to the observations for each
 *   code.  \a p_obs[0] through \a p_obs[n_sig-1] are valid.
 * \param[in] p_lli If not NULL, \a p_lli[n] holds the loss-of-lock
 *   indicators for the \a n'th code, and has length \a n_values[n].
 * \param[in] p_ssi If not NULL, \a p_ssi[n] holds the signal strength
 *   indicators for the \a n'th code, and has length \a n_values[n].
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_obs_by_name(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int codes_len,
    struct srnx_obs_code code[],
    int n_values[],
    int64_t **p_obs,
    char **p_lli,
    char **p_ssi
);

/** Loads all available observation values for a given satellite,
 * selected by observation index or indices.
 *
 * \a p_obs, \a p_lli and \a p_ssi point to arrays of pointers, each
 * array having length \a idx_len, which become jagged matrices with
 * column \a ii having length \a n_values[ii].
 *
 * \param[in] srnx SRNX reader object.
 * \param[in] name Name of the satellite to load.
 * \param[in] idx_len Number of observation codes to load.
 * \param[in] idx Indices of observation codes to load.  If any observation
 *   code is larger than the number of codes for this satellite system,
 *   this function fails.
 * \param[int,out] n_values Receives number of observations for each
 *   code.  Caller allocates \a idx_len elements.
 * \param[in,out] p_obs Receives a pointer to the observations for each
 *   code.  \a p_obs[0] through \a p_obs[idx_len-1] are valid.
 * \param[in] p_lli If not NULL, \a p_lli[n] holds the loss-of-lock
 *   indicators for the \a n'th code, and has length \a n_values[n].
 * \param[in] p_ssi If not NULL, \a p_ssi[n] holds the signal strength
 *   indicators for the \a n'th code, and has length \a n_values[n].
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_obs_by_index(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int idx_len,
    int idx[],
    int n_values[],
    int64_t **p_obs,
    char **p_lli,
    char **p_ssi
);

/** Prepares to read from a satellite's observations by name.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in] name Name of the satellite to read from.
 * \param[in] code Code for observation to read.  If the observation is
 *   not known for the named satellite, this function fails.
 * \param[in,out] p_rdr Receives a pointer to the signal reader object.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_open_obs_by_name(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    struct srnx_obs_code code,
    struct srnx_obs_reader **p_rdr
);

/** Prepares to read from a satellite's observations by index.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in] name Name of the satellite to read from.
 * \param[in] obs_idx Index of the observation to read.  If negative or
 *   too large, this function fails.
 * \param[in,out] p_rdr Receives a pointer to the signal reader object.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_open_obs_by_index(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int obs_idx,
    struct srnx_obs_reader **p_rdr
);

/** Reads LLI and SSI from an satellite-observation code reader.
 *
 * \param[in] p_socd Pointer to satellite-observation reader object.
 * \param[out] p_n_values Receives number of SSIs and LLIs decoded.
 * \param[out] p_lli If not NULL, receives pointer to LLIs.
 * \param[out] p_ssi If not NULL, receives pointer to SSIs.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_read_obs_ssi_lli(
    struct srnx_obs_reader *p_socd,
    int *p_n_values,
    char **p_lli,
    char **p_ssi
);

/** Reads the next observation from an observation reader.
 *
 * \param[in] p_socd Pointer to observation-reader object.
 * \param[out] p_value Receives the next observation value.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_read_obs_value(
    struct srnx_obs_reader *p_socd,
    int64_t *p_value
);

/** Unallocates resources used by \a p_socd.
 *
 * \param[in] p_socd Pointer to observation-reader object to free.
 */
void srnx_free_obs_reader(
    struct srnx_obs_reader *p_socd
);

/** Converts a vector of int64_t to a vector of double¸ in place.
 *
 * This function is specific to RINEX-type data: in addition to scaling
 * the values, it is only guaranteed for inputs in the range [-2**51,
 * +2**51].
 *
 * \$ ((double *)s64)[ii] = ((int64_t *)s64)[ii] * (scale / 1000.0) \$,
 * for 0 <= ii < \a count.
 * 
 * \param [in,out] s64 Pointer to array of values to convert.
 * \param[in] count Number of values to convert.
 * \param[in] scale Scaling factor times 1000.
 * \warning Undefined behavior if \a count is negative.
 */
void srnx_convert_s64_to_double(
    void *s64,
    int count,
    int scale
);

#endif /* !defined(SRNX_H_a2b6e4a7_3fda_4ba2_8ed1_67b906d55b2c) */
