/** Materialize the tracked RINEX sample for command-line integration tests. */
#include <stdio.h>
#include "bin/abmf00glp_rnx.h"

static int write_file(const char *path, const void *data, size_t length)
{
    FILE *file = fopen(path, "wb");
    int failed;
    if (!file)
        return 1;
    failed = fwrite(data, 1, length, file) != length;
    return fclose(file) != 0 || failed;
}

int main(int argc, char **argv)
{
    static const char invalid[] = "This is not a RINEX file.\n";
    if (argc != 3)
        return 1;
    return write_file(argv[1], abmf_rnx, abmf_rnx_len)
        || write_file(argv[2], invalid, sizeof invalid - 1);
}
