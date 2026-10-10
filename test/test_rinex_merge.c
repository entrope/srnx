/** test/test_rinex_merge.c
 *
 * Tests for rinex-merge.  The RINEX 3 fixture (three epochs, 30 s
 * apart) is first normalized by rinex-merge itself, then split into
 * pieces that are merged back together:
 *
 *  - Overlapping pieces, given out of order, one with CR LF line
 *    endings and different "PGM / RUN BY / DATE" and COMMENT lines,
 *    must merge back to exactly the normalized whole.
 *  - A differing MARKER NAME is rejected, unless -f is given.
 *  - Differing observation types are rejected, even with -f.
 *  - A gap between pieces is rejected, unless -g is given.
 *  - Differing sample intervals are rejected, unless -f is given.
 *  - Single-epoch inputs are checked for gaps using INTERVAL.
 *  - Records within one input that share a timestamp (an observation
 *    epoch, a flag-6 record and a flag-4 event) are all kept.
 *  - An output that is an input (directly, or by a symbolic or hard
 *    link) is refused, and the input is left intact.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tap.h>

#ifndef RINEX_MERGE_PATH
# define RINEX_MERGE_PATH "rinex-merge"
#endif
#ifndef FIXTURE_V3_PATH
# define FIXTURE_V3_PATH "fixture_v3.rnx"
#endif

#define N_EPOCHS 3

static char *header;
static char *epochs[N_EPOCHS];

/** Read a whole file into a NUL-terminated malloc'd buffer. */
static char *slurp(const char *path)
{
    FILE *fin;
    long len;
    char *buf;

    fin = fopen(path, "rb");
    if (!fin)
        return NULL;
    fseek(fin, 0, SEEK_END);
    len = ftell(fin);
    rewind(fin);
    buf = malloc(len + 1);
    if (buf && fread(buf, 1, len, fin) != (size_t)len)
    {
        free(buf);
        buf = NULL;
    }
    if (buf)
        buf[len] = '\0';
    fclose(fin);
    return buf;
}

/** Run rinex-merge with \a args, discarding its stderr.
 * Returns its exit status.
 */
static int run_merge(const char *args)
{
    char cmd[1024];
    int res;

    snprintf(cmd, sizeof cmd, "%s %s 2>/dev/null", RINEX_MERGE_PATH, args);
    res = system(cmd);
    return res == -1 ? -1 : (res >> 8) & 0xff;
}

/** Write \a text to \a fout, optionally turning LF into CR LF. */
static void put_text(FILE *fout, const char *text, int crlf)
{
    for (; *text; ++text)
    {
        if (crlf && *text == '\n')
            fputc('\r', fout);
        fputc(*text, fout);
    }
}

/** Return a copy of \a text with the header line labelled \a label
 * replaced by \a line (which includes its label and newline).
 */
static char *replace_line(const char *text, const char *label, const char *line)
{
    const char *pos = strstr(text, label);
    const char *bol, *eol;
    char *res;

    if (!pos)
        return strdup(text);
    for (bol = pos; bol > text && bol[-1] != '\n'; --bol)
        continue;
    eol = strchr(pos, '\n') + 1;
    res = malloc(strlen(text) + strlen(line) + 1);
    sprintf(res, "%.*s%s%s", (int)(bol - text), text, line, eol);
    return res;
}

/** Return a copy of \a text with \a line inserted before the line
 * labelled \a label.
 */
static char *insert_line(const char *text, const char *label, const char *line)
{
    const char *bol = strstr(text, label);
    char *res;

    while (bol > text && bol[-1] != '\n')
        --bol;
    res = malloc(strlen(text) + strlen(line) + 1);
    sprintf(res, "%.*s%s%s", (int)(bol - text), text, line, bol);
    return res;
}

/** Return a copy of epoch \a idx with its "hh mm ss.sssssss" fields
 * replaced by \a hhmmss.
 */
static char *retime_epoch(int idx, const char *hhmmss)
{
    char *res = strdup(epochs[idx]);

    memcpy(res + 13, hhmmss, 16);
    return res;
}

/** Write a RINEX file from \a hdr and the NULL-terminated list of
 * epoch blocks that follows.
 */
