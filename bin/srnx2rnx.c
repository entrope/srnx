/** srnx2rnx.c - Succinct RINEX decompressor.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"
#include "rinex/srnx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Program structure:
 * - Open SRNX file using loading utilities.
 * - Emit RINEX header, grabbing the version to control v2 vs v3/v4 format.
 * - Initialize a per-satellite index into observation values.
 * - From first to last epoch:
 *   - Emit any special events for the record.
 *   - Find any satellites with observations in this epoch; format them.
 *   - Emit epoch header line.
 *   - Emit observation lines.
 */

/* ---- Observation formatting ---- */

/** Lookup table for the three fractional digits.
 *  _d3[frac * 3 .. frac * 3 + 2] yields the zero-padded "ddd" string
 *  for frac in [0, 999].  3000 data bytes + implicit NUL terminator.
 */
static const char _d3[3001] =
    "000001002003004005006007008009010011012013014015016017018019020021022023024"
    "025026027028029030031032033034035036037038039040041042043044045046047048049"
    "050051052053054055056057058059060061062063064065066067068069070071072073074"
    "075076077078079080081082083084085086087088089090091092093094095096097098099"
    "100101102103104105106107108109110111112113114115116117118119120121122123124"
    "125126127128129130131132133134135136137138139140141142143144145146147148149"
    "150151152153154155156157158159160161162163164165166167168169170171172173174"
    "175176177178179180181182183184185186187188189190191192193194195196197198199"
    "200201202203204205206207208209210211212213214215216217218219220221222223224"
    "225226227228229230231232233234235236237238239240241242243244245246247248249"
    "250251252253254255256257258259260261262263264265266267268269270271272273274"
    "275276277278279280281282283284285286287288289290291292293294295296297298299"
    "300301302303304305306307308309310311312313314315316317318319320321322323324"
    "325326327328329330331332333334335336337338339340341342343344345346347348349"
    "350351352353354355356357358359360361362363364365366367368369370371372373374"
    "375376377378379380381382383384385386387388389390391392393394395396397398399"
    "400401402403404405406407408409410411412413414415416417418419420421422423424"
    "425426427428429430431432433434435436437438439440441442443444445446447448449"
    "450451452453454455456457458459460461462463464465466467468469470471472473474"
    "475476477478479480481482483484485486487488489490491492493494495496497498499"
    "500501502503504505506507508509510511512513514515516517518519520521522523524"
    "525526527528529530531532533534535536537538539540541542543544545546547548549"
    "550551552553554555556557558559560561562563564565566567568569570571572573574"
    "575576577578579580581582583584585586587588589590591592593594595596597598599"
    "600601602603604605606607608609610611612613614615616617618619620621622623624"
    "625626627628629630631632633634635636637638639640641642643644645646647648649"
    "650651652653654655656657658659660661662663664665666667668669670671672673674"
    "675676677678679680681682683684685686687688689690691692693694695696697698699"
    "700701702703704705706707708709710711712713714715716717718719720721722723724"
    "725726727728729730731732733734735736737738739740741742743744745746747748749"
    "750751752753754755756757758759760761762763764765766767768769770771772773774"
    "775776777778779780781782783784785786787788789790791792793794795796797798799"
    "800801802803804805806807808809810811812813814815816817818819820821822823824"
    "825826827828829830831832833834835836837838839840841842843844845846847848849"
    "850851852853854855856857858859860861862863864865866867868869870871872873874"
    "875876877878879880881882883884885886887888889890891892893894895896897898899"
    "900901902903904905906907908909910911912913914915916917918919920921922923924"
    "925926927928929930931932933934935936937938939940941942943944945946947948949"
    "950951952953954955956957958959960961962963964965966967968969970971972973974"
    "975976977978979980981982983984985986987988989990991992993994995996997998999";

/** Format a single observation value into a 16-character buffer.
 *
 * The output is always exactly 16 characters: a 14-character
 * right-justified fixed-point decimal (3 decimal places) followed
 * directly by the LLI and SSI indicator characters.
 */
