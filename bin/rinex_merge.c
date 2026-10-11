/** rinex_merge.c - Merge consecutive RINEX observation files.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex.h"
#include "rinex/rnx_priv.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Program structure:
 * - Pass 1: open each input (RINEX, CRINEX or SRNX), copy its header,
 *   and read every record to check that it parses and to find its
 *   first and last observation epochs and its sample interval.
 * - Sort the inputs by first observation epoch.
 * - Check that the headers agree, apart from CRINEX lines and labels
 *   that normally vary from file to file, that the inputs share a
 *   sample interval, and (unless -g) that there are no gaps between
 *   consecutive inputs.
 * - Pass 2: write the merged header, then every record from each input
 *   in turn, dropping observation epochs that are not later than the
 *   last epoch already written (overlap between adjacent files).
 *
 * The output is RINEX text with the same major version as the inputs.
 */

/** header_class says how a header line's label is merged. */
enum header_class
{
    /** Must match across inputs; with -f, a mismatch is a warning. */
    HDR_CHECK,
    /** Must match across inputs, even with -f. */
    HDR_FATAL,
    /** Copied from the first input, not compared. */
    HDR_FIRST,
    /** Copied from the first input; only the time system is compared. */
    HDR_TIME,
    /** Replaced by a line describing this program. */
    HDR_PGM,
    /** Omitted from the output. */
    HDR_DROP
};

static const struct
{
    const char *label;
    enum header_class cls;
} header_rules[] = {
    { "CRINEX VERS   / TYPE", HDR_DROP },
    { "CRINEX PROG / DATE", HDR_DROP },
    { "TIME OF LAST OBS", HDR_DROP },
    { "# OF SATELLITES", HDR_DROP },
    { "PRN / # OF OBS", HDR_DROP },
    { "END OF HEADER", HDR_DROP },
    { "PGM / RUN BY / DATE", HDR_PGM },
    { "COMMENT", HDR_FIRST },
    { "TIME OF FIRST OBS", HDR_TIME },
    { "RINEX VERSION / TYPE", HDR_FATAL },
    { "# / TYPES OF OBSERV", HDR_FATAL },
    { "SYS / # / OBS TYPES", HDR_FATAL },
    { "SYS / SCALE FACTOR", HDR_FATAL },
    { "INTERVAL", HDR_FATAL },
};

/** hdr_line is one line of an input's header. */
struct hdr_line
{
    /** Start of the line. */
    const char *text;

    /** Length, excluding the line terminator and trailing blanks. */
    int len;

    /** Label (from column 61), and its length. */
    const char *label;
    int label_len;

    enum header_class cls;
};

/** input describes one input file. */
struct input
{
    /** File name, as given on the command line. */
    const char *name;

    /** Position on the command line, to keep sorting stable. */
    int arg_idx;

    /** File identity, to refuse to overwrite an input. */
    dev_t dev;
    ino_t ino;

    /** Copy of the file header, and its lines. */
    char *header;
    struct hdr_line *lines;
    int n_lines;

    /** Count of observation epochs (flags 0, 1 and 6). */
    int n_epochs;

    /** First and last observation epochs. */
    struct rinex_epoch first, last;
    int64_t first_e7, last_e7;

    /** Smallest positive spacing between observation epochs, in units
     * of 1e-7 s; zero if there are fewer than two distinct epochs.
     */
    int64_t min_dt;

    /** Observation epochs and special events written, and dropped
     * because they overlap the previous input.
     */
    int n_written, n_dropped, n_events_dropped;
};

static const char *argv0 = "rinex-merge";
static int verbose;
static int force;
static int allow_gaps;

/* ---- Input streams ---- */

/** Open a parser for \a name.  Returns NULL (after reporting why) on
 * failure.  The caller destroys both the parser and *p_stream.
 */
static struct rinex_parser *open_input(
    const char *name,
    struct rinex_stream **p_stream
)
{
    struct rinex_parser *p = NULL;
    const char *err;

    *p_stream = rinex_mmap_stream(name);
    if (!*p_stream)
    {
        fprintf(stderr, "%s: unable to open %s: %s\n", argv0, name,
            strerror(errno));
        return NULL;
    }

    err = rinex_open(&p, *p_stream);
    if (err)
    {
        fprintf(stderr, "%s: unable to parse %s: %s\n", argv0, name, err);
        (*p_stream)->destroy(*p_stream);
        *p_stream = NULL;
        return NULL;
    }

    return p;
}

/* ---- Utility functions ---- */

/** Is \a flag the epoch flag for a record with observations? */
static int is_obs_flag(char flag)
{
    return flag == '0' || flag == '1' || flag == '6';
}

/** Format \a ep for diagnostic messages. */
static const char *epoch_str(const struct rinex_epoch *ep, char buf[40])
{
    snprintf(buf, 40, "%04d-%02d-%02d %02d:%02d:%02d.%07d",
        ep->yyyy_mm_dd / 10000, ep->yyyy_mm_dd / 100 % 100,
        ep->yyyy_mm_dd % 100, ep->hh_mm / 100, ep->hh_mm % 100,
        ep->sec_e7 / 10000000, ep->sec_e7 % 10000000);
    return buf;
}

/* ---- Header handling ---- */

/** Classify the header line \a line by its label. */
static enum header_class classify(const struct hdr_line *line)
{
    size_t ii, len;

    for (ii = 0; ii < sizeof header_rules / sizeof header_rules[0]; ++ii)
    {
        len = strlen(header_rules[ii].label);
        if ((size_t)line->label_len == len
            && !memcmp(line->label, header_rules[ii].label, len))
        {
            return header_rules[ii].cls;
        }
    }

    return HDR_CHECK;
}

/** Copy \a p's header into \a in and split it into lines.
 * Returns zero on success, non-zero on allocation failure.
 */
static int split_header(struct input *in, const struct rinex_parser *p)
{
    struct hdr_line *line;
    const char *eol;
    int ofs, n_alloc, len;

    in->header = malloc(p->buffer_len + 1);
    if (!in->header)
        return 1;
    memcpy(in->header, p->buffer, p->buffer_len);
    in->header[p->buffer_len] = '\0';

    n_alloc = 0;
    for (ofs = 0; ofs < p->buffer_len; ofs += len + (eol != NULL))
    {
        eol = memchr(in->header + ofs, '\n', p->buffer_len - ofs);
        len = eol ? (int)(eol - (in->header + ofs)) : p->buffer_len - ofs;

        if (in->n_lines == n_alloc)
        {
            n_alloc = n_alloc ? 2 * n_alloc : 64;
            line = realloc(in->lines, n_alloc * sizeof *in->lines);
            if (!line)
                return 1;
            in->lines = line;
        }

        line = &in->lines[in->n_lines++];
        line->text = in->header + ofs;
        line->len = len;
        while (line->len > 0 && (line->text[line->len - 1] == ' '
            || line->text[line->len - 1] == '\r'))
        {
            --line->len;
        }
        line->label = line->len > 60 ? line->text + 60 : "";
        line->label_len = line->len > 60 ? line->len - 60 : 0;
        line->cls = classify(line);
    }

    return 0;
}

/** Do header lines \a aa and \a bb share a label? */
static int same_label(const struct hdr_line *aa, const struct hdr_line *bb)
{
    return aa->label_len == bb->label_len
        && !memcmp(aa->label, bb->label, aa->label_len);
}

/** Do header lines \a aa and \a bb (with the same label) agree? */
static int lines_agree(const struct hdr_line *aa, const struct hdr_line *bb)
{
    int ii;

    if (aa->cls == HDR_TIME)
    {
        /* Only compare the time system, columns 49-51. */
        for (ii = 48; ii < 51; ++ii)
        {
            char ca = ii < aa->len ? aa->text[ii] : ' ';
            char cb = ii < bb->len ? bb->text[ii] : ' ';
            if (ca != cb)
                return 0;
        }
        return 1;
    }

    return aa->len == bb->len && !memcmp(aa->text, bb->text, aa->len);
}

/** Find the next line at or after \a *idx in \a in that has the same
 * label as \a key, and advance \a *idx past it.  Returns NULL if there
 * is no such line.
 */
static const struct hdr_line *next_with_label(
    const struct input *in,
    const struct hdr_line *key,
    int *idx
)
{
    while (*idx < in->n_lines)
    {
        const struct hdr_line *line = &in->lines[(*idx)++];
        if (same_label(line, key))
            return line;
    }

    return NULL;
}

