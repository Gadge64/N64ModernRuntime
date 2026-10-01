#include <cstdio>
#include <thread>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <cassert>
#include <string>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(t->id);
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_main_thread = false;
// Whether this thread is part of the game (i.e. the start thread or one spawned by osCreateThread)
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;

void ultramodern::set_main_thread() {
    ::is_game_thread = true;
    is_main_thread = true;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_main_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
void ultramodern::set_native_thread_name(const std::string& name) {
    std::wstring wname{name.begin(), name.end()};

    HRESULT r;
    r = SetThreadDescription(
        GetCurrentThread(),
        wname.c_str()
    );
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    int nPriority = THREAD_PRIORITY_NORMAL;

    // Convert ThreadPriority to Win32 priority
    switch (pri) {
        case ThreadPriority::Low:
            nPriority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            nPriority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::VeryHigh:
            nPriority = THREAD_PRIORITY_HIGHEST;
            break;
        case ThreadPriority::Critical:
            nPriority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
            break;
    }
    // [wcw fix] Apply the priority (this call was commented out, so it was computed but never set).
    SetThreadPriority(GetCurrentThread(), nPriority);
}
#elif defined(__linux__)
#include <sys/prctl.h>

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // TODO linux thread priority
    // printf("set_native_thread_priority unimplemented\n");
    // int nPriority = THREAD_PRIORITY_NORMAL;

    // // Convert ThreadPriority to Win32 priority
    // switch (pri) {
    //     case ThreadPriority::Low:
    //         nPriority = THREAD_PRIORITY_BELOW_NORMAL;
    //         break;
    //     case ThreadPriority::Normal:
    //         nPriority = THREAD_PRIORITY_NORMAL;
    //         break;
    //     case ThreadPriority::High:
    //         nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
    //         break;
    //     case ThreadPriority::VeryHigh:
    //         nPriority = THREAD_PRIORITY_HIGHEST;
    //         break;
    //     case ThreadPriority::Critical:
    //         nPriority = THREAD_PRIORITY_TIME_CRITICAL;
    //         break;
    //     default:
    //         throw std::runtime_error("Invalid thread priority!");
    //         break;
    // }
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#endif

// [wcw2k] forward declarations (defined below with the context map).
static UltraThreadContext* wcw2k_get_context(RDRAM_ARG PTR(OSThread) t_);
static UltraThreadContext* wcw2k_get_context_host(RDRAM_ARG OSThread* t);

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    thread_context->running.wait();
    // If this thread's context was replaced by another thread or deleted, destroy it again from its own context.
    // This will trigger thread cleanup instead.
    // [wcw2k] compare through the repairing accessor so a game-wiped (but live)
    // context doesn't read as "replaced".
    if (wcw2k_get_context(PASS_RDRAM ultramodern::this_thread()) != thread_context) {
        osDestroyThread(PASS_RDRAM NULLPTR);
    }
}

void resume_thread(RDRAM_ARG OSThread* t) {
    debug_printf("[Thread] Resuming execution of thread %d\n", t->id);
    // [wcw2k] repair a game-wiped context; a thread absent from the host map was
    // genuinely destroyed — nothing to resume.
    UltraThreadContext* ctx = wcw2k_get_context_host(PASS_RDRAM t);
    if (ctx == nullptr) {
        fprintf(stderr, "[wcw2k][thread] resume of destroyed thread id=%d — skipped\n", t->id);
        return;
    }
    ctx->running.signal();
}