static void format_observation(char buf[17], int64_t obs, char lli, char ssi)
{
    int64_t abs_val;
    int frac, ii;

    if (obs < RINEX_MIN_OBS || obs > RINEX_MAX_OBS) abort();
    buf[16] = '\0';
    buf[15] = ssi;
    buf[14] = lli;

    abs_val = obs < 0 ? -obs : obs;
    frac = (int)(abs_val % 1000);
    abs_val /= 1000;
    buf[13] = _d3[frac*3+2];
    buf[12] = _d3[frac*3+1];
    buf[11] = _d3[frac*3+0];
    buf[10] = '.';

    frac = (int)(abs_val % 1000);
    abs_val /= 1000;
    buf[9] = _d3[frac*3+2];
    buf[8] = _d3[frac*3+1];
    buf[7] = _d3[frac*3+0];

    frac = (int)(abs_val % 1000);
    abs_val /= 1000;
    buf[6] = _d3[frac*3+2];
    buf[5] = _d3[frac*3+1];
    buf[4] = _d3[frac*3+0];

    frac = (int)(abs_val % 1000);
    abs_val /= 1000;
    buf[3] = _d3[frac*3+2];
    buf[2] = _d3[frac*3+1];
    buf[1] = _d3[frac*3+0];
    buf[0] = _d3[abs_val*3];

    for (ii = 0; buf[ii] == '0'; ii++) buf[ii] = ' ';
    buf[ii-1] = obs < 0 ? '-': ' ';
}

/** Fill \a buf (16 chars + NUL) with 16 spaces. */
static void fill_blank_observation(char buf[17])
{
    memset(buf, ' ', 16);
    buf[16] = '\0';
}

/** Write \a buf[0..len) to \a out, with trailing spaces stripped, then '\n'. */
static void write_rtrim(FILE *out, const char *buf, size_t len)
{
    while (len > 0 && buf[len - 1] == ' ')
        --len;
    if (len > 0)
        fwrite(buf, 1, len, out);
    fputc('\n', out);
}

/* ---- Per-satellite cursor (walks sv->when[] ranges) ---- */

/** sat_cursor tracks how far through a satellite's when[] ranges we
 * have advanced.  One cursor per satellite slot in data->sv.
 */
struct sat_cursor
{
    int range_idx; /* index into sv->when[] */
    int obs_idx;   /* cumulative observation index from completed ranges */
};

/** Advance \a c to \a epoch_idx.  Returns 1 if the satellite is
 * present at this epoch, 0 if absent.  Epochs must be visited in
 * non-decreasing order.
 */
static int cursor_at_epoch(
    struct sat_cursor *c,
    const struct rinex_satellite_data *sv,
    int epoch_idx
)
{
    if (!sv)
        return 0;

    while (c->range_idx < sv->when_used
        && sv->when[c->range_idx].end <= epoch_idx)
    {
        c->obs_idx += sv->when[c->range_idx].end
                    - sv->when[c->range_idx].start;
        ++c->range_idx;
    }

    if (c->range_idx < sv->when_used
        && sv->when[c->range_idx].start <= epoch_idx
        && epoch_idx < sv->when[c->range_idx].end)
    {
        return 1;
    }
    return 0;
}

/** Get the obs/lli/ssi index for the current epoch, given a cursor
 * for which cursor_at_epoch() just returned 1.
 */
static int cursor_obs_idx(
    const struct sat_cursor *c,
    const struct rinex_satellite_data *sv,
    int epoch_idx
)
{
    return c->obs_idx + (epoch_idx - sv->when[c->range_idx].start);
}

/* ---- Active-satellite enumeration ---- */

/** active_sat captures one satellite that is present at the current
 * epoch, with the obs index already resolved.
 */
struct active_sat
{
    const struct rinex_satellite_data *sv;
    const struct rinex_system_data *sys;
    int obs_idx;
};

/** Compute the size of data->sv (max sv.end across constellations). */
static int compute_total_sv(const struct rinex_data *data)
{
    int ii, total = 0;

    for (ii = 0; ii < 32; ++ii)
    {
        if (data->sys[ii].sv.end > total)
            total = data->sys[ii].sv.end;
    }
    return total;
}

/** Build the list of satellites present at \a epoch_idx.  Walks all
 * constellations in sys-index order, then PRN order within each, so
 * the resulting list is deterministically sorted.  Returns the number
 * of active satellites written to \a out.
 */