/** Is \a line the first line in \a in with its label? */
static int first_with_label(const struct input *in, const struct hdr_line *line)
{
    int ii;

    for (ii = 0; &in->lines[ii] != line; ++ii)
    {
        if (same_label(&in->lines[ii], line))
            return 0;
    }

    return 1;
}

/** Compare the lines labelled like \a key in \a ref and \a in, and
 * report any difference unless \a quiet.  Returns 1 if the difference
 * is an error.
 */
static int compare_label(
    const struct input *ref,
    const struct input *in,
    const struct hdr_line *key,
    int quiet
)
{
    const struct hdr_line *la, *lb;
    int ia = 0, ib = 0, fatal;

    do
    {
        la = next_with_label(ref, key, &ia);
        lb = next_with_label(in, key, &ib);
        if (!la && !lb)
            return 0;
    } while (la && lb && lines_agree(la, lb));

    fatal = key->cls == HDR_FATAL || !force;
    if (quiet)
        return fatal;
    fprintf(stderr, "%s: %s: %s header \"%.*s\" differs from %s\n",
        argv0, fatal ? "error" : "warning", in->name, key->label_len,
        key->label, ref->name);
    if (la)
        fprintf(stderr, "  %s: %.*s\n", ref->name, la->len, la->text);
    else
        fprintf(stderr, "  %s: (missing)\n", ref->name);
    if (lb)
        fprintf(stderr, "  %s: %.*s\n", in->name, lb->len, lb->text);
    else
        fprintf(stderr, "  %s: (missing)\n", in->name);

    return fatal;
}

/** Is \a cls a header class whose lines are compared across inputs? */
static int is_compared(enum header_class cls)
{
    return cls == HDR_CHECK || cls == HDR_FATAL || cls == HDR_TIME;
}

/** Check that \a in's header agrees with \a ref's, describing each
 * difference unless \a quiet.  Returns the number of errors found.
 */
static int check_header(
    const struct input *ref,
    const struct input *in,
    int quiet
)
{
    const struct hdr_line *line;
    int ii, jj, n_errors = 0;

    /* Labels that are in the reference header. */
    for (ii = 0; ii < ref->n_lines; ++ii)
    {
        line = &ref->lines[ii];
        if (is_compared(line->cls) && first_with_label(ref, line))
            n_errors += compare_label(ref, in, line, quiet);
    }

    /* Labels that are only in the other header. */
    for (ii = 0; ii < in->n_lines; ++ii)
    {
        line = &in->lines[ii];
        if (!is_compared(line->cls) || !first_with_label(in, line))
            continue;
        jj = 0;
        if (!next_with_label(ref, line, &jj))
            n_errors += compare_label(ref, in, line, quiet);
    }

    return n_errors;
}

/** Return the RINEX major version from \a in's header, or 0. */
static int rinex_major_version(const struct input *in)
{
    int ii, jj;

    for (ii = 0; ii < in->n_lines; ++ii)
    {
        const struct hdr_line *line = &in->lines[ii];
        if (line->cls != HDR_FATAL || line->label_len != 20
            || memcmp(line->label, "RINEX VERSION / TYPE", 20))
        {
            continue;
        }
        for (jj = 0; jj < 9 && jj < line->len; ++jj)
        {
            if (line->text[jj] >= '2' && line->text[jj] <= '4')
                return line->text[jj] - '0';
        }
        return 0;
    }

    return 0;
}

/** Write the "PGM / RUN BY / DATE" line for the merged file.
 * Honors SOURCE_DATE_EPOCH, for reproducible output.
 */
static void write_pgm_line(FILE *out)
{
    const char *sde;
    char date[32];
    struct tm tm;
    time_t now;

    sde = getenv("SOURCE_DATE_EPOCH");
    now = sde ? (time_t)strtoll(sde, NULL, 10) : time(NULL);
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%Y%m%d %H%M%S UTC", &tm);
    fprintf(out, "%-20s%-20s%-20s%s\n", "rinex-merge", "", date,
        "PGM / RUN BY / DATE");
}

