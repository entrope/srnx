/** srnx.h - Succinct RINEX reader API.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(SRNX_H_a2b6e4a7_3fda_4ba2_8ed1_67b906d55b2c)
#define SRNX_H_a2b6e4a7_3fda_4ba2_8ed1_67b906d55b2c

#include <stddef.h>

#include "rinex_epoch.h"

/* Reader objects are library-owned: release them with srnx_close() or
 * srnx_free_obs_reader().  Output arrays are allocated/reallocated by the
 * library and owned by the caller, who releases them with free().  Array
 * pointers passed by address must initially be NULL or point to compatible
 * malloc storage.  The library and caller must use the same C allocator.
 * Header and event text pointers are borrowed until the reader is closed
 * or reopened; do not free them.
 */

/** srnx_reader represents a SRNX stream reader. */
struct srnx_reader;

/** srnx_obs_reader is used to read a particular `SOCD` chunk.
 * One SOCD chunk contains the observation data for a single (satellite,
 * observation code) pair.
 */
struct srnx_obs_reader;

struct rinex_data;

/** srnx_load_slot holds per-signal load state for srnx_get_obs_by_index()
 * and srnx_get_obs_by_name().  Caller-allocated and zero-initialized;
 * on success each slot is populated, and the caller is responsible for
 * freeing #obs, #lli, and #ssi.
 */
struct srnx_load_slot
{
    /** n_values is the number of observations loaded for this signal. */
    int n_values;

    /** obs holds library-allocated observation values, freed by the caller. */
    int64_t *obs;

    /** lli holds the loss-of-lock indicators, length #n_values. */
    char *lli;

    /** ssi holds the signal-strength indicators, length #n_values. */
    char *ssi;
};

#define SRNX_OPEN_UNCHECKED 1

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
     * initialize them to '\0', and callees can ignore the actual value.)
     */
    char name[4];
};

/** Retrieves the latest error message for an SRNX reader.
 *
 * \param[in] srnx SRNX reader object.
 * \returns Text error message, or NULL if no error has been raised.
 */
const char *srnx_error(const struct srnx_reader *srnx);

/** Opens a new SRNX reader by file name.
 *
 * \param[in,out] p_srnx Pointer to SRNX reader object.  If this is not
 *   null on entry, the old object is destroyed.
 * \param[in] filename Name of SRNX file on a local filesystem.
 * \returns Zero on success, non-zero on error.
 */
int srnx_open(struct srnx_reader **p_srnx, const char *filename, int flags);

/** Releases a reader, including after a failed open.  NULL is allowed.
 * Free observation readers before closing or reopening their parent.
 * Caller-owned output arrays remain valid and must be freed separately.
 */
void srnx_close(struct srnx_reader *srnx);

/** Loads the RINEX header from a SRNX file.
 *
 * \param[in] srnx SRNX reader object.
 * \param[out] p_rhdr Receives pointer to RINEX header text.
 * \param[out] rhdr_len Receives number of bytes valid in \a *p_rhdr.
 * \returns Zero on success, non-zero on error.
 */
int srnx_get_header(
    struct srnx_reader *srnx,
    const char **p_rhdr,
    size_t *rhdr_len
);

/** Loads the RINEX epoch values from a SRNX file.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in,out] p_epoch Receives pointer to epoch structures.
 * \param[out] p_epochs_len Number of epochs on success; zero on failure.
 * \returns Zero on success, non-zero on error.
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
 * \returns Zero on success, non-zero on error.
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
 * \param[in,out] slots Receives per-signal load state.  Caller allocates
 *   and zero-initializes \a codes_len elements; \a slots[0] through
 *   \a slots[codes_len-1] are valid on success.  On failure, slots already
 *   populated in this call are freed and reset to zero.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_obs_by_name(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int codes_len,
    struct srnx_obs_code code[],
    struct srnx_load_slot slots[]
);

/** Loads all available observation values for a given satellite,
 * selected by observation index or indices.
 *
 * \param[in] srnx SRNX reader object.
 * \param[in] name Name of the satellite to load.
 * \param[in] idx_len Number of observation codes to load.
 * \param[in] idx Indices of observation codes to load.  If any observation
 *   code is larger than the number of codes for this satellite system,
 *   this function fails.
 * \param[in,out] slots Receives per-signal load state.  Caller allocates
 *   and zero-initializes \a idx_len elements; \a slots[0] through
 *   \a slots[idx_len-1] are valid on success.  On failure, slots already
 *   populated in this call are freed and reset to zero.
 * \returns Zero on success, non-zero SRNX error number on error.
 */
int srnx_get_obs_by_index(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int idx_len,
    int idx[],
    struct srnx_load_slot slots[]
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
void srnx_free_obs_reader(struct srnx_obs_reader *p_socd);

/** Verifies the file-level digest, if the SRNX file carries one.
 *
 * The digest is computed over all bytes from just after the SRNX chunk
 * (including its own trailing chunk digest) through the end of the file
 * excluding the file-level digest itself, then compared to the trailing
 * bytes.
 *
 * \param[in] srnx SRNX reader object.
 * \returns Zero on success (including when no file digest is present),
 *   \a SRNX_BAD_DIGEST on mismatch, or another non-zero error code on
 *   other failures.
 */
int srnx_verify_file_digest(struct srnx_reader *srnx);

/** Loads an SRNX file into a rinex_data structure.
 *
 * \param[in] filename Path to the SRNX file.
 * \param[out] out Receives the loaded data.
 * \returns Zero on success, else a non-zero error code (populating
 *   \a out->error).
 */
int srnx_load(const char *filename, struct rinex_data *out);

#endif /* !defined(SRNX_H_a2b6e4a7_3fda_4ba2_8ed1_67b906d55b2c) */
