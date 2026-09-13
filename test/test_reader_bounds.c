/** Regression tests for bounded header, stream and EPOC decoding. */
#include "rinex/rnx_priv.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tap.h>

static const char header[] =
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n"
    "G    1 C1C                                                  SYS / # / OBS TYPES\n"
    "                                                            END OF HEADER\n";

static void test_headers(void)
{
    char *input = malloc(100);
    char *text = calloc(sizeof header + RINEX_EXTRA, 1);
    struct rinex_stream *stream;
    struct rinex_parser *parser = NULL;
    const char *err;
    const char exact[] = "abc";
    memset(input, ' ', 100);
    memcpy(input + 10, "INTERVAL", 8);
    ok(rnx_find_header(input, 100, "INTERVAL", 9) < 0,
        "misplaced label does not extend search beyond input");
    memset(input, ' ', 100);
    memcpy(input + 60, "INTERVAL", 8);
    ok(rnx_find_header(input, 68, "INTERVAL", 9) < 0,
        "label without terminating newline is rejected");
    input[68] = '\n';
    ok(rnx_find_header(input, 69, "INTERVAL", 9) == 0,
        "label at the end of a bounded header is found");
    ok(rnx_memmem(exact, 3, exact, 3) == exact, "whole-buffer substring matches");
    ok(rnx_memmem(exact, 3, "bc", 2) == exact + 1, "substring at last possible position matches");
    ok(rnx_memmem(exact, 3, "cd", 2) == NULL, "partial suffix is not a match");
    ok(rnx_memmem(exact, 1, "abc", 3) == NULL, "needle longer than input is not a match");
    free(input);

    memcpy(text, header, sizeof header);
    stream = rinex_buffer_stream(text, sizeof header - 1);
    err = rinex_open(&parser, stream);
    ok(err == NULL && parser, "v3 observation header may immediately precede END OF HEADER");
    if (parser) parser->destroy(parser);
    stream->destroy(stream);
    parser = NULL;

    /* Require a continuation line, but provide END OF HEADER instead. */
    memcpy(strchr(text, '\n') + 4, " 14", 3);
    stream = rinex_buffer_stream(text, sizeof header - 1);
    err = rinex_open(&parser, stream);
    ok(err != NULL, "missing observation-code continuation is rejected safely");
    if (parser) parser->destroy(parser);
    stream->destroy(stream);
    free(text);
}

static void test_mmap(void)
{
    char path[] = "reader_bounds_XXXXXX";
    char data[1000];
    int fd = mkstemp(path);
    struct rinex_stream *stream;
    memset(data, 'x', sizeof data);
    if (fd < 0 || write(fd, data, sizeof data) != sizeof data)
        BAIL_OUT("cannot create mmap fixture");
    close(fd);
    stream = rinex_mmap_stream(path);
    if (!stream) BAIL_OUT("cannot open mmap fixture");
    ok(stream->advance(stream, 100, 0) == 0 && stream->size == 100,
        "initial partial mapping exposes requested bytes");
    ok(stream->advance(stream, 100, 100) == 0 && stream->size == 100,
        "reusing mapping exposes more real data");
    ok(stream->advance(stream, 1000, 100) == 0 && stream->size == 800,
        "refill clamps size to actual remaining file length");
    ok(stream->advance(stream, 100, 801) == EINVAL && stream->size == 800,
        "step is checked against current position, without changing state");
    ok(stream->advance(stream, 100, 800) == 0 && stream->size == 0,
        "advancing exactly to EOF exposes no padding as data");
    stream->destroy(stream);
    unlink(path);
}

static void put_u64(unsigned char **p, uint64_t value)
{
    do {
        *(*p)++ = (value & 127) | (value > 127 ? 128 : 0);
        value >>= 7;
    } while (value);
}

static void chunk(unsigned char **p, const char *name, const void *data, size_t size)
{
    memcpy(*p, name, 4); *p += 4;
    put_u64(p, size);
    memcpy(*p, data, size); *p += size;
}

static void check_epoc(const unsigned char *payload, size_t size,
    int expected, size_t expected_count, const char *description)
{
    unsigned char file[1024], *p = file;
    static const unsigned char version[] = {1, 0, 0, 0, 0};
    char path[] = "epoc_bounds_XXXXXX";
    int fd = mkstemp(path), result;
    struct srnx_reader *reader = NULL;
    struct rinex_epoch *epochs = malloc(sizeof *epochs);
    size_t count = 123;
    memset(epochs, 0x55, sizeof *epochs);
    chunk(&p, "SRNX", version, sizeof version);
    chunk(&p, "RHDR", header, sizeof header - 1);
    chunk(&p, "EPOC", payload, size);
    if (fd < 0 || write(fd, file, p - file) != p - file)
        BAIL_OUT("cannot write EPOC fixture");
    close(fd);
    result = srnx_open(&reader, path, 0);
    if (result) BAIL_OUT("cannot open EPOC fixture");
    result = srnx_get_epochs(reader, &epochs, &count);
    ok(result == expected && count == expected_count, "%s (result=%d, count=%zu)",
        description, result, count);
    srnx_close(reader);
    if (result == 0 && count)
        ok(epochs[0].clock_offset == 0 && epochs[0].n_sats == 0
            && epochs[0].flag == '0',
            "reused output array has initialized fields and default clock offset");
    free(epochs);
    unlink(path);
}

static void test_epoc(void)
{
    unsigned char payload[128], *p;
    size_t span_size;
    p = payload;
    put_u64(&p, 1); /* one epoch */
    put_u64(&p, 0); /* interval */
    put_u64(&p, 0); /* span count minus one */
    put_u64(&p, 20200101);
    put_u64(&p, 0); /* midnight */
    span_size = p - payload;
    check_epoc(payload, span_size, 0, 1, "valid epoch with omitted clock offsets");
    put_u64(&p, 0); /* clock offset */
    put_u64(&p, 1); /* invalid run of two offsets */
    check_epoc(payload, p - payload, SRNX_CORRUPT, 0, "oversized clock run rejected");
    p = payload + span_size + 1;
    put_u64(&p, UINT64_MAX);
    check_epoc(payload, p - payload, SRNX_CORRUPT, 0, "clock count cannot wrap to zero");
    check_epoc(payload, span_size + 1, SRNX_CORRUPT, 0, "missing clock count rejected");
    check_epoc(payload, span_size - 1, SRNX_CORRUPT, 0, "truncated timestamp rejected");
    payload[0] = 2;
    check_epoc(payload, span_size, SRNX_CORRUPT, 0, "missing epoch span rejected");
    payload[0] = 1;
    payload[2] = 1;
    check_epoc(payload, span_size, SRNX_CORRUPT, 0, "oversized timestamp run rejected");
    p = payload;
    put_u64(&p, UINT64_MAX);
    check_epoc(payload, p - payload, SRNX_CORRUPT, 0, "epoch allocation cannot overflow");
    memset(payload, 0x80, 11);
    check_epoc(payload, 11, SRNX_CORRUPT, 0, "overlong integer rejected without oversized shift");
    payload[0] = 0;
    check_epoc(payload, 1, 0, 0, "empty EPOC releases reused output array");
    srnx_close(NULL);
}

int main(void)
{
    test_headers();
    test_mmap();
    test_epoc();
    done_testing();
    return exit_status();
}