/** Write the merged header, based on \a ref's header. */
static void write_header(FILE *out, const struct input *ref)
{
    const struct hdr_line *line;
    int ii, wrote_pgm = 0;

    for (ii = 0; ii < ref->n_lines; ++ii)
    {
        line = &ref->lines[ii];
        switch (line->cls)
        {
        case HDR_DROP:
            continue;
        case HDR_PGM:
            if (!wrote_pgm)
                write_pgm_line(out);
            wrote_pgm = 1;
            continue;
        default:
            fprintf(out, "%.*s\n", line->len, line->text);
            break;
        }

        /* PGM / RUN BY / DATE is required; add one if the input had none. */
        if (!wrote_pgm && line->cls == HDR_FATAL && line->label_len == 20
            && !memcmp(line->label, "RINEX VERSION / TYPE", 20))
        {
            write_pgm_line(out);
            wrote_pgm = 1;
        }
    }

    fprintf(out, "%60s%s\n", "", "END OF HEADER");
}

/* ---- Record output ---- */

/** Write \a value, scaled by 10^\a frac_digits, as a fixed-point
 * number right-justified in \a width columns (plus a NUL).
 * Returns zero on success, or -1 if the value does not fit.
 */
static int format_fixed(char *buf, int width, int64_t value, int frac_digits)
{
    char tmp[32];
    uint64_t mag = value < 0 ? -(uint64_t)value : (uint64_t)value;
    uint64_t scale = 1;
    int ii, len;

    for (ii = 0; ii < frac_digits; ++ii)
        scale *= 10;
    len = snprintf(tmp, sizeof tmp, "%s%" PRIu64 ".%0*" PRIu64,
        value < 0 ? "-" : "", mag / scale, frac_digits, mag % scale);
    if (len < 0 || len > width)
        return -1;
    memset(buf, ' ', width - len);
    memcpy(buf + width - len, tmp, len + 1);
    return 0;
}

/** Format one observation as a 16-character F14.3,A1,A1 record.
 * Missing or out-of-range values are written as blanks.
 */
static void format_obs(char buf[16], int64_t obs, char lli, char ssi)
{
    uint64_t mag;
    int pos, ii;

    if (obs < RINEX_MIN_OBS || obs > RINEX_MAX_OBS
        || (obs == 0 && lli == ' ' && ssi == ' '))
    {
        memset(buf, ' ', 16);
        return;
    }

    buf[14] = lli;
    buf[15] = ssi;
    mag = obs < 0 ? -(uint64_t)obs : (uint64_t)obs;
    pos = 13;
    for (ii = 0; ii < 3; ++ii, mag /= 10)
        buf[pos--] = '0' + mag % 10;
    buf[pos--] = '.';
    do
    {
        buf[pos--] = '0' + mag % 10;
        mag /= 10;
    } while (mag > 0);
    if (obs < 0)
        buf[pos--] = '-';
    while (pos >= 0)
        buf[pos--] = ' ';
}

/** Write \a len bytes of \a line, without trailing blanks, then '\n'. */
static void write_rtrim(FILE *out, const char *line, int len)
{
    while (len > 0 && line[len - 1] == ' ')
        --len;
    fwrite(line, 1, len, out);
    fputc('\n', out);
}

/** Write \a p's current observation epoch in RINEX 3/4 format.
 * Returns zero on success, or -1 (writing nothing) if the seconds or
 * clock offset do not fit their fields.
 */
static int write_v34_epoch(FILE *out, const struct rinex_parser *p)
{
    static char line[3 + 16 * 256];
    const struct rinex_epoch *ep = &p->epoch;
    char sec[16], clk[24];
    int ii, jj, len;

    if (format_fixed(sec, 11, ep->sec_e7, 7)
        || (ep->clock_offset != 0
            && format_fixed(clk, 15, ep->clock_offset, 12)))
    {
        return -1;
    }
    fprintf(out, "> %04d %02d %02d %02d %02d%s  %c%3d",
        ep->yyyy_mm_dd / 10000, ep->yyyy_mm_dd / 100 % 100,
        ep->yyyy_mm_dd % 100, ep->hh_mm / 100, ep->hh_mm % 100, sec,
        ep->flag, ep->n_sats);
    if (ep->clock_offset != 0)
        fprintf(out, "      %s", clk);
    fputc('\n', out);

    for (ii = 0; ii < ep->n_sats; ++ii)
    {
        const struct rinex_sv_info *sv = &p->sats[ii];
        int n_obs = p->n_obs[sv->system & 31];

        line[0] = sv->system;
        line[1] = '0' + sv->number / 10;
        line[2] = '0' + sv->number % 10;
        len = 3;
        for (jj = 0; jj < n_obs; ++jj, len += 16)
        {
            format_obs(line + len, p->obs[sv->obs_0 + jj],
                p->lli[sv->obs_0 + jj], p->ssi[sv->obs_0 + jj]);
        }
        write_rtrim(out, line, len);
    }
    return 0;
}

