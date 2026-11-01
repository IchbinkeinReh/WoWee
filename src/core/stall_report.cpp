#include "core/stall_report.hpp"
#include "core/local_time.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace wowee::core::stall_report {

namespace {

std::atomic<const char*> gStage{"(outside any stage)"};
#ifdef _WIN32
// Only the Windows stack walk marks the main thread; elsewhere nothing reads it.
std::atomic<uint32_t> gMainThreadId{0};
#endif

const char* reportPath() {
    const char* p = std::getenv("WOWEE_STALL_REPORT");
    return (p && *p) ? p : "stall_report.txt";
}

#if defined(_WIN32) && defined(_M_X64) || defined(_WIN32) && defined(__x86_64__)
constexpr int kMaxFrames = 48;

// Return addresses from a context, unwound with the image's own tables. No
// allocation: the thread may hold the heap's lock.
int walkFrom(CONTEXT ctx, DWORD64* out) {
    int n = 0;
    for (; n < kMaxFrames && ctx.Rip != 0; ++n) {
        out[n] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher,
                             nullptr);
        } else {
            // A leaf: the return address is on top of the stack.
            if (ctx.Rsp == 0) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }
    return n;
}

// A thread's return addresses, from the context it was suspended in.
int walk(HANDLE thread, DWORD64* out) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return 0;
    return walkFrom(ctx, out);
}

void writeFrames(FILE* f, const DWORD64* frames, int count) {
    const auto exeBase = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
    for (int i = 0; i < count; ++i) {
        const DWORD64 pc = frames[i];
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pc), &mod);
        const auto base = reinterpret_cast<DWORD64>(mod);
        if (base == exeBase) {
            std::fprintf(f, " exe+0x%llx", static_cast<unsigned long long>(pc - base));
        } else {
            char name[MAX_PATH] = "?";
            if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
            const char* leaf = name;
            for (const char* c = name; *c; ++c)
                if (*c == '\\' || *c == '/') leaf = c + 1;
            std::fprintf(f, " %s+0x%llx", leaf, static_cast<unsigned long long>(pc - base));
        }
    }
}

void writeStacks(FILE* f) {
    // Every thread walked first, each suspended only while it is, into
    // fixed storage; the module names are looked up afterwards, with every
    // thread running again - a lookup can take the loader's lock, which a
    // suspended thread may be holding.
    constexpr int kMaxThreads = 96;
    static DWORD ids[kMaxThreads];
    static DWORD64 frames[kMaxThreads][kMaxFrames];
    static int counts[kMaxThreads];
    int threads = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && threads < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!t) continue;
        int n = 0;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            n = walk(t, frames[threads]);
            ResumeThread(t);
        }
        CloseHandle(t);
        ids[threads] = te.th32ThreadID;
        counts[threads] = n;
        ++threads;
    }
    CloseHandle(snap);

    for (int k = 0; k < threads; ++k) {
        std::fprintf(f, "thread %lu%s:", static_cast<unsigned long>(ids[k]),
                     ids[k] == gMainThreadId.load() ? " (main)" : "");
        writeFrames(f, frames[k], counts[k]);
        std::fprintf(f, "\n");
    }
}
#endif

}  // namespace

void initMainThread() {
#ifdef _WIN32
    gMainThreadId.store(GetCurrentThreadId());
#endif
}

void setStage(const char* stage) { gStage.store(stage, std::memory_order_relaxed); }

const char* stage() { return gStage.load(std::memory_order_relaxed); }

void write(int64_t stallMs, int snapshot) {
    FILE* f = std::fopen(reportPath(), "a");
    if (!f) return;
    const std::time_t now = std::time(nullptr);
    char when[32] = "";
    const std::tm tm = localTime(now);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(f, "%s main loop stalled %lld ms (snapshot %d), stage: %s\n", when,
                 static_cast<long long>(stallMs), snapshot, gStage.load(std::memory_order_relaxed));
#if defined(_WIN32) && defined(_M_X64) || defined(_WIN32) && defined(__x86_64__)
    writeStacks(f);
#endif
    std::fprintf(f, "\n");
    std::fclose(f);
}

void writeCrash(const char* reason, const void* context) {
    // The first only: a crash raises others on its way out (a C++ throw
    // nothing catches is followed by a bad function table), and the first is
    // the one that says why.
    static std::atomic<bool> written{false};
    if (written.exchange(true)) return;
    const char* p = std::getenv("WOWEE_CRASH_REPORT");
    FILE* f = std::fopen((p && *p) ? p : "crash_report.txt", "a");
    const std::time_t now = std::time(nullptr);
    char when[32] = "";
    const std::tm tm = localTime(now);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    // To stderr as well, which is where the log of a run being watched goes.
    std::fprintf(stderr, "\n=== CRASH %s: %s, stage: %s (stacks in %s) ===\n", when, reason,
                 gStage.load(std::memory_order_relaxed), (p && *p) ? p : "crash_report.txt");
    std::fflush(stderr);
    if (!f) return;
    std::fprintf(f, "%s crash: %s, stage: %s\n", when, reason, gStage.load(std::memory_order_relaxed));
#if defined(_WIN32) && defined(_M_X64) || defined(_WIN32) && defined(__x86_64__)
    // The crashing thread first: from the faulting context where there is
    // one, else from here.
    static DWORD64 frames[kMaxFrames];
    CONTEXT ctx{};
    if (context) {
        ctx = *static_cast<const CONTEXT*>(context);
    } else {
        RtlCaptureContext(&ctx);
    }
    const int n = walkFrom(ctx, frames);
    std::fprintf(f, "thread %lu (crashing)%s:", static_cast<unsigned long>(GetCurrentThreadId()),
                 GetCurrentThreadId() == gMainThreadId.load() ? " (main)" : "");
    writeFrames(f, frames, n);
    std::fprintf(f, "\n");
    writeStacks(f);
#else
    (void)context;
#endif
    std::fprintf(f, "\n");
    std::fclose(f);
}

}  // namespace wowee::core::stall_report