// [wcw2k] WM2000's main-loop thread wipes its OWN OSThread struct while running (the
// mode-transition code clears the arena its boot TCB was allocated from, then keeps
// executing). On hardware that's survivable: the TCB is just memory the OS writes
// register state into at the next switch. In ultramodern it nulls the host-written
// `context` pointer, which the runtime needs at every yield. The context pointer is
// RUNTIME property that merely lives in game memory — so keep an authoritative
// host-side copy keyed by struct address and transparently restore it when the
// in-rdram copy reads null. Entries are erased on genuine destruction (osDestroyThread
// / thread replacement), so a restore can never resurrect a dead thread.
// The map also keeps priority and id: a wiped TCB leaves garbage priority
// (observed 0xC4430000 = huge negative), which starves the thread in every
// priority comparison — it would never finish the mode transition. Restoring the
// runtime's last-known values mirrors what the scheduler itself knew about the
// thread; osSetThreadPri keeps the copy in sync.
struct Wcw2kThreadRecord {
    UltraThreadContext* context;
    OSPri priority;
    OSId id;
};
static std::mutex wcw2k_context_map_mutex;
static std::unordered_map<int32_t, Wcw2kThreadRecord> wcw2k_context_map;

static void wcw2k_register_context(PTR(OSThread) t_, UltraThreadContext* context, OSPri pri, OSId id) {
    std::lock_guard lock{wcw2k_context_map_mutex};
    wcw2k_context_map[(int32_t)t_] = Wcw2kThreadRecord{context, pri, id};
}

static void wcw2k_update_priority(PTR(OSThread) t_, OSPri pri) {
    std::lock_guard lock{wcw2k_context_map_mutex};
    auto it = wcw2k_context_map.find((int32_t)t_);
    if (it != wcw2k_context_map.end()) {
        it->second.priority = pri;
    }
}

static void wcw2k_unregister_context(PTR(OSThread) t_, UltraThreadContext* context) {
    std::lock_guard lock{wcw2k_context_map_mutex};
    auto it = wcw2k_context_map.find((int32_t)t_);
    if (it != wcw2k_context_map.end() && it->second.context == context) {
        wcw2k_context_map.erase(it);
    }
}

// Returns the thread's context, repairing a game-wiped in-rdram pointer from the
// host-side map. Null only if the thread was genuinely destroyed.
static UltraThreadContext* wcw2k_get_context_host(RDRAM_ARG OSThread* t) {
    // reverse of TO_PTR: recover the rdram-relative pointer for the map key.
    PTR(OSThread) t_ = (PTR(OSThread))(int32_t)((uint32_t)((uint8_t*)t - rdram) + 0x80000000u);
    return wcw2k_get_context(PASS_RDRAM t_);
}

static UltraThreadContext* wcw2k_get_context(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    std::lock_guard lock{wcw2k_context_map_mutex};
    auto it = wcw2k_context_map.find((int32_t)t_);
    if (it == wcw2k_context_map.end()) {
        // No host record (thread not made by osCreateThread, or genuinely destroyed):
        // the rdram copy is all there is.
        return t->context;
    }
    // The host record is AUTHORITATIVE: the in-rdram copy is just a cache the game
    // may overwrite with anything (WM2000's transitions wrote zeros one time and
    // 0x200000012 another). Repair on any mismatch — on hardware libultra likewise
    // rewrites TCB fields on every context switch, so games cannot keep load-bearing
    // data in them.
    // Compare ALL mirrored fields, not context alone: boot47's wedge was a wipe flavor
    // that trashed only the first 0x10 TCB bytes (priority=-1, queue=-1, next=0) while
    // leaving the +0x20 context intact — a context-only trigger no-ops, and the pri -1
    // thread then loses every check_running_queue compare from the HEAD of the running
    // queue (which the starvation valve exempted) and parks forever. Priority is safe
    // to compare: osSetThreadPri keeps the record in sync, so any divergence is a
    // clobber.
    if (t->context != it->second.context || t->priority != it->second.priority || t->id != it->second.id) {
        fprintf(stderr, "[wcw2k][thread] repaired clobbered TCB for struct 0x%08X (read id=%d pri=%d ctx=%p; restoring id=%d pri=%d)\n",
            (uint32_t)t_, t->id, t->priority, (void*)t->context, it->second.id, it->second.priority);
        t->context = it->second.context;
        t->priority = it->second.priority;
        t->id = it->second.id;
    }
    return it->second.context;
}