/** Write \a p's current observation epoch in RINEX 2 format.
 * Returns zero on success, or -1 (writing nothing) if the seconds or
 * clock offset do not fit their fields.
 */
static int write_v2_epoch(FILE *out, const struct rinex_parser *p)
{
    char line[5 * 16];
    const struct rinex_epoch *ep = &p->epoch;
    char sec[16], clk[24];
    int ii, jj, kk, n_first;

    if (format_fixed(sec, 11, ep->sec_e7, 7)
        || (ep->clock_offset != 0
            && format_fixed(clk, 12, ep->clock_offset / 1000, 9)))
    {
        return -1;
    }
    fprintf(out, " %02d %2d %2d %2d %2d%s  %c%3d",
        ep->yyyy_mm_dd / 10000 % 100, ep->yyyy_mm_dd / 100 % 100,
        ep->yyyy_mm_dd % 100, ep->hh_mm / 100, ep->hh_mm % 100, sec,
        ep->flag, ep->n_sats);

    /* Up to 12 satellites on the first line, then the clock offset
     * (F12.9) in columns 69-80.
     */
    n_first = ep->n_sats < 12 ? ep->n_sats : 12;
    for (ii = 0; ii < n_first; ++ii)
        fprintf(out, "%c%02d", p->sats[ii].system, p->sats[ii].number);
    if (ep->clock_offset != 0)
        fprintf(out, "%*s%s", 3 * (12 - n_first), "", clk);
    fputc('\n', out);

    /* Continuation lines, indented 32 columns. */
    for (ii = 12; ii < ep->n_sats; ii += 12)
    {
        fprintf(out, "%32s", "");
        for (jj = ii; jj < ep->n_sats && jj < ii + 12; ++jj)
            fprintf(out, "%c%02d", p->sats[jj].system, p->sats[jj].number);
        fputc('\n', out);
    }

    /* Five observations per line. */
    for (ii = 0; ii < ep->n_sats; ++ii)
    {
        const struct rinex_sv_info *sv = &p->sats[ii];
        int n_obs = p->n_obs[sv->system & 31];

        for (jj = 0; jj < n_obs; jj += 5)
        {
            for (kk = 0; kk < 5 && jj + kk < n_obs; ++kk)
            {
                int idx = sv->obs_0 + jj + kk;
                format_obs(line + 16 * kk, p->obs[idx], p->lli[idx],
                    p->ssi[idx]);
            }
            write_rtrim(out, line, 16 * kk);
        }
    }
    return 0;
}

/** Does special event record text \a text carry an epoch timestamp? */
static int event_has_time(const char *text, int len, int version)
{
    /* The last digit of the year: column 3 (v2) or 6 (v3/v4). */
    int col = version == 2 ? 2 : 5;
    return len > col && text[col] >= '0' && text[col] <= '9';
}

/* ---- Passes over the input ---- */

/** Read all of \a in, recording its header and epoch statistics.
 * Returns zero on success.
 */
static int scan_input(struct input *in)
{
    struct rinex_stream *stream;
    struct rinex_parser *p;
    struct stat st;
    rinex_error_t res;
    int64_t t_e7;
    int status = 0;

    if (stat(in->name, &st))
    {
        fprintf(stderr, "%s: unable to open %s: %s\n", argv0, in->name,
            strerror(errno));
        return 1;
    }
    in->dev = st.st_dev;
    in->ino = st.st_ino;

    p = open_input(in->name, &stream);
    if (!p)
        return 1;

    if (split_header(in, p))
    {
        fprintf(stderr, "%s: out of memory\n", argv0);
        status = 1;
        goto out;
    }

    while ((res = p->read(p)) == RINEX_SUCCESS)
    {
        if (!is_obs_flag(p->epoch.flag))
            continue;
        t_e7 = rnx_epoch_time_e7(&p->epoch);
        if (in->n_epochs == 0)
        {
            in->first = p->epoch;
            in->first_e7 = t_e7;
        }
        else if (t_e7 > in->last_e7
            && (in->min_dt == 0 || t_e7 - in->last_e7 < in->min_dt))
        {
            in->min_dt = t_e7 - in->last_e7;
        }
        in->last = p->epoch;
        in->last_e7 = t_e7;
        ++in->n_epochs;
    }
    if (res != RINEX_EOF)
    {
        fprintf(stderr, "%s: %s: read error: %s\n", argv0, in->name,
            p->errmsg[0] ? p->errmsg : "unknown error");
        status = 1;
    }

out:
    p->destroy(p);
    stream->destroy(stream);
    return status;
}

