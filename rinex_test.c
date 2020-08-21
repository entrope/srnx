/** rinex_test.c - RINEX unit test file.
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

#include "rinex.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct {
    const char *obs;
    int64_t value;
} obs_test[] = {
    { "  23619095.450  ", 23619095450 },
    { "          .300 8", 300 },
    { "         -.353  ", -353 },
    { "    -53875.632 8", -53875632 },
    { NULL, 0 }
};

int main(int argc, char *argv[])
{
    int ii;
    int status = EXIT_SUCCESS;

    for (ii = 0; obs_test[ii].obs; ++ii)
    {
        int64_t got = rinex_parse_obs(obs_test[ii].obs);
        if (got != obs_test[ii].value)
        {
            printf("Mismatch: %s -> %ld, expected %ld\n", obs_test[ii].obs,
                got, obs_test[ii].value);
            status = EXIT_FAILURE;
        }
    }
    printf(" ... performed %d tests\n", ii);

    (void)argc; (void)argv;
    return status;
}