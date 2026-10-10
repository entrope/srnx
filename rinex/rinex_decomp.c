/** rinex_decomp.c - RINEX stream that decompresses through a child process.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rnx_priv.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Observation files are often distributed gzipped, bzip2ed, etc.
 * rinex_open() recognizes those formats by their signatures and wraps
 * the stream in this filter, which runs the usual decompression program
 * (zcat, bzcat, ...) with a socket as its standard input and a pipe as
 * its standard output.  It copies data from the underlying stream into
 * the socket and from the pipe into its own buffer, using poll() so that
 * neither direction can block the other.  The child's input is a socket
 * rather than a pipe so that writing to it after the child exits gives
 * EPIPE rather than SIGPIPE.
 */

struct rinex_stream_decomp
{
    struct rinex_stream base;

    /** inner is the underlying stream.  It is owned by the caller of
     * rinex_open(), so destroying this stream does not touch it.
     */
    struct rinex_stream *inner;

    /** in_ofs is how many bytes of \a inner->buffer have been sent to
     * the child.
     */
    unsigned int in_ofs;

    /** alloc is the allocated length of #base.buffer. */
    unsigned int alloc;

    /** to_child is our end of the child's standard input, or -1 once
     * it has been closed.
     */
    int to_child;

    /** from_child is our end of the child's standard output, or -1 once
     * it has been closed.
     */
    int from_child;

    /** pid is the child's process ID, or 0 once it has been reaped. */
    pid_t pid;

    /** err is a C errno value to report from every call to advance once
     * the child has exited, nonzero if it failed.
     */
    int err;
};

/** rnx_decompressors maps compressed-file signatures to the programs
 * that decompress them to standard output.
 */
static const struct
{
    const char *magic;
    unsigned int magic_len;
    const char *program;
} rnx_decompressors[] = {
    { "\x1f\x8b", 2, "zcat" },          /* gzip */
    { "\x1f\x9d", 2, "zcat" },          /* compress (.Z) */
    { "BZh", 3, "bzcat" },              /* bzip2 */
    { "BZ3v1", 5, "bz3cat" },           /* bzip3 */
    { "\x28\xb5\x2f\xfd", 4, "zstdcat" }, /* zstd */
    { "\xfd" "7zXZ", 6, "xzcat" },      /* xz (the magic includes a NUL) */
};

/** Reap the child, recording in \a stream->err whether it failed. */
static void rnx_decomp_reap(struct rinex_stream_decomp *stream)
{
    int status;

    while (waitpid(stream->pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            stream->err = errno;
            stream->pid = 0;
            return;
        }
    }
    stream->pid = 0;

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        stream->err = EIO;
    }
}

/** Close the child's standard input. */
static void rnx_decomp_close_input(struct rinex_stream_decomp *stream)
{
    close(stream->to_child);
    stream->to_child = -1;
}

/** Send whatever the child will accept from the inner stream's buffer,
 * first refilling that buffer if we have sent all of it.
 * \returns Zero on success, a C errno value on failure.
 */
static int rnx_decomp_send(struct rinex_stream_decomp *stream)
{
    struct rinex_stream *inner = stream->inner;
    ssize_t nbw;
    int res, flags;

    if (stream->in_ofs == inner->size)
    {
        res = inner->advance(inner, BLOCK_SIZE, stream->in_ofs);
        if (res)
        {
            return res;
        }
        stream->in_ofs = 0;
        if (inner->size == 0)
        {
            rnx_decomp_close_input(stream);
            return 0;
        }
    }

    flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    nbw = send(stream->to_child, inner->buffer + stream->in_ofs,
        inner->size - stream->in_ofs, flags);
    if (nbw < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return 0;
        }
        if (errno == EPIPE)
        {
            /* The child stopped reading; its exit status says why. */
            rnx_decomp_close_input(stream);
            return 0;
        }
        return errno;
    }
    stream->in_ofs += nbw;

    return 0;
}

/** Read what the child has written, up to \a req_size bytes in total.
 * \returns Zero on success, a C errno value on failure.
 */
static int rnx_decomp_recv(struct rinex_stream_decomp *stream, unsigned int req_size)
{
    ssize_t nbr;

    nbr = read(stream->from_child, stream->base.buffer + stream->base.size,
        req_size - stream->base.size);
    if (nbr < 0)
    {
        return (errno == EINTR) ? 0 : errno;
    }
    if (nbr == 0)
    {
        close(stream->from_child);
        stream->from_child = -1;
        if (stream->to_child >= 0)
        {
            rnx_decomp_close_input(stream);
        }
        rnx_decomp_reap(stream);
        return 0;
    }
    stream->base.size += nbr;

    return 0;
}