static int collect_active_sats(
    const struct rinex_data *data,
    int epoch_idx,
    struct sat_cursor *cursors,
    struct active_sat *out
)
{
    int sys_id, ii, n_active = 0;

    for (sys_id = 0; sys_id < 32; ++sys_id)
    {
        const struct rinex_system_data *p_sys = &data->sys[sys_id];
        int n_sv = p_sys->sv.end - p_sys->sv.start;

        for (ii = 0; ii < n_sv; ++ii)
        {
            int slot = p_sys->sv.start + ii;
            const struct rinex_satellite_data *sv = data->sv[slot];

            if (!sv)
                continue;
            if (!cursor_at_epoch(&cursors[slot], sv, epoch_idx))
                continue;

            out[n_active].sv = sv;
            out[n_active].sys = p_sys;
            out[n_active].obs_idx = cursor_obs_idx(&cursors[slot], sv, epoch_idx);
            ++n_active;
        }
    }
    return n_active;
}

/** Format the obs/lli/ssi at code index \a code into \a buf.
 * Writes 16 spaces if the satellite never observed this code or the
 * observation for this epoch is missing.
 */
static void format_one_obs(
    char buf[17],
    const struct rinex_data *data,
    const struct active_sat *as,
    int code
)
{
    int start = as->sv->start[code];
    int64_t obs;

    if (start < 0)
    {
        fill_blank_observation(buf);
        return;
    }
    obs = data->obs[start + as->obs_idx];
    if (obs == INT64_MIN)
    {
        fill_blank_observation(buf);
        return;
    }
    format_observation(buf, obs,
        data->lli[start + as->obs_idx],
        data->ssi[start + as->obs_idx]);
}

/* ---- Epoch headers ---- */

/** Emit a v2 epoch header: date/time line and 12-per-line satellite
 * IDs (continuation lines indented 32 spaces).  When the receiver
 * clock offset is non-zero, the last sat-list line is padded out to
 * 12 slots and an F12.9 field is appended.
 *
 * Format: 1X,I2.2,4(1X,I2),F11.7,2X,I1,I3,12(A1,I2),F12.9
 *
 * The clock offset is stored at scale 1e9 in v2 (matching the F12.9
 * fractional digits); see rnx_parse.c v2 path.
 */
static void emit_v2_epoch_header(
    FILE *out,
    const struct rinex_epoch *ep,
    const struct active_sat *active,
    int n_active
)
{
    int yy = (ep->yyyy_mm_dd / 10000) % 100;
    int mm = (ep->yyyy_mm_dd % 10000) / 100;
    int dd = ep->yyyy_mm_dd % 100;
    int hh = ep->hh_mm / 100;
    int mi = ep->hh_mm % 100;
    double sec_f = (double)ep->sec_e7 / 1.0e7;
    char flag = ep->flag ? ep->flag : '0';
    int ii, first_batch;

    fprintf(out, " %02d %2d %2d %2d %2d%11.7f  %c%3d",
        yy, mm, dd, hh, mi, sec_f, flag, n_active);

    /* First batch: up to 12 sats on the first line. */
    first_batch = n_active < 12 ? n_active : 12;
    for (ii = 0; ii < first_batch; ++ii)
        fwrite(active[ii].sv->id, 1, 3, out);

    /* Clock offset is at columns 69-80 of the first line (RINEX 2 spec).
     * Pad the first batch to 12 slots before the F12.9 field.
     */
    if (ep->clock_offset != 0)
    {
        fprintf(out, "%*s", 3 * (12 - first_batch), "");
        fprintf(out, "%12.9f", (double)ep->clock_offset / 1.0e12);
    }
    fputc('\n', out);

    /* Continuation lines for any satellites beyond the first 12. */
    for (ii = 12; ii < n_active; ii += 12)
    {
        int batch_end = ii + 12 < n_active ? ii + 12 : n_active;
        int jj;
        fprintf(out, "%32s", "");
        for (jj = ii; jj < batch_end; ++jj)
            fwrite(active[jj].sv->id, 1, 3, out);
        fputc('\n', out);
    }
}

/** Emit a v3/v4 epoch header line.  When the receiver clock offset is
 * non-zero, append the F15.12 field with 6 spaces of padding (6X
 * reserved field per RINEX 3.05).
 *
 * The clock offset is stored at scale 1e12 in v3/v4 (matching F15.12);
 * see rnx_parse.c v3 path.
 */