// [wcw2k] public wrapper so the scheduler (scheduling.cpp) can repair a wiped TCB
// before its raw priority reads: check_running_queue's compare and
// thread_queue_insert's ordering both read t->priority straight from rdram, and a
// game-wiped garbage priority there mis-sorts the thread PERMANENTLY (a hugely
// negative value inserts it behind the pri-8 idle thread, whose pause_self loop only
// ever swaps to strictly-higher-priority queued threads — the thread is then never
// popped, so the pop-time repair never fires; observed as the post-transition wedge:
// thread 6 starved forever, gfx frozen, audio alive).
static void wcw2k_spraywatch(RDRAM_ARG const char* where);

UltraThreadContext* ultramodern::wcw2k_repair_thread(RDRAM_ARG PTR(OSThread) t_) {
    wcw2k_spraywatch(PASS_RDRAM "sched");
    return wcw2k_get_context(PASS_RDRAM t_);
}

// [wcw2k] Anti-starvation valve. WM2000's audio pipeline is a closed message loop
// among four high-priority threads (pri 80/100/110/120): each one's send wakes the
// next before it blocks, and because ultramodern completes RSP audio tasks and DMAs
// instantly (no hardware latency), the loop never has an "everything blocked" gap.
// Result: at every run_next_thread pop instant some >=pri-80 thread is runnable, and
// the pri-8 game-loop thread sits in the running queue forever (observed live in
// boot36/37: thread 6 runnable-at-correct-priority for minutes while gfx froze at
// task 1 — NOT a lost wakeup; mq 0x800559A0 state was consistent). On hardware the
// RSP/AI latencies create gaps where the game thread runs. The valve restores that:
// any queued thread (above idle priority 0) that has waited longer than
// WCW2K_STARVATION_MS is popped ahead of the priority order, with a loud log line —
// healthy scheduling never trips it (normal waits are <1 frame).
static constexpr int64_t WCW2K_STARVATION_MS = 50;
static std::unordered_map<int32_t, int64_t> wcw2k_queued_since;
static std::mutex wcw2k_queued_since_mutex;

static int64_t wcw2k_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// [wcw2k] Post-valve boost window: a starvation-popped thread runs un-preempted for a
// short window (check_running_queue skips the swap-away), because otherwise its very
// first osSendMesg/osRecvMesg swaps it right back behind the saturated high-priority
// loop and it advances one OS call per valve fire (observed: gfx +1 per ~10 fires).
// 20ms is ~1 frame of work and far below the game's ~100ms audio buffering.
static constexpr int64_t WCW2K_BOOST_MS = 20;
static std::atomic<int32_t> wcw2k_boosted_thread{0};
static std::atomic<int64_t> wcw2k_boost_until{0};

bool ultramodern::wcw2k_thread_boosted(PTR(OSThread) t_) {
    return (int32_t)t_ == wcw2k_boosted_thread.load(std::memory_order_relaxed) &&
           wcw2k_now_ms() < wcw2k_boost_until.load(std::memory_order_relaxed);
}

void ultramodern::wcw2k_note_thread_queued(PTR(OSThread) t_) {
    std::lock_guard lock{wcw2k_queued_since_mutex};
    wcw2k_queued_since[(int32_t)t_] = wcw2k_now_ms();
}