/** Copy the records of \a in to \a out.  \a last_e7 is the time of the
 * latest observation epoch written, or INT64_MIN if none has been.
 * Records no later than that, as it was before this input, overlap an
 * earlier input and are dropped; records within this input are kept
 * even if they share a timestamp (for example, a flag-6 cycle-slip
 * record or a special event at the same time as an observation epoch).
 * Returns zero on success.
 */
static int merge_input(
    FILE *out,
    struct input *in,
    int version,
    int64_t *last_e7
)
{
    struct rinex_stream *stream;
    struct rinex_parser *p;
    rinex_error_t res;
    int64_t t_e7, cutoff = *last_e7;
    int overlap = 0, status = 0;

    p = open_input(in->name, &stream);
    if (!p)
        return 1;

    while ((res = p->read(p)) == RINEX_SUCCESS)
    {
        if (is_obs_flag(p->epoch.flag))
        {
            t_e7 = rnx_epoch_time_e7(&p->epoch);
            overlap = t_e7 <= cutoff;
            if (overlap)
            {
                ++in->n_dropped;
                continue;
            }
            if (version == 2 ? write_v2_epoch(out, p)
                : write_v34_epoch(out, p))
            {
                fprintf(stderr, "%s: %s: epoch at %08d %04d has a time or"
                    " clock offset too wide for RINEX %d\n", argv0, in->name,
                    p->epoch.yyyy_mm_dd, p->epoch.hh_mm, version);
                status = 1;
                break;
            }
            if (t_e7 > *last_e7)
                *last_e7 = t_e7;
            ++in->n_written;
            continue;
        }

        /* A special event: drop it if it falls in an overlap.  An event
         * without a timestamp goes with the preceding epoch.
         */
        if (event_has_time(p->buffer, p->buffer_len, version)
            ? rnx_epoch_time_e7(&p->epoch) <= cutoff : overlap)
        {
            ++in->n_events_dropped;
            continue;
        }
        fwrite(p->buffer, 1, p->buffer_len, out);
    }
    if (!status && res != RINEX_EOF)
    {
        fprintf(stderr, "%s: %s: read error: %s\n", argv0, in->name,
            p->errmsg[0] ? p->errmsg : "unknown error");
        status = 1;
    }

    p->destroy(p);
    stream->destroy(stream);
    return status;
}

/** qsort() comparator: order inputs by first observation epoch, then
 * by command-line position.
 */
static int input_cmp(const void *va, const void *vb)
{
    const struct input *aa = va, *bb = vb;

    if (aa->first_e7 != bb->first_e7)
        return aa->first_e7 < bb->first_e7 ? -1 : 1;
    return aa->arg_idx - bb->arg_idx;
}

/** Check that the inputs that have a sample interval agree on it.
 * Returns the number of errors found.
 */
static int check_intervals(const struct input *inputs, int n_inputs)
{
    const struct input *ref = NULL;
    int ii, fatal, n_errors = 0;

    for (ii = 0; ii < n_inputs; ++ii)
    {
        if (!inputs[ii].min_dt)
            continue;
        if (!ref)
        {
            ref = &inputs[ii];
            continue;
        }
        if (inputs[ii].min_dt == ref->min_dt)
            continue;
        fatal = !force;
        n_errors += fatal;
        fprintf(stderr, "%s: %s: %s has a %g s sample interval but %s has"
            " %g s\n", argv0, fatal ? "error" : "warning", inputs[ii].name,
            inputs[ii].min_dt / 1e7, ref->name, ref->min_dt / 1e7);
    }

    return n_errors;
}