static void emit_v34_epoch_header(
    FILE *out,
    const struct rinex_epoch *ep,
    int n_active
)
{
    int yyyy = ep->yyyy_mm_dd / 10000;
    int mm = (ep->yyyy_mm_dd % 10000) / 100;
    int dd = ep->yyyy_mm_dd % 100;
    int hh = ep->hh_mm / 100;
    int mi = ep->hh_mm % 100;
    double sec_f = (double)ep->sec_e7 / 1.0e7;
    char flag = ep->flag ? ep->flag : '0';

    fprintf(out, "> %4d %02d %02d %02d %02d%11.7f  %c%3d",
        yyyy, mm, dd, hh, mi, sec_f, flag, n_active);
    if (ep->clock_offset != 0)
        fprintf(out, "      %15.12f", (double)ep->clock_offset / 1.0e12);
    fputc('\n', out);
}

/* ---- Observation lines ---- */

/** Emit a satellite's observations in v2 format: 5 obs per line,
 * 16 chars each, blank-padded for missing values, with each line
 * right-trimmed.
 */
static void emit_v2_observations(
    FILE *out,
    const struct rinex_data *data,
    const struct active_sat *as
)
{
    char line[5 * 16 + 1];
    int n_obs = as->sys->n_obs;
    int code, slot;

    for (code = 0; code < n_obs; )
    {
        for (slot = 0; slot < 5 && code < n_obs; ++slot, ++code)
        {
            format_one_obs(line + slot*16, data, as, code);
        }
        write_rtrim(out, line, slot*16);
    }
}

/** Emit a satellite's observations in v3/v4 format: 3-char sat ID
 * followed by all observations on one right-trimmed line.
 */
static void emit_v34_observations(
    FILE *out,
    const struct rinex_data *data,
    const struct active_sat *as
)
{
    /* 3 chars id + n_obs * 16 chars; n_obs is bounded by the header. */
    int n_obs = as->sys->n_obs;
    char *line = malloc(4 + (size_t)n_obs * 16);
    int code;
    size_t len;

    if (!line)
    {
        /* Fall back to direct writes; format will still be correct,
         * we just lose the rtrim.  Practically unreachable.
         */
        char field[17];
        fwrite(as->sv->id, 1, 3, out);
        for (code = 0; code < n_obs; ++code)
        {
            format_one_obs(field, data, as, code);
            fwrite(field, 1, 16, out);
        }
        fputc('\n', out);
        return;
    }

    memcpy(line, as->sv->id, 3);
    len = 3;
    for (code = 0; code < n_obs; ++code)
    {
        format_one_obs(line + len, data, as, code);
        len += 16;
    }
    write_rtrim(out, line, len);
    free(line);
}

/* ---- Header emission ---- */

/** Emit the RINEX file header verbatim.
 *
 * The SRNX RHDR chunk preserves the original header text, so we write
 * it as-is.  The header determines the output version (v2 vs v3/v4).
 */
static void emit_header(const struct rinex_data *data, FILE *out)
{
    fwrite(data->file_header, 1, (size_t)data->file_header_len, out);
}

/* ---- Special events (EVTF chunks) ---- */

/** Emit any events that fire before observation epoch \a ep_idx,
 * advancing \a *cursor past them.
 */
static void emit_events_at(
    FILE *out,
    const struct rinex_event *events,
    int n_events,
    int *cursor,
    int ep_idx
)
{
    while (*cursor < n_events && events[*cursor].epoch_index <= ep_idx)
    {
        fwrite(events[*cursor].text, 1, events[*cursor].text_len, out);
        ++*cursor;
    }
}

/* ---- Converter entry point ---- */

/** Convert a loaded SRNX file to RINEX text on the given stream. */
static void convert(const struct rinex_data *data, FILE *out)
{
    struct sat_cursor *cursors;
    struct active_sat *active;
    int evt_cursor = 0;
    int n_total_sv, ep_idx, ii;

    emit_header(data, out);

    n_total_sv = compute_total_sv(data);
    cursors = (n_total_sv > 0) ? calloc((size_t)n_total_sv, sizeof *cursors) : NULL;
    active = (n_total_sv > 0) ? calloc((size_t)n_total_sv, sizeof *active) : NULL;
    if (n_total_sv > 0 && (!cursors || !active))
    {
        fprintf(stderr, "Out of memory\n");
        free(cursors);
        free(active);
        return;
    }

    for (ep_idx = 0; ep_idx < data->epoch_used; ++ep_idx)
    {
        const struct rinex_epoch *ep = &data->epoch[ep_idx];
        int n_active;

        emit_events_at(out, data->event, data->event_used,
            &evt_cursor, ep_idx);

        n_active = (n_total_sv > 0)
            ? collect_active_sats(data, ep_idx, cursors, active) : 0;

        if (data->rinex_version == 2)
            emit_v2_epoch_header(out, ep, active, n_active);
        else
            emit_v34_epoch_header(out, ep, n_active);

        for (ii = 0; ii < n_active; ++ii)
        {
            if (data->rinex_version == 2)
                emit_v2_observations(out, data, &active[ii]);
            else
                emit_v34_observations(out, data, &active[ii]);
        }
    }

    /* Trailing events with epoch_index >= epoch_used. */
    while (evt_cursor < data->event_used)
    {
        fwrite(data->event[evt_cursor].text, 1,
            data->event[evt_cursor].text_len, out);
        ++evt_cursor;
    }

    free(cursors);
    free(active);
}

/* ---- Filename helpers ---- */

/** Check if a filename ends with the ".srnx" extension.
 * Returns the length of the extension (5) on match, 0 otherwise.
 */
static int is_srnx_file_name(const char name[], size_t len)
{
    if (len < 5)
        return 0;
    if (name[len - 5] != '.')
        return 0;
    if (!memcmp(name + len - 4, "srnx", 4))
        return 5;
    return 0;
}

/** Derive a RINEX output filename from an SRNX input filename.
 * If the input ends in ".srnx" the extension is replaced with ".rnx".
 * Otherwise ".rnx" is appended to the input name.
 * Returns a heap-allocated string, or NULL on allocation failure.
 */
static char *derive_output_name(const char input_name[])
{
    size_t name_len;
    int srnx_ext;
    char *output_name;

    name_len = strlen(input_name);
    srnx_ext = is_srnx_file_name(input_name, name_len);

    if (srnx_ext)
    {
        /* Replace ".srnx" (5 chars) with ".rnx" (4 chars): one byte shorter. */
        output_name = malloc(name_len);
        if (!output_name)
            return NULL;
        memcpy(output_name, input_name, name_len - 5);
        strcpy(output_name + name_len - 5, ".rnx");
    }
    else
    {
        /* Append ".rnx" to the full input name. */
        output_name = malloc(name_len + 5);
        if (!output_name)
            return NULL;
        memcpy(output_name, input_name, name_len);
        strcpy(output_name + name_len, ".rnx");
    }

    return output_name;
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    const char *input_name;
    const char *output_arg;
    char *output_name;
    struct rinex_data data;
    const char *err;
    FILE *out;

    /* Parse positional arguments: <input.srnx> [output.rnx] */
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "Usage: %s <input.srnx> [output.rnx]\n", argv[0]);
        return EXIT_FAILURE;
    }

    input_name = argv[1];
    output_arg = (argc == 3) ? argv[2] : NULL;

    /* Derive output filename if not explicitly given. */
    output_name = output_arg ? strdup(output_arg) : derive_output_name(input_name);
    if (!output_name)
    {
        fprintf(stderr, "Out of memory\n");
        return EXIT_FAILURE;
    }

    /* Load the SRNX file (populates data.event from EVTF chunks). */
    if (srnx_load(input_name, &data))
    {
        fprintf(stderr, "Unable to load %s: %s\n", input_name, data.error);
        free(output_name);
        return EXIT_FAILURE;
    }

    /* Open the output file ("-" means stdout). */
    if (!strcmp(output_name, "-"))
    {
        out = stdout;
    }
    else
    {
        out = fopen(output_name, "w");
        if (!out)
        {
            fprintf(stderr, "Unable to create %s\n", output_name);
            free_rinex_data(&data);
            free(output_name);
            return EXIT_FAILURE;
        }
    }

    /* Convert SRNX data to RINEX text. */
    convert(&data, out);

    /* Clean up. */
    if (out != stdout)
        fclose(out);
    free_rinex_data(&data);
    free(output_name);
    return EXIT_SUCCESS;
}