// Walk the running queue; unlink and return the first above-idle-priority entry that
// has been queued longer than the starvation threshold (NULLPTR if none).
static PTR(OSThread) wcw2k_pop_starved(RDRAM_ARG1) {
    int64_t now = wcw2k_now_ms();
    PTR(OSThread) head = ultramodern::thread_queue_peek(PASS_RDRAM ultramodern::running_queue);
    if (head == NULLPTR) {
        return NULLPTR;
    }
    std::lock_guard lock{wcw2k_queued_since_mutex};
    // Leave priority-0 idle threads alone (starving idle is by design). The head used
    // to be exempt ("the normal pop handles it") — but boot47 proved a clobbered head
    // can sit unpoppable forever: TCB wiped to priority=-1 with context intact, so the
    // context-only repair no-op'd and check_running_queue never saw it outrank idle.
    // The field-mismatch repair now fixes that flavor at peek time; popping an overdue
    // head here as well covers any future flavor the repair can't recognize (e.g. no
    // host record). Healthy scheduling never leaves the head queued 50ms, so this
    // changes nothing on the happy path.
    {
        OSThread* h = TO_PTR(OSThread, head);
        auto hit = wcw2k_queued_since.find((int32_t)head);
        if (h->priority != 0 && hit != wcw2k_queued_since.end() && now - hit->second > WCW2K_STARVATION_MS) {
            ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
            wcw2k_boosted_thread.store((int32_t)head, std::memory_order_relaxed);
            wcw2k_boost_until.store(now + WCW2K_BOOST_MS, std::memory_order_relaxed);
            fprintf(stderr, "[wcw2k][sched] STARVATION valve (HEAD): thread id=%d pri=%d @0x%08X queued %lld ms unpopped; boosting %lld ms\n",
                h->id, h->priority, (uint32_t)head, (long long)(now - hit->second), (long long)WCW2K_BOOST_MS);
            return head;
        }
    }
    PTR(OSThread) prev = head;
    PTR(OSThread) cur = TO_PTR(OSThread, head)->next;
    while (cur != NULLPTR) {
        OSThread* t = TO_PTR(OSThread, cur);
        auto it = wcw2k_queued_since.find((int32_t)cur);
        if (t->priority > 0 && it != wcw2k_queued_since.end() && now - it->second > WCW2K_STARVATION_MS) {
            TO_PTR(OSThread, prev)->next = t->next;   // unlink
            t->queue = NULLPTR;
            wcw2k_boosted_thread.store((int32_t)cur, std::memory_order_relaxed);
            wcw2k_boost_until.store(now + WCW2K_BOOST_MS, std::memory_order_relaxed);
            static uint32_t fire_count = 0;
            if ((fire_count++ % 32) == 0) {
                fprintf(stderr, "[wcw2k][sched] STARVATION valve (fire %u): running thread id=%d pri=%d @0x%08X (queued %lld ms behind higher-priority loop; boosting %lld ms)\n",
                    fire_count, t->id, t->priority, (uint32_t)cur, (long long)(now - it->second), (long long)WCW2K_BOOST_MS);
            }
            return cur;
        }
        prev = cur;
        cur = t->next;
    }
    return NULLPTR;
}

// [wcw2k] DIAGNOSTIC: dump an OSThread struct's raw words (game-byte-order view) —
// distinguishes "game clobbered it with unrelated data" from "game manually
// re-initialized a TCB" (menu-transition crash investigation).
static void wcw2k_dump_thread_struct(RDRAM_ARG PTR(OSThread) t_) {
    // rdram stores 32-bit words host-endian at aligned offsets, so a direct u32 view
    // yields the game's word values.
    const uint32_t* words = reinterpret_cast<const uint32_t*>(TO_PTR(void, t_));
    fprintf(stderr, "[wcw2k][thread] struct @0x%08X:", (uint32_t)t_);
    for (uint32_t i = 0; i < 0x10; i++) {
        fprintf(stderr, " %08X", words[i]);
    }
    fprintf(stderr, "\n");
}

