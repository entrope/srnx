/** rnx2srnx.c - Convert RINEX files to Succinct RINEX format.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex.h"
#include "rinex/srnx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rnx2srnx(const char input_name[], const char output_name[])
{
    struct rinex_parser *parser;
    struct rinex_stream *stream;
    const char *err;
    rinex_error_t r_err;

    /* Open the input file. */
    stream = rinex_mmap_stream(input_name);
    if (!stream)
        return;
    parser = NULL;
    err = rinex_open(&parser, stream);
    if (err)
    {
        fprintf(stderr, "Unable to open %s: %s\n", input_name, err);
        return;
    }
    while ((r_err = parser->read(parser)) == RINEX_SUCCESS)
    {
        /* TODO: Append record from *parser to our buffer(s). */
    }
    if (r_err != RINEX_EOF)
    {
        fprintf(stderr, "Error on line %d while reading %s\n", parser->error_line, input_name);
        parser->destroy(parser);
        return;
    }
    parser->destroy(parser);

    /* TODO: write the output file */
}

static int is_rinex_file_name(const char name[], size_t name_len)
{
    const char *end_name;
    size_t name_len;

    if (name_len < 12)
        return 0;
    end_name = name + name_len;
    if (end_name[-4] != '.')
        return 0;
    if (!memcmp(end_name - 3, "rnx", 3))
        return 3;
    if (end_name[-1] == 'o' && isdigit(end_name[-2]) && isdigit(end_name[-3]))
        return 2;
    return 0;
}

int main(int argc, char *argv[])
{
    const char *input_name;
    char *output_name;
    size_t name_len;

    if (argc < 1)
    {
        fprintf(stdout, "Usage: %s <input.rnx> [output.srnx]\n", argv[0]);
        return EXIT_FAILURE;
    }

    input_name = argv[1];
    output_name = (argc > 2) ? strdup(argv[2]) : NULL;
    name_len = strlen(input_name);
    if (is_rinex_file_name(input_name, name_len))
    {
        if (!output_name)
        {
            output_name = malloc(name_len + 1);
            memcpy(output_name, input_name, name_len - 4);
            strcpy(output_name + name_len - 4, ".srnx");
        }
    }
    else
    {
        fprintf(stdout, "WARNING: Input file name '%s' does not look RINEX-like\n", input_name);
        if (!output_name)
        {
            output_name = malloc(name_len + 6);
            memcpy(output_name, input_name, name_len);
            strcpy(output_name + name_len, ".srnx");
        }
    }

    rnx2srnx(input_name, output_name);

    free(output_name);
    return EXIT_SUCCESS;
}
