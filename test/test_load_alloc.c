/** Force each observation-array growth to fail after earlier reallocations
 * have moved, so cleanup is checked under ASan as well as normal builds. */
#include "rinex/rnx_priv.h"
#include "rinex/rinex_load.h"
#include <stdlib.h>
#include <string.h>
#include <tap.h>

static int fail_at, calls;
static void *moving_realloc(void *old, size_t size)
{
    void *result;
    size_t old_size = calls == 0 ? 8 * sizeof(int64_t) : 8;
    if (++calls == fail_at)
        return NULL;
    result = malloc(size);
    if (!result) BAIL_OUT("test allocation failed");
    memcpy(result, old, old_size < size ? old_size : size);
    free(old);
    return result;
}

/* Compile the loader with a local allocator override. */
#define realloc moving_realloc
#include "rinex/rinex_load.c"
#undef realloc

int main(void)
{
    for (fail_at = 1; fail_at <= 3; ++fail_at)
    {
        struct rinex_data data = {0};
        calls = 0;
        data.obs = calloc(8, sizeof *data.obs);
        data.lli = malloc(8);
        data.ssi = malloc(8);
        if (!data.obs || !data.lli || !data.ssi) BAIL_OUT("test allocation failed");
        data.obs[0] = 8;
        data.obs[4] = 1234;
        memset(data.lli, '1', 8);
        memset(data.ssi, '2', 8);
        ok(rnx_load_realloc_obs(&data, -1, 0, 16) == -1,
            "allocation %d fails", fail_at);
        ok(data.obs[0] == 8 && data.obs[4] == 1234
            && data.lli[0] == '1' && data.ssi[0] == '2',
            "previous data remains owned and readable after failure %d", fail_at);
        free_rinex_data(&data);
    }
    done_testing();
    return exit_status();
}