// [wcw2k] WCW_SCHED_LOG=1: periodic one-line dump of the running queue (id/pri/struct
// chain) so a starved thread is visible — a wedge shows up as a thread sitting in the
// queue with wrong priority (or missing entirely) while others cycle.
static void wcw2k_sched_log(RDRAM_ARG1) {
    static const bool enabled = getenv("WCW_SCHED_LOG") != nullptr;
    if (!enabled) {
        return;
    }
    static uint32_t pop_count = 0;
    if ((pop_count++ % 512) != 0) {
        return;
    }
    fprintf(stderr, "[wcw2k][sched] self=0x%08X queue:", (uint32_t)ultramodern::this_thread());
    PTR(OSThread) cur = ultramodern::thread_queue_peek(PASS_RDRAM ultramodern::running_queue);
    for (int i = 0; cur != NULLPTR && i < 16; i++) {
        OSThread* t = TO_PTR(OSThread, cur);
        fprintf(stderr, " id=%d pri=%d @0x%08X", t->id, t->priority, (uint32_t)cur);
        cur = t->next;
    }
    fprintf(stderr, "\n");
}

// [wcw2k] DIAGNOSTIC (WCW2K_SPRAYWATCH=1): watch the region holding WM2000's thread-6
// TCB + stack (0x80080000..0x80084000) for writes, attributed to the game thread that
// just ran (checked at every scheduler touch). WM2000's transitions spray this region
// (evolving garbage across 7 TCB repairs in boot48) and thread 6 resumes with a saved
// register restored from a clobbered stack slot (s0=1 -> crash in func_800222D8).
// Changes while thread 6 itself runs are its own stack traffic and are not logged.
static void wcw2k_spraywatch(RDRAM_ARG const char* where) {
    static const bool enabled = getenv("WCW2K_SPRAYWATCH") != nullptr;
    if (!enabled) return;
    // Watch thread 6's TCB (0x800807F0, 0x30 host bytes + margin) word-by-word: boot50
    // showed the render thread making STRUCTURED writes into it (state/flags
    // transitions, a pointer at +0x18, evolving values), not a bulk fill — the game
    // appears to run its own thread bookkeeping against stock-libultra TCB offsets.
    constexpr uint32_t base = 0x800807C0, size = 0xA0;
    static uint32_t prev[size / 4];
    static bool primed = false;
    const uint32_t* cur = (const uint32_t*)TO_PTR(void, (int32_t)base);
    if (!primed) { memcpy(prev, cur, size); primed = true; return; }
    PTR(OSThread) self_ = ultramodern::this_thread();
    // Thread 6 itself and the scheduler legitimately write its TCB.
    if ((uint32_t)self_ == 0x800807F0u) { memcpy(prev, cur, size); return; }
    int shown = 0;
    for (uint32_t i = 0; i < size / 4; i++) {
        if (cur[i] != prev[i] && shown < 8) {
            fprintf(stderr, "[wcw2k][spray] 0x%08X: %08X -> %08X (by struct=0x%08X id=%d at %s)\n",
                base + i * 4, prev[i], cur[i], (uint32_t)self_,
                self_ != NULLPTR ? TO_PTR(OSThread, self_)->id : -1, where);
            shown++;
        }
    }
    if (shown) { memcpy(prev, cur, size); }

    // Second window: thread 6's shallow stack frames (func_800222D8's 0x40-byte frame
    // below entry sp 0x80083990). The deterministic crash restores saved-s0 == 1 from
    // here; trap any word BECOMING 1 to identify the slot and the writer.
    constexpr uint32_t fbase = 0x80083900, fsize = 0xA0;
    static uint32_t fprev[fsize / 4];
    static bool fprimed = false;
    const uint32_t* fcur = (const uint32_t*)TO_PTR(void, (int32_t)fbase);
    if (!fprimed) { memcpy(fprev, fcur, fsize); fprimed = true; return; }
    for (uint32_t i = 0; i < fsize / 4; i++) {
        if (fcur[i] != fprev[i] && (fcur[i] == 1 || fprev[i] == 1)) {
            fprintf(stderr, "[wcw2k][frame] 0x%08X: %08X -> %08X (by struct=0x%08X id=%d at %s)\n",
                fbase + i * 4, fprev[i], fcur[i], (uint32_t)self_,
                self_ != NULLPTR ? TO_PTR(OSThread, self_)->id : -1, where);
        }
    }
    memcpy(fprev, fcur, fsize);
}

void run_next_thread(RDRAM_ARG1) {
    wcw2k_spraywatch(PASS_RDRAM "pop");
    // [wcw2k] Repair game-wiped contexts (see wcw2k_get_context); a queued thread whose
    // context is null even in the host map was genuinely destroyed — drop it.
    wcw2k_sched_log(PASS_RDRAM1);
    for (;;) {
        if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
            throw std::runtime_error("No threads left to run!\n");
        }

        // [wcw2k] anti-starvation valve (see wcw2k_pop_starved above).
        PTR(OSThread) to_run_ = wcw2k_pop_starved(PASS_RDRAM1);
        if (to_run_ == NULLPTR) {
            to_run_ = ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue);
        }
        OSThread* to_run = TO_PTR(OSThread, to_run_);
        UltraThreadContext* ctx = wcw2k_get_context(PASS_RDRAM to_run_);
        if (ctx == nullptr) {
            fprintf(stderr, "[wcw2k][thread] queued thread id=%d (0x%08X) has NULL context and no host record — dropping\n",
                to_run->id, (uint32_t)to_run_);
            continue;
        }
        debug_printf("[Scheduling] Resuming execution of thread %d\n", to_run->id);
        ctx->running.signal();
        return;
    }
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    // [wcw2k] wcw2k_get_context repairs the in-rdram pointer if the game wiped its own
    // TCB (WM2000's mode transitions); dump + park only if genuinely destroyed.
    UltraThreadContext* cur_context = wcw2k_get_context(PASS_RDRAM thread_self);
    if (cur_context == nullptr) {
        OSThread* self = TO_PTR(OSThread, thread_self);
        fprintf(stderr, "[wcw2k][thread] yielding thread id=%d self=0x%08X has NULL context and no host record — parking forever\n",
            self->id, (uint32_t)thread_self);
        wcw2k_dump_thread_struct(PASS_RDRAM thread_self);
        run_next_thread(PASS_RDRAM1);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM cur_context);
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* cur_context = wcw2k_get_context(PASS_RDRAM thread_self);
    resume_thread(PASS_RDRAM t);
    // [wcw2k] see run_next_thread_and_wait — execution was already handed to t.
    if (cur_context == nullptr) {
        OSThread* self = TO_PTR(OSThread, thread_self);
        fprintf(stderr, "[wcw2k][thread] resuming-and-waiting thread id=%d self=0x%08X has NULL context and no host record — parking forever\n",
            self->id, (uint32_t)thread_self);
        wcw2k_dump_thread_struct(PASS_RDRAM thread_self);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    wait_for_resumed(PASS_RDRAM cur_context);
}