static int rnx_decomp_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    struct rinex_stream_decomp *stream = (struct rinex_stream_decomp *)stream_base;
    struct pollfd pfd[2];
    int res, n_fds;

    if (req_size > INT_MAX || step > stream->base.size)
    {
        return EINVAL;
    }

    if (stream->alloc < req_size + RINEX_EXTRA)
    {
        char *new_buf;

        new_buf = realloc(stream->base.buffer, req_size + RINEX_EXTRA);
        if (!new_buf)
        {
            return ENOMEM;
        }
        stream->base.buffer = new_buf;
        stream->alloc = req_size + RINEX_EXTRA;
    }

    stream->base.size -= step;
    memmove(stream->base.buffer, stream->base.buffer + step, stream->base.size);

    while (stream->base.size < req_size && stream->from_child >= 0)
    {
        pfd[0].fd = stream->from_child;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        n_fds = 1;
        if (stream->to_child >= 0)
        {
            pfd[1].fd = stream->to_child;
            pfd[1].events = POLLOUT;
            pfd[1].revents = 0;
            n_fds = 2;
        }

        if (poll(pfd, n_fds, -1) < 0)
        {
            if (errno == EINTR)
                continue;
            return errno;
        }

        if (n_fds > 1 && pfd[1].revents)
        {
            res = rnx_decomp_send(stream);
            if (res)
            {
                return res;
            }
        }

        if (pfd[0].revents)
        {
            res = rnx_decomp_recv(stream, req_size);
            if (res)
            {
                return res;
            }
        }
    }

    memset(stream->base.buffer + stream->base.size, 0, RINEX_EXTRA);
    return stream->err;
}

static void rnx_decomp_destroy(struct rinex_stream *stream_base)
{
    struct rinex_stream_decomp *stream = (struct rinex_stream_decomp *)stream_base;

    /* We may be stopping early, so do not wait for the child to finish.
     * Signal it before closing its pipes so it does not first complain
     * about them.
     */
    if (stream->pid)
    {
        kill(stream->pid, SIGTERM);
    }
    if (stream->to_child >= 0)
    {
        close(stream->to_child);
    }
    if (stream->from_child >= 0)
    {
        close(stream->from_child);
    }
    if (stream->pid)
    {
        rnx_decomp_reap(stream);
    }

    free(stream->base.buffer);
    free(stream);
}

/** Set \a flag in \a fd's flags, which are read by fcntl() command
 * \a get (F_GETFD or F_GETFL) and written by \a set (F_SETFD or F_SETFL).
 */
static int rnx_set_fd_flag(int fd, int get, int set, int flag)
{
    int flags;

    flags = fcntl(fd, get);
    if (flags < 0)
    {
        return -1;
    }
    return fcntl(fd, set, flags | flag);
}

/* Doc comment is in rnx_priv.h. */
const char *rnx_decompressor(const struct rinex_stream *stream)
{
    unsigned int ii;

    for (ii = 0; ii < sizeof rnx_decompressors / sizeof rnx_decompressors[0]; ++ii)
    {
        if (stream->size >= rnx_decompressors[ii].magic_len
            && !memcmp(stream->buffer, rnx_decompressors[ii].magic,
                rnx_decompressors[ii].magic_len))
        {
            return rnx_decompressors[ii].program;
        }
    }

    return NULL;
}

/* Doc comment is in rnx_priv.h. */
const char *rnx_decomp_stream(
    struct rinex_stream **p_stream,
    struct rinex_stream *inner,
    const char *program
)
{
    static char errmsg[80];
    struct rinex_stream_decomp *stream;
    posix_spawn_file_actions_t actions;
    char *argv[2];
    int in_fds[2], out_fds[2], res;
    pid_t pid;

    *p_stream = NULL;
    stream = calloc(1, sizeof *stream);
    if (!stream)
    {
        return "Memory allocation failed";
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, in_fds) < 0)
    {
        res = errno;
        goto fail_stream;
    }
    if (pipe(out_fds) < 0)
    {
        res = errno;
        goto fail_in;
    }
#if defined(SO_NOSIGPIPE)
    res = 1;
    setsockopt(in_fds[0], SOL_SOCKET, SO_NOSIGPIPE, &res, sizeof res);
#endif
    /* Our ends must not leak into the child (or other children). */
    if (rnx_set_fd_flag(in_fds[0], F_GETFD, F_SETFD, FD_CLOEXEC)
        || rnx_set_fd_flag(in_fds[1], F_GETFD, F_SETFD, FD_CLOEXEC)
        || rnx_set_fd_flag(out_fds[0], F_GETFD, F_SETFD, FD_CLOEXEC)
        || rnx_set_fd_flag(out_fds[1], F_GETFD, F_SETFD, FD_CLOEXEC)
        || rnx_set_fd_flag(in_fds[0], F_GETFL, F_SETFL, O_NONBLOCK))
    {
        res = errno;
        goto fail_out;
    }

    res = posix_spawn_file_actions_init(&actions);
    if (res)
    {
        goto fail_out;
    }
    res = posix_spawn_file_actions_adddup2(&actions, in_fds[1], STDIN_FILENO);
    if (!res)
    {
        res = posix_spawn_file_actions_adddup2(&actions, out_fds[1], STDOUT_FILENO);
    }
    if (!res)
    {
        argv[0] = (char *)program;
        argv[1] = NULL;
        res = posix_spawnp(&pid, program, &actions, NULL, argv, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (res)
    {
        goto fail_out;
    }

    close(in_fds[1]);
    close(out_fds[1]);
    stream->base.advance = rnx_decomp_advance;
    stream->base.destroy = rnx_decomp_destroy;
    stream->inner = inner;
    stream->to_child = in_fds[0];
    stream->from_child = out_fds[0];
    stream->pid = pid;
    *p_stream = &stream->base;
    return NULL;

fail_out:
    close(out_fds[0]);
    close(out_fds[1]);
fail_in:
    close(in_fds[0]);
    close(in_fds[1]);
fail_stream:
    free(stream);
    snprintf(errmsg, sizeof errmsg, "Unable to run %s: %s",
        program, strerror(res));
    return errmsg;
}
