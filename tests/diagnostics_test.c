#include "diagnostics.h"
#include <psp2kern/io/fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

void diagnostics_other_tu(const char *, unsigned, int);
static struct {
    char path[96];
    unsigned char bytes[VCO_LOG_FILE_LIMIT + 256];
    uint64_t length;
} files[4];
static struct {
    unsigned opens, closes, seeks, writes, truncations, maximum_write;
    unsigned fail_open, fail_seek, fail_write, zero_write, fail_close;
    int active;
    unsigned index;
    uint64_t position;
    int close_consumes;
} io;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int hold_open, entered, released;

static unsigned file_index(const char *path)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (!strcmp(files[i].path, path))
            return i;
        if (!files[i].path[0]) {
            assert(strlen(path) < sizeof(files[i].path));
            memcpy(files[i].path, path, strlen(path) + 1);
            return i;
        }
    }
    assert(0);
    return 0;
}
int sceIoOpen(const char *path, int flags, int mode)
{
    assert(path && *path && !io.active && mode == 0666);
    assert((flags & (SCE_O_WRONLY | SCE_O_CREAT)) == (SCE_O_WRONLY | SCE_O_CREAT));
    assert(!(flags & SCE_O_APPEND));
    ++io.opens;
    if (io.opens == io.fail_open)
        return -1;
    io.index = file_index(path);
    io.position = 0;
    if (flags & SCE_O_TRUNC) {
        files[io.index].length = 0;
        ++io.truncations;
    }
    io.active = 1;
    if (hold_open) {
        assert(!pthread_mutex_lock(&mutex));
        entered = 1;
        assert(!pthread_cond_broadcast(&changed));
        while (!released)
            assert(!pthread_cond_wait(&changed, &mutex));
        assert(!pthread_mutex_unlock(&mutex));
    }
    return 7;
}
int64_t sceIoLseek(int fd, int64_t offset, int whence)
{
    assert(fd == 7 && io.active && !offset && whence == SCE_SEEK_END);
    ++io.seeks;
    if (io.seeks == io.fail_seek)
        return -1;
    io.position = files[io.index].length;
    assert(io.position <= INT64_MAX);
    return (int64_t)io.position;
}
int sceIoWrite(int fd, const void *bytes, unsigned size)
{
    assert(fd == 7 && io.active && bytes && size && size <= 122);
    ++io.writes;
    if (io.writes == io.fail_write)
        return -1;
    if (io.writes == io.zero_write)
        return 0;
    if (io.maximum_write && size > io.maximum_write)
        size = io.maximum_write;
    assert(io.position + size <= VCO_LOG_FILE_LIMIT);
    memcpy(files[io.index].bytes + io.position, bytes, size);
    io.position += size;
    if (files[io.index].length < io.position)
        files[io.index].length = io.position;
    return (int)size;
}
int sceIoClose(int fd)
{
    assert(fd == 7 && io.active);
    ++io.closes;
    if (io.closes == io.fail_close) {
        if (io.close_consumes)
            io.active = 0;
        return -1;
    }
    io.active = 0;
    return 0;
}
int ksceIoOpen(const char *p, int f, int m)
{
    return sceIoOpen(p, f, m);
}
int ksceIoWrite(int fd, const void *p, unsigned n)
{
    return sceIoWrite(fd, p, n);
}
int ksceIoClose(int fd)
{
    return sceIoClose(fd);
}
int64_t ksceIoLseek(int fd, int64_t n, int w)
{
    return sceIoLseek(fd, n, w);
}
static void clean(void)
{
    assert(!io.active);
    memset(files, 0, sizeof(files));
    memset(&io, 0, sizeof(io));
}
static void expect_file(const char *path, const char *text)
{
    unsigned i = file_index(path);
    assert(files[i].length == strlen(text));
    assert(!memcmp(files[i].bytes, text, strlen(text)));
}
static void format(void)
{
    assert(!vco_diagnostics_write("one", "hello", 0x1234ABCD, 0));
    expect_file("one", "hello 0x1234abcd\n");
    assert(!vco_diagnostics_write("one", "next", 0, 0));
    expect_file("one", "hello 0x1234abcd\nnext 0x00000000\n");
    assert(!vco_diagnostics_write("two", "other", ~0u, 0));
    expect_file("two", "other 0xffffffff\n");
    assert(!vco_diagnostics_write("one", "reset", 1, 1));
    expect_file("one", "reset 0x00000001\n");
    unsigned calls = io.opens;
    assert(vco_diagnostics_write(NULL, "event", 0, 0) < 0);
    assert(vco_diagnostics_write("", "event", 0, 0) < 0);
    assert(vco_diagnostics_write("one", NULL, 0, 0) < 0);
    assert(io.opens == calls);
    char long_event[256];
    memset(long_event, 'x', sizeof(long_event));
    long_event[sizeof(long_event) - 1] = 0;
    assert(!vco_diagnostics_write("one", long_event, ~0u, 1));
    unsigned i = file_index("one");
    assert(files[i].length == 122 && !memcmp(files[i].bytes + 110, " 0xffffffff\n", 12));
    assert(!io.active && io.closes == io.opens);
}
static void bounds(void)
{
    unsigned i = file_index("one");
    const unsigned record = sizeof("x 0x00000000\n") - 1;
    files[i].length = VCO_LOG_FILE_LIMIT - record;
    assert(!vco_diagnostics_write("one", "x", 0, 0));
    assert(files[i].length == VCO_LOG_FILE_LIMIT && !io.truncations);
    assert(!vco_diagnostics_write("one", "new", 1, 0));
    expect_file("one", "new 0x00000001\n");
    assert(io.truncations == 1 && io.opens == io.closes);
    files[i].length = (uint64_t)VCO_LOG_FILE_LIMIT + 100; /* Old unbounded log. */
    assert(!vco_diagnostics_write("one", "oversized", 2, 0));
    expect_file("one", "oversized 0x00000002\n");
    files[i].length = INT64_MAX; /* Must not narrow a large file position. */
    assert(!vco_diagnostics_write("one", "large", 3, 0));
    expect_file("one", "large 0x00000003\n");
    for (unsigned n = 0; n < 10000; ++n) {
        assert(!vco_diagnostics_write("one", "continuous", n, 0));
        assert(files[i].length <= VCO_LOG_FILE_LIMIT && !io.active);
    }
    assert(io.truncations > 3 && io.opens == io.closes);
}
static void short_writes(void)
{
    io.maximum_write = 1;
    assert(!vco_diagnostics_write("one", "short", 42, 0));
    expect_file("one", "short 0x0000002a\n");
    assert(io.writes == files[file_index("one")].length && io.opens == io.closes);
    clean();
    io.maximum_write = 2;
    io.fail_write = 3;
    assert(vco_diagnostics_write("one", "partial", 0, 0) < 0);
    assert(files[file_index("one")].length == 4 && io.writes == 3 && !io.active);
    io.fail_write = 0;
    assert(!vco_diagnostics_write("one", "after", 0, 0));
    clean();
    io.maximum_write = 1;
    io.zero_write = 2;
    assert(vco_diagnostics_write("one", "zero", 0, 0) < 0);
    assert(files[file_index("one")].length == 1 && io.writes == 2 && !io.active);
}
static void failures(void)
{
    io.fail_open = 1;
    assert(vco_diagnostics_write("one", "open", 0, 0) < 0);
    assert(io.opens == 1 && !io.closes && !io.active);
    clean();
    io.fail_seek = 1;
    assert(vco_diagnostics_write("one", "seek", 0, 0) < 0);
    assert(io.opens == io.closes && !io.writes && !io.active);
    clean();
    files[file_index("one")].length = VCO_LOG_FILE_LIMIT;
    io.fail_open = 2;
    assert(vco_diagnostics_write("one", "reopen", 0, 0) < 0);
    assert(io.opens == 2 && io.closes == 1 && !io.writes && !io.active);
    assert(files[file_index("one")].length == VCO_LOG_FILE_LIMIT);
    clean();
    io.fail_write = 1;
    assert(vco_diagnostics_write("one", "write", 0, 0) < 0);
    assert(io.opens == io.closes && !io.active);
    clean();
    assert(!vco_diagnostics_write("one", "recovered", 0, 0));
}
static void close_failure(int consumed, int rotation)
{
    io.close_consumes = consumed;
    io.fail_close = 1;
    if (rotation)
        files[file_index("one")].length = VCO_LOG_FILE_LIMIT;
    assert(vco_diagnostics_write("one", "close", 0, 0) < 0);
    assert(io.closes == 1 && io.active == !consumed);
    assert(io.opens == 1 && io.writes == (rotation ? 0u : 1u));
    unsigned calls = io.opens;
    io.fail_close = 0;
    for (unsigned n = 0; n < 1000; ++n) {
        assert(vco_diagnostics_write("other", "disabled", n, 1) < 0);
        diagnostics_other_tu("disabled too", n, 0);
    }
    assert(io.opens == calls && io.closes == 1 && io.active == !consumed);
    /* Ambiguous close is not retried, regardless of whether the fake consumed
     * it. At most this one retained ownership record survives until reboot. */
}
static void *held_writer(void *unused)
{
    (void)unused;
    assert(!vco_diagnostics_write(VCO_LOG_PATH, "held", 0, 0));
    return NULL;
}
static void *many_writers(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 2000; ++i) {
        if (i & 1)
            diagnostics_other_tu("other TU", i, 0);
        else
            vco_log("main TU", i, 0);
    }
    return NULL;
}
static void shared(void)
{
    vco_log("main", 1, 1);
    diagnostics_other_tu("second", 2, 0);
    expect_file(VCO_LOG_PATH, "main 0x00000001\nsecond 0x00000002\n");
    hold_open = 1;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, held_writer, NULL));
    assert(!pthread_mutex_lock(&mutex));
    while (!entered)
        assert(!pthread_cond_wait(&changed, &mutex));
    unsigned calls = io.opens;
    diagnostics_other_tu("busy dropped", 3, 0);
    assert(vco_diagnostics_write("other", "busy", 0, 0) == VCO_DIAGNOSTICS_BUSY &&
           io.opens == calls);
    /* Argument errors remain distinguishable even while another writer owns
     * the gate. Neither result may open a second descriptor. */
    assert(vco_diagnostics_write(NULL, "busy", 0, 0) == VCO_DIAGNOSTICS_ERROR && io.opens == calls);
    released = 1;
    assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&mutex));
    assert(!pthread_join(thread, NULL));
    hold_open = 0;
    assert(vco_diagnostics_write("other", "retry", 4, 0) == VCO_DIAGNOSTICS_OK);
    expect_file("other", "retry 0x00000004\n");
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], NULL, many_writers, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    assert(!io.active && io.opens == io.closes);
    assert(files[file_index(VCO_LOG_PATH)].length <= VCO_LOG_FILE_LIMIT);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "format"))
        format();
    else if (!strcmp(argv[1], "bounds"))
        bounds();
    else if (!strcmp(argv[1], "short_writes"))
        short_writes();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "shared"))
        shared();
    else if (!strcmp(argv[1], "close_consumed"))
        close_failure(1, 0);
    else if (!strcmp(argv[1], "close_retained"))
        close_failure(0, 0);
    else if (!strcmp(argv[1], "close_rotation"))
        close_failure(0, 1);
    else
        assert(0);
    printf("PASS diagnostics %s\n", argv[1]);
    return 0;
}