static void _thread_func(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* thread_context) {
    OSThread *self = TO_PTR(OSThread, self_);
    debug_printf("[Thread] Thread created: %d\n", self->id);
    // [wcw] map game thread ids to their entry PCs (to identify threads in traces)
    // [wcw2k] + stack pointer: to check whether a thread's stack lies inside the region
    // WM2000's transitions spray (boot48 crash: thread 6 resumed with a saved reg
    // restored as garbage from a stack slot).
    fprintf(stderr, "[wcw][thread] id=%d entry=0x%08X pri=%d struct=0x%08X sp=0x%08X\n", self->id, (unsigned)(uint32_t)entrypoint, self->priority, (uint32_t)self_, (uint32_t)self->sp);
    thread_self = self_;
    is_game_thread = true;

    // Set the thread name
    ultramodern::set_native_thread_name(ultramodern::threads::get_game_thread_name(self));
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Signal the initialized semaphore to indicate that this thread can be started.
    thread_context->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", self->id);

    // Wait until the thread is marked as running.
    try {
        wait_for_resumed(PASS_RDRAM thread_context);
    } catch (ultramodern::thread_terminated& terminated) {
    }

    // Make sure the thread wasn't replaced or destroyed before it was started.
    if (self->context == thread_context) {
        debug_printf("[Thread] Thread started: %d\n", self->id);
        try {
            // Run the thread's function with the provided argument.
            run_thread_function(PASS_RDRAM entrypoint, self->sp, arg);
        } catch (ultramodern::thread_terminated& terminated) {
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", self->id);
    }

    // Check if the thread hasn't been destroyed or replaced. If so, then the thread terminated or destroyed itself,
    // so mark this thread as destroyed and run the next queued thread.
    // [wcw2k] compare through the repairing accessor (a game-wiped context is still
    // "ours"), and drop the host-map record — this thread is gone for real.
    UltraThreadContext* effective_context = wcw2k_get_context(PASS_RDRAM self_);
    wcw2k_unregister_context(self_, thread_context);
    if (effective_context == thread_context) {
        self->context = nullptr;
        run_next_thread(PASS_RDRAM1);
    }

    // Dispose of this thread now that it's completed or terminated.
    ultramodern::cleanup_thread(thread_context);
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    debug_printf("[os] Start Thread %d\n", t->id);

    // If this is a game thread, insert the new thread into the running queue and then check the running queue.
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    // Otherwise, immediately start the thread and terminate this one.
    else {
        t->state = OSThreadState::QUEUED;
        resume_thread(PASS_RDRAM t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    OSThread *t = TO_PTR(OSThread, t_);
    
    // [wcw2k] DIAGNOSTIC: struct reuse while a live host thread still references it
    // (WM2000 recreates threads on the same OSThread structs at menu transitions).
    if (t->context != nullptr) {
        fprintf(stderr, "[wcw2k][thread] osCreateThread REUSING struct 0x%08X (old id=%d, ctx alive) for new id=%d entry=0x%08X\n",
            (uint32_t)t_, t->id, id, (uint32_t)entrypoint);
    }

    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame

    // Spawn a new thread, which will immediately pause itself and wait until it's been started.
    // Pass the context as an argument to the thread function to ensure that it can't get cleared before the thread captures its value.
    UltraThreadContext* context = new UltraThreadContext{};
    t->context = context;
    wcw2k_register_context(t_, context, pri, id); // [wcw2k] authoritative host-side copy
    context->host_thread = std::thread{_thread_func, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Wait until the thread is initialized to indicate that it's ready to be started.
    context->initialized.wait();
    debug_printf("[os] Thread %d is ready to be started\n", t->id);
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    // Check if the thread is stopping itself (arg is null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        assert(false);
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    // [wcw2k] DIAGNOSTIC: trace thread churn (menu-transition crash investigation).
    fprintf(stderr, "[wcw2k][thread] osDestroyThread target=0x%08X id=%d state=%d ctx=%s (self=0x%08X)\n",
        (uint32_t)t_, t->id, (int)t->state, t->context ? "live" : "null", (uint32_t)thread_self);
    // Check if the thread is destroying itself (arg is null or thread_self)
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Otherwise if the thread isn't stopped, remove it from its currrent queue., 
    if (t->state != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
    }
    // Check if the thread has already been destroyed to prevent destroying it again.
    // [wcw2k] repair a game-wiped pointer first so wiped-then-destroyed threads get
    // full cleanup, and drop the host-map record — this is genuine destruction.
    UltraThreadContext* cur_context = wcw2k_get_context(PASS_RDRAM t_);
    wcw2k_unregister_context(t_, cur_context);
    if (cur_context != nullptr) {
        // Mark the target thread as destroyed and resume it. When it starts it'll check this and terminate itself instead of resuming.
        t->context = nullptr;
        cur_context->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (t->priority != pri) {
        t->priority = pri;
        wcw2k_update_priority(t_, pri); // [wcw2k] keep the host-side record in sync

        if (t_ != ultramodern::this_thread() && t->state != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            ultramodern::thread_queue_insert(PASS_RDRAM t->queue, t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->priority;
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            to_delete->host_thread.join();
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    deleted_threads.enqueue(cur_context);
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}