/** Return the sample interval declared by \a in's INTERVAL header, in
 * units of 1e-7 s, or zero if there is none.
 */
static int64_t declared_interval(const struct input *in)
{
    char field[11];
    double secs;
    int ii;

    for (ii = 0; ii < in->n_lines; ++ii)
    {
        const struct hdr_line *line = &in->lines[ii];
        if (line->label_len != 8 || memcmp(line->label, "INTERVAL", 8))
            continue;
        memcpy(field, line->text, 10);
        field[10] = '\0';
        secs = strtod(field, NULL);
        return secs > 0 ? (int64_t)(secs * 1e7 + 0.5) : 0;
    }

    return 0;
}

/** Return nonzero if \a path names the same file as any input. */
static int is_an_input(
    const char *path,
    const struct input *inputs,
    int n_inputs
)
{
    struct stat st;
    int ii;

    /* stat() follows symbolic links; hard links share an inode. */
    if (stat(path, &st))
        return 0;
    for (ii = 0; ii < n_inputs; ++ii)
    {
        if (inputs[ii].dev == st.st_dev && inputs[ii].ino == st.st_ino)
            return 1;
    }

    return 0;
}

/** Check that each input with epochs starts no more than \a interval
 * after the latest epoch of the inputs before it.  Returns the number
 * of errors found.
 */
static int check_gaps(const struct input *inputs, int n_inputs, int64_t interval)
{
    const struct input *prev = NULL;
    char first_buf[40], last_buf[40];
    int ii, n_errors = 0;

    for (ii = 0; ii < n_inputs; ++ii)
    {
        const struct input *in = &inputs[ii];

        if (in->n_epochs == 0)
            continue;
        if (prev && in->first_e7 - prev->last_e7 > interval)
        {
            if (!allow_gaps || verbose)
            {
                fprintf(stderr, "%s: %s: gap from %s (end of %s) to %s"
                    " (start of %s)\n", argv0,
                    allow_gaps ? "note" : "error",
                    epoch_str(&prev->last, last_buf), prev->name,
                    epoch_str(&in->first, first_buf), in->name);
            }
            n_errors += !allow_gaps;
        }
        if (!prev || in->last_e7 > prev->last_e7)
            prev = in;
    }

    return n_errors;
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: %s [-f] [-g] [-v] [-o outfile] infile1 infile2 ...\n"
        "Merges consecutive RINEX, CRINEX or SRNX observation files from one\n"
        "station into a single RINEX file (written to stdout by default).\n"
        "  -f          Warn about, rather than reject, inconsistent headers\n"
        "              or sample intervals (observation types must match).\n"
        "  -g          Allow gaps between consecutive input files.\n"
        "  -o outfile  Write to outfile instead of stdout.\n"
        "  -v          Report a summary of each input on stderr.\n",
        argv0);
}

