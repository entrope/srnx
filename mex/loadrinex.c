/** loadrinex.c - MEX interface to RINEX parsing library.
 * Copyright 2024 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Usage:
 *  obs = loadrinex(filename[, mode='loose'])
 * OR
 *  [obs, hdr] = loadrinex(filename[, mode='loose'])
 *
 * This function loads an observation data file into a structure array,
 * with a second optional output to get the file header.
 *
 * FILENAME is the name of the RINEX file to load.
 *
 * MODE can be one of:
 *  1, 'loose'- Row vector; observations are indexed by record within
 *      the file (usually one record per N seconds where N is the file's
 *      INTERVAL)
 *  2, 'packed' - Row vector; only non-blank (but possibly zero)
 *      observations are stored
 *  3, 'pass' - Matrix, one row per satellite pass (with 60 seconds of
 *      no observations marking the end of a pass); observations are
 *      indexed relative to the start of the pass
 *
 * HDR is a 2xN cell matrix where HDR(1,II) is the content of the header
 * line (60 characters) and HDR(2,II) is the header label (up to 20
 * characters).
 *
 * OBS is a row structure array where the indexing depends on the MODE
 * parameter but the column index always represents one combination of
 * satellite-observable codes.  Within OBS, observations from one
 * satellite will be consecutive; within each satellite, observations
 * will be in the same order as the RINEX file.
 *
 * OBS has the following fields:
 *  sat - Three-character satellite identifier, for example 'G01'
 *  code - Observation code, two characters for RINEX 2 or three
 *      characters for RINEX 3 or RINEX 4
 *  start - Record index within the file of this structure's first row
 *  data - Observation data, column vector of scalar doubles
 *  lli - Link loss indicator, column vector of char
 *  ssi - Signal strength indicator, column vector of char
 *  present - Observation present (indexed by record number), column
 *      vector of logical
 *
 * TODO: Add options: no lli, no ssi, data as int64 (vs double).
 * TODO: Report record metadata (timestamp, nsat, receiver clock offset,
 *     ...) as pseudo-observations.
 * TODO: Report special event records (epoch flag > 1).
 */

#include "io64.h"
#include "mex.h"
#include "rinex/rinex.h"

enum load_mode
{
    INVALID,
    LOOSE,
    PACKED,
    PASS
};

void parse_load_args(const char **pFilename, enum load_mode *mode, int nrhs, const mxArray *prhs[])
{
    mxArray *a;
    double dd;
    mwSize len;
    enum load_mode mode;

    if (nrhs < 1 || nrhs > 2)
    {
        mexErrMsgIdAndText("MDP:loadrinex:rhs", "Requires one to two input arguments");
    }

    /* Get filename argument. */
    a = prhs[0];
    if (!mxIsChar(a))
    {
        mexErrMsgIdAndText("MDP:loadrinex:filename", "Filename must be a string");
    }
    len = mxGetNumberOfElements(a) + 1;
    *pFilename = mxCalloc(len, sizeof char);
    if (0 != mxGetString(a, *pFilename, len))
    {
        mxErrMsgIdAndText("MDP:loadrinex:filename", "Could not convert filename string");
    }

    /* Get mode argument. */
    if (nrhs < 2)
    {
        mode = LOOSE;
    }
    else
    {
        char bb[8];

        a = phrs[1];
        mode = INVALID;
        if (mxIsChar(a))
        {
            if (0 != mxGetString(a, bb, sizeof bb))
            {
                mxErrMsgIdAndText("MDP:loadrinex:mode", "Could not convert mode string");
            }

            if (0 == strcmp(bb, "loose"))
            {
                mode = LOOSE;
            }
            else if (0 == strcmp(bb, "packed"))
            {
                mode = PACKED;
            }
            else if (0 == strcmp(bb, "pass"))
            {
                mode = PASS;
            }
        }
        else if (mxIsNumeric(a))
        {
            dd = mxGetScalar(a);
            switch (int(dd))
            {
            case 1:
                mode = LOOSE;
                break;
            case 2:
                mode = PACKED;
                break;
            case 3:
                mode = PASS;
                break;
            }
        }
        /* else keep mode == INVALID */

        if (mode == INVALID)
        {
            mxErrMsgIdAndText("MDP:loadrinex:mode", "Unknown or invalid mode");
        }
    }
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray *prhs[])
{
    const char *filename, *err;
    struct rinex_stream *rs;
    struct rinex_parser *rp;
    mxArray *str;
    enum load_mode mode;
    rinex_error_t re;
    int ii, jj, sys_idx, sat_idx, sat_ofs, n_recs, nrows, ncols;

    /* Check arity. */
    if (nlhs < 1 || nlhs > 2)
    {
        mexErrMsgIdAndText("MDP:loadrinex:lhs", "Requires one or two output arguments");
    }

    /* Parse input parameters. */
    parse_load_args(&filename, &mode, nrhs, prhs);

    /* Open the file. */
    rp = NULL;
    rs = rinex_mmap_stream(filename);
    err = rinex_open(&rp, rs);
    if (err != NULL)
    {
        rs->destroy(rs);
        mxErrMsgIdAndText("MDP:loadrinex:rinex_open", "%s", err);
    }

    /* Should we (split and) save the file header? */
    if (nlhs > 1)
    {
        const char *hdr, *eol, *end;
        int nlines;
        mxArray *hdrs;

        hdr = rp->buffer;
        end = hdr + rp->buffer_len;
        for (nlines = 0; hdr < end; ++hdr)
        {
            if (*hdr == '\n')
            {
                ++nlines;
            }
        }
        hdrs = mxCreateCellMatrix(nlines, 2);
        for (hdr = rp->buffer, ii = 0; hdr < end; ++ii)
        {
            eol = strchr(hdr, '\n');
            if (!eol || (eol < hdr + 60) || (eol > hdr + 81))
            {
                mxErrMsgIdAndText("MDP:loadrinex:header", "RINEX header seems corrupt");
            }
            mxSetCell(hdrs, ii * 2 + 0, mxCreateStringFromNChars(hdr, 60));
            mxSetCell(hdrs, ii * 2 + 1, mxCreateStringFromNChars(hdr + 60, eol - hdr - 60));
        }
        plhs[1] = hdrs;
    }

    /* Read the records from the file. */
    for (nrows = ncols = n_recs = 0; (re = rp->read(rp)) == RINEX_SUCCESS; )
    {
        if (p->epoch.flag > '1')
        {
            continue;
        }
        for (ii = 0; ii < p->epoch.n_sats; ++ii)
        {
            sys_idx = p->sats[ii].system & 31;
            sat_idx = p->sats[ii].number + sys_idx * 100;
            /* TODO: read the records in the file */
        }
    }

    /* Save the observations to plhs[0]. */
    str = mxCreateStructMatrix(nrows, ncols);
    /* TODO convert the records into phls[0] */

    rp->destroy(rp);
    rs->destroy(rs);
}