static void write_piece(const char *path, const char *hdr, int crlf, ...)
{
    const char *block;
    va_list args;
    FILE *fout;

    fout = fopen(path, "wb");
    if (!fout)
        BAIL_OUT("unable to create %s", path);
    put_text(fout, hdr, crlf);
    va_start(args, crlf);
    while ((block = va_arg(args, const char *)) != NULL)
        put_text(fout, block, crlf);
    va_end(args);
    fclose(fout);
}

/** Split the normalized RINEX 3 file \a text into header and epochs. */
static void split_file(const char *text)
{
    const char *pos, *next;
    int ii;

    pos = strstr(text, "END OF HEADER\n");
    if (!pos)
        BAIL_OUT("no END OF HEADER in normalized fixture");
    pos += strlen("END OF HEADER\n");
    header = strndup(text, pos - text);

    for (ii = 0; ii < N_EPOCHS; ++ii, pos = next)
    {
        if (*pos != '>')
            BAIL_OUT("epoch %d does not start with '>'", ii);
        next = strstr(pos, "\n>");
        next = next ? next + 1 : pos + strlen(pos);
        epochs[ii] = strndup(pos, next - pos);
    }
    if (*pos)
        BAIL_OUT("fixture has more than %d epochs", N_EPOCHS);
}