int main(int argc, char *argv[])
{
    struct input *inputs;
    const char *out_name = NULL;
    char first_buf[40], last_buf[40];
    FILE *out;
    int64_t last_e7, interval;
    int ii, opt, n_inputs, n_errors, version;

    if (argc > 1 && !strcmp(argv[1], "--help"))
    {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    while ((opt = getopt(argc, argv, "fgho:v")) != -1)
    {
        switch (opt)
        {
        case 'f':
            force = 1;
            break;
        case 'g':
            allow_gaps = 1;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        case 'o':
            out_name = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (optind >= argc)
    {
        usage(stderr);
        return EXIT_FAILURE;
    }

    /* Pass 1: read and check every input before writing anything. */
    n_inputs = argc - optind;
    inputs = calloc(n_inputs, sizeof *inputs);
    if (!inputs)
    {
        fprintf(stderr, "%s: out of memory\n", argv0);
        return EXIT_FAILURE;
    }
    for (ii = 0, n_errors = 0; ii < n_inputs; ++ii)
    {
        inputs[ii].name = argv[optind + ii];
        inputs[ii].arg_idx = ii;
        n_errors += scan_input(&inputs[ii]);
        if (inputs[ii].n_epochs == 0)
            inputs[ii].first_e7 = inputs[ii].last_e7 = INT64_MAX;
    }
    if (n_errors)
        return EXIT_FAILURE;

    qsort(inputs, n_inputs, sizeof *inputs, input_cmp);
    if (inputs[0].n_epochs == 0)
    {
        fprintf(stderr, "%s: no input has any observation epochs\n", argv0);
        return EXIT_FAILURE;
    }
    version = rinex_major_version(&inputs[0]);
    if (version == 0)
    {
        fprintf(stderr, "%s: %s: unrecognized RINEX version\n", argv0,
            inputs[0].name);
        return EXIT_FAILURE;
    }
    for (ii = 1; ii < n_inputs; ++ii)
    {
        /* Describe the differences for the first inconsistent input,
         * then just name the others.
         */
        int nn = check_header(&inputs[0], &inputs[ii], n_errors > 0);
        if (nn && n_errors)
        {
            fprintf(stderr, "%s: error: %s header also differs from %s\n",
                argv0, inputs[ii].name, inputs[0].name);
        }
        n_errors += nn;
    }
    n_errors += check_intervals(inputs, n_inputs);
    if (n_errors)
    {
        fprintf(stderr, "%s: not merging inconsistent inputs%s\n", argv0,
            force ? "" : " (-f relaxes some checks)");
        return EXIT_FAILURE;
    }
    /* Prefer the observed sample interval; the INTERVAL header (which
     * must agree across inputs) covers inputs with single epochs.
     */
    for (ii = 0, interval = 0; ii < n_inputs && !interval; ++ii)
        interval = inputs[ii].min_dt;
    if (!interval)
        interval = declared_interval(&inputs[0]);
    if (!interval && !allow_gaps && n_inputs > 1)
    {
        fprintf(stderr, "%s: warning: no sample interval is known, so gaps"
            " between inputs cannot be detected\n", argv0);
    }
    if (interval && check_gaps(inputs, n_inputs, interval))
    {
        fprintf(stderr, "%s: not merging non-consecutive inputs"
            " (-g allows gaps)\n", argv0);
        return EXIT_FAILURE;
    }

    /* Pass 2: write the merged file. */
    if (!out_name || !strcmp(out_name, "-"))
    {
        out = stdout;
        out_name = NULL;
    }
    else if (is_an_input(out_name, inputs, n_inputs))
    {
        fprintf(stderr, "%s: output %s is one of the inputs\n", argv0,
            out_name);
        return EXIT_FAILURE;
    }
    else
    {
        out = fopen(out_name, "w");
        if (!out)
        {
            fprintf(stderr, "%s: unable to create %s: %s\n", argv0, out_name,
                strerror(errno));
            return EXIT_FAILURE;
        }
    }
    setvbuf(out, NULL, _IOFBF, 1 << 20);

    write_header(out, &inputs[0]);
    last_e7 = INT64_MIN;
    for (ii = 0; ii < n_inputs && !n_errors; ++ii)
        n_errors += merge_input(out, &inputs[ii], version, &last_e7);

    if (fflush(out) || ferror(out))
    {
        fprintf(stderr, "%s: error writing %s: %s\n", argv0,
            out_name ? out_name : "stdout", strerror(errno));
        n_errors++;
    }
    if (out_name && fclose(out))
    {
        fprintf(stderr, "%s: error closing %s: %s\n", argv0, out_name,
            strerror(errno));
        n_errors++;
    }
    if (n_errors)
    {
        if (out_name)
            remove(out_name);
        return EXIT_FAILURE;
    }

    for (ii = 0; ii < n_inputs; ++ii)
    {
        const struct input *in = &inputs[ii];

        if (in->n_epochs > 0 && in->n_written == 0)
        {
            fprintf(stderr, "%s: warning: every epoch of %s duplicates an"
                " earlier input\n", argv0, in->name);
        }
        if (!verbose)
            continue;
        if (in->n_epochs == 0)
        {
            fprintf(stderr, "%s: no observation epochs\n", in->name);
            continue;
        }
        fprintf(stderr, "%s: %s to %s, %d epochs written",
            in->name, epoch_str(&in->first, first_buf),
            epoch_str(&in->last, last_buf), in->n_written);
        if (in->n_dropped || in->n_events_dropped)
        {
            fprintf(stderr, ", %d overlapping epochs and %d events dropped",
                in->n_dropped, in->n_events_dropped);
        }
        fputc('\n', stderr);
    }

    return EXIT_SUCCESS;
}
