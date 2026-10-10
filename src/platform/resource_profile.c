#include "resource_profile.h"
#include "execution_service.h"
#include "../diagnostics.h"
#include "../version.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stddef.h>
#include <string.h>

#define INTERVAL_US UINT64_C(5000000)
#define WINDOW_US UINT64_C(300000000)
#define SAMPLE_LIMIT 60u
#define INVALID_INFO UINT32_C(0xffff0001)
enum { END_WINDOW = 1, END_CLOCK = 2, END_LIMIT = 3 };
/* All storage is plain zero-initialized C data. No allocator, new thread,
 * constructor, RPC, or diagnostic-ring traffic is used by this profiler. */
static uint32_t visibility;
static bool begun, done;
static uint64_t first, last_sample, last_tick;
static unsigned sequence, start_attempts, end_attempts, end_reason;

void vco_resource_profile_visible(bool visible)
{
    /* Only the view thread writes. Publish visibility and transition count
     * together so intervals containing a close/reopen can be discarded even
     * when their endpoints both show an open page. Saturation is explicit. */
    uint32_t previous = __atomic_load_n(&visibility, __ATOMIC_RELAXED);
    if ((previous & 1u) == (unsigned)visible)
        return;
    uint32_t epoch = previous & ~UINT32_C(1);
    if (epoch != (UINT32_MAX & ~UINT32_C(1)))
        epoch += 2;
    __atomic_store_n(&visibility, epoch | (unsigned)visible, __ATOMIC_RELEASE);
}
static char *hex(char *out, uint64_t value, unsigned digits)
{
    *out++ = ' ';
    for (int shift = (int)(digits * 4) - 4; shift >= 0; shift -= 4)
        *out++ = "0123456789abcdef"[(value >> shift) & 15];
    *out = 0;
    return out;
}
static char *prefix(char *out, const char *name)
{
    while (*name)
        *out++ = *name++;
    *out = 0;
    return out;
}
static bool save(const char *tag, uint32_t value, bool reset)
{
    /* BUSY performed no I/O. Drop this sample and continue at the next normal
     * interval; never wait, replay its records or reuse the acquired counters.
     * A real storage error still ends the capture immediately. */
    int result = vco_diagnostics_write(VCO_RESOURCE_LOG, tag, value, reset);
    if (result != VCO_DIAGNOSTICS_OK) {
        if (result != VCO_DIAGNOSTICS_BUSY)
            done = true;
        return false;
    }
    return true;
}
static bool start(void)
{
    int result =
        vco_diagnostics_write(VCO_RESOURCE_LOG, "rp_start " VCO_VERSION, SAMPLE_LIMIT, true);
    ++start_attempts;
    if (result == VCO_DIAGNOSTICS_OK)
        return true;
    /* BUSY guarantees no file I/O occurred. Try again on a later control-worker
     * iteration, at most eight times; never wait or replay an acquired sample.
     * Real storage failures still stop immediately. */
    if (result != VCO_DIAGNOSTICS_BUSY || start_attempts >= VCO_RESOURCE_PROFILE_START_ATTEMPTS)
        done = true;
    return false;
}
static void end(unsigned reason)
{
    end_reason = reason;
    char tag[48] = "rp_end";
    char *p = hex(tag + strlen(tag), sequence, 4);
    (void)hex(p, last_tick, 16);
    int result = vco_diagnostics_write(VCO_RESOURCE_LOG, tag, reason, false);
    ++end_attempts;
    /* The end marker has no mutable sample data. Retry BUSY on a later worker
     * turn, at most eight times, preserving the original reason and tick. */
    if (result != VCO_DIAGNOSTICS_BUSY || end_attempts >= VCO_RESOURCE_PROFILE_END_ATTEMPTS)
        done = true;
}
static void thread_record(const char *name, int32_t id, char *tag, uint32_t *error)
{
    SceKernelThreadInfo info = {0};
    info.size = sizeof(info);
    int result = id > 0 ? sceKernelGetThreadInfo(id, &info) : -1;
    *error = (uint32_t)result;
    if (!*error && (info.size != sizeof(info) || info.stackSize < 0))
        *error = INVALID_INFO;
    char *p = hex(prefix(tag, name), sequence, 4);
    p = hex(p, (uint32_t)id, 8);
    p = hex(p, *error ? 0 : info.runClocks, 16);
    (void)hex(p, *error ? 0 : (uint32_t)info.stackSize, 8);
}
void vco_resource_profile_tick(int32_t observer, int32_t control, uint32_t state, unsigned enabled,
                               uint64_t session)
{
    if (done)
        return;
    if (end_reason) {
        end(end_reason);
        return;
    }
    /* Leave boot/menu time and any prior log untouched. The five-minute
     * window starts once a real game has a ready, bound execution snapshot.
     * Once started, keep observing transitions and never restart the window. */
    if (!begun && (!session || state != VCO_EXECUTION_READY))
        return;
    if (!begun && !start())
        return;
    int64_t begin = sceKernelGetSystemTimeWide();
    if (begin < 0 || (begun && (uint64_t)begin < last_tick)) {
        end(END_CLOCK);
        return;
    }
    last_tick = (uint64_t)begin;
    if (!begun) {
        begun = true;
        first = last_tick;
    } else {
        if (last_tick - first >= WINDOW_US) {
            end(END_WINDOW);
            return;
        }
        if (sequence >= SAMPLE_LIMIT) {
            end(END_LIMIT);
            return;
        }
        if (last_tick - last_sample < INTERVAL_US)
            return;
    }
    last_sample = last_tick;
    ++sequence;
    uint32_t phase = __atomic_load_n(&visibility, __ATOMIC_ACQUIRE);
    char observer_tag[72], control_tag[72];
    uint32_t observer_error, control_error;
    thread_record("rp_observer", observer, observer_tag, &observer_error);
    thread_record("rp_control", control, control_tag, &control_error);
    SceKernelFreeMemorySizeInfo memory = {0};
    memory.size = sizeof(memory);
    int result = sceKernelGetFreeMemorySize(&memory);
    uint32_t memory_error = (uint32_t)result;
    if (!memory_error && (memory.size != sizeof(memory) || memory.size_user < 0 ||
                          memory.size_cdram < 0 || memory.size_phycont < 0))
        memory_error = INVALID_INFO;
    int64_t finish = sceKernelGetSystemTimeWide();
    if (finish < begin) {
        end(END_CLOCK);
        return;
    }
    last_tick = (uint64_t)finish;
    char tag[112] = "rp_sample";
    char *p = hex(tag + strlen(tag), sequence, 4);
    p = hex(p, (uint64_t)begin, 16);
    p = hex(p, (uint64_t)finish, 16);
    p = hex(p, phase, 8);
    p = hex(p, enabled, 8);
    p = hex(p, state, 8);
    (void)hex(p, session, 16);
    if (!save(tag, 0, false) || !save(observer_tag, observer_error, false) ||
        !save(control_tag, control_error, false))
        return;
    p = hex(prefix(tag, "rp_memory"), sequence, 4);
    p = hex(p, memory_error ? 0 : (uint32_t)memory.size_user, 8);
    p = hex(p, memory_error ? 0 : (uint32_t)memory.size_cdram, 8);
    (void)hex(p, memory_error ? 0 : (uint32_t)memory.size_phycont, 8);
    if (!save(tag, memory_error, false))
        return;
    (void)hex(prefix(tag, "rp_commit"), sequence, 4);
    (void)save(tag, 0, false);
}