int main(void)
{
    char *whole, *merged, *hdr, *text;
    int res;

    plan(20);
    setenv("SOURCE_DATE_EPOCH", "1700000000", 1);

    res = run_merge("-o tm_whole.rnx " FIXTURE_V3_PATH);
    ok(res == 0, "rinex-merge normalizes a single file");
    whole = slurp("tm_whole.rnx");
    if (!whole)
        BAIL_OUT("unable to read tm_whole.rnx");
    split_file(whole);

    /* Overlapping, out-of-order pieces; one with CR LF line endings
     * and header lines that are allowed to vary. */
    hdr = replace_line(header, "PGM / RUN BY / DATE",
        "other program       someone             20991231 235959 UTC "
        "PGM / RUN BY / DATE\n");
    hdr = replace_line(hdr, "COMMENT",
        "a different comment                                         "
        "COMMENT\n");
    write_piece("tm_p1.rnx", header, 0, epochs[0], epochs[1], NULL);
    write_piece("tm_p2.rnx", hdr, 1, epochs[1], epochs[2], NULL);
    res = run_merge("-o tm_merged.rnx tm_p2.rnx tm_p1.rnx");
    ok(res == 0, "overlapping pieces merge");
    merged = slurp("tm_merged.rnx");
    ok(merged && !strcmp(merged, whole),
        "merged pieces match the normalized whole");
    free(merged);

    /* A different station. */
    hdr = replace_line(header, "MARKER NAME",
        "XXXX                                                        "
        "MARKER NAME\n");
    write_piece("tm_p3.rnx", hdr, 0, epochs[2], NULL);
    res = run_merge("tm_p1.rnx tm_p3.rnx > /dev/null");
    ok(res != 0, "differing MARKER NAME is rejected");
    res = run_merge("-f tm_p1.rnx tm_p3.rnx > /dev/null");
    ok(res == 0, "-f accepts differing MARKER NAME");

    /* Different observation types (D2L becomes D2X). */
    hdr = replace_line(header, "SYS / # / OBS TYPES",
        "G   18 C1C L1C D1C S1C C1W S1W C2W L2W D2W S2W C2L L2L D2X  "
        "SYS / # / OBS TYPES\n");
    write_piece("tm_p4.rnx", hdr, 0, epochs[2], NULL);
    res = run_merge("-f tm_p1.rnx tm_p4.rnx > /dev/null");
    ok(res != 0, "-f still rejects differing observation types");

    /* A gap: epochs at 0 and 30 s, then 90 s. */
    write_piece("tm_p5.rnx", header, 0,
        retime_epoch(2, "00 01 30.0000000"), NULL);
    res = run_merge("tm_p1.rnx tm_p5.rnx > /dev/null");
    ok(res != 0, "a gap between inputs is rejected");
    res = run_merge("-g -o tm_gap.rnx tm_p1.rnx tm_p5.rnx");
    ok(res == 0, "-g allows a gap between inputs");
    merged = slurp("tm_gap.rnx");
    ok(merged && strstr(merged, "> 2020 07 07 00 01 30.0000000"),
        "-g output includes the epoch after the gap");
    free(merged);

    /* Sample intervals of 30 s, then 60 s. */
    write_piece("tm_p6.rnx", header, 0, epochs[2],
        retime_epoch(2, "00 02  0.0000000"), NULL);
    res = run_merge("tm_p1.rnx tm_p6.rnx > /dev/null");
    ok(res != 0, "differing sample intervals are rejected");
    res = run_merge("-f tm_p1.rnx tm_p6.rnx > /dev/null");
    ok(res == 0, "-f accepts differing sample intervals");

    /* Single-epoch inputs: only INTERVAL gives the sample interval. */
    hdr = insert_line(header, "END OF HEADER",
        "    30.000                                                  "
        "INTERVAL\n");
    write_piece("tm_s1.rnx", hdr, 0, epochs[0], NULL);
    write_piece("tm_s2.rnx", hdr, 0, epochs[1], NULL);
    write_piece("tm_s3.rnx", hdr, 0,
        retime_epoch(2, "00 01 30.0000000"), NULL);
    res = run_merge("tm_s1.rnx tm_s2.rnx > /dev/null");
    ok(res == 0, "consecutive single-epoch inputs merge");
    res = run_merge("tm_s1.rnx tm_s3.rnx > /dev/null");
    ok(res != 0, "a gap between single-epoch inputs is rejected");
    res = run_merge("-g tm_s1.rnx tm_s3.rnx > /dev/null");
    ok(res == 0, "-g allows a gap between single-epoch inputs");

    /* An observation epoch, a cycle-slip record and a special event,
     * all at the same time, within one input. */
    text = strdup(epochs[0]);
    text[31] = '6';
    write_piece("tm_same.rnx", header, 0, epochs[0], text,
        "> 2020 07 07 00 00  0.0000000  4  1\n"
        "same-time event                                             "
        "COMMENT\n", epochs[1], NULL);
    res = run_merge("-o tm_same_out.rnx tm_same.rnx");
    ok(res == 0, "same-time records merge");
    merged = slurp("tm_same_out.rnx");
    ok(merged && strstr(merged, "\n> 2020 07 07 00 00  0.0000000  6"),
        "same-time flag-6 record is kept");
    ok(merged && strstr(merged, "same-time event"),
        "same-time flag-4 event is kept");
    free(merged);

    /* The output must not overwrite an input. */
    remove("tm_sym.rnx");
    remove("tm_hard.rnx");
    if (symlink("tm_p1.rnx", "tm_sym.rnx") || link("tm_p1.rnx", "tm_hard.rnx"))
        BAIL_OUT("unable to create links to tm_p1.rnx");
    text = slurp("tm_p1.rnx");
    res = run_merge("-o tm_p1.rnx tm_p1.rnx tm_p2.rnx");
    merged = slurp("tm_p1.rnx");
    ok(res != 0 && merged && !strcmp(merged, text),
        "output that is an input is refused");
    free(merged);
    res = run_merge("-o tm_sym.rnx tm_p1.rnx tm_p2.rnx");
    merged = slurp("tm_p1.rnx");
    ok(res != 0 && merged && !strcmp(merged, text),
        "output that is a symbolic link to an input is refused");
    free(merged);
    res = run_merge("-o tm_hard.rnx tm_p1.rnx tm_p2.rnx");
    merged = slurp("tm_p1.rnx");
    ok(res != 0 && merged && !strcmp(merged, text),
        "output that is a hard link to an input is refused");
    free(merged);
    free(text);

    remove("tm_sym.rnx");
    remove("tm_hard.rnx");
    remove("tm_s1.rnx");
    remove("tm_s2.rnx");
    remove("tm_s3.rnx");
    remove("tm_same.rnx");
    remove("tm_same_out.rnx");
    remove("tm_whole.rnx");
    remove("tm_merged.rnx");
    remove("tm_gap.rnx");
    remove("tm_p1.rnx");
    remove("tm_p2.rnx");
    remove("tm_p3.rnx");
    remove("tm_p4.rnx");
    remove("tm_p5.rnx");
    remove("tm_p6.rnx");
    free(whole);
    done_testing();
}
