#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
    int64_t enqueue_ms; // [wcw fix] set at enqueue; bounds how long an undeliverable message may requeue
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

// [wcw fix] Grace window for undeliverable requeue_if_blocked messages, after which they are
// dropped. On hardware, completion/event messages (PI DMA done, SI/VI/AI events) are posted with
// OS_MESG_NOBLOCK and silently DROPPED if the target queue is full — a game may legally
// fire-and-forget DMAs (WCW does, from three separate DMA queues) and never receive them.
// requeue_if_blocked exists because librecomp's DMAs complete instantly (the completion can
// arrive while the game's queue is momentarily full, where real DMA latency would have let the
// game drain it first), so messages must be retried briefly — but retrying FOREVER makes every
// orphaned completion permanent: the external_messages backlog grows without bound (~55/s
// during WCW gameplay) and every osSendMesg/osRecvMesg drains-and-requeues the whole backlog,
// i.e. O(backlog) work per call under message_mutex. Measured result: game slowdown and starved
// message delivery (SFX loss) after ~4 minutes of play, persisting until restart. One second is
// orders of magnitude longer than any transient queue-full window (a frame or two) yet still
// emulates the hardware drop.
constexpr int64_t requeue_grace_ms = 1000;

static int64_t wcw_steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked, wcw_steady_ms()});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

// [wcw] DIAGNOSTIC counters for the 1/s [wcw][health] line: requeue churn (undeliverable
// requeue_if_blocked messages put back on external_messages), per-second delivery counts,
// and messages dropped after exhausting the requeue grace window.
std::atomic_int wcw_requeues{0}, wcw_extmsg_delivered{0}, wcw_extmsg_dropped{0};

// [wcw] DIAGNOSTIC: small histogram of the (mq, msg) pairs being requeued, to identify WHICH
// message is undeliverable when the backlog grows. Lossy/racy by design (diagnostic only).
struct WcwRequeueSlot { std::atomic<uint64_t> key{0}; std::atomic<uint32_t> count{0}; };
static WcwRequeueSlot wcw_requeue_hist[4];
static void wcw_note_requeue(PTR(OSMesgQueue) mq, OSMesg msg) {
    uint64_t key = ((uint64_t)(uint32_t)mq << 32) | (uint32_t)msg;
    for (auto& slot : wcw_requeue_hist) {
        uint64_t cur = slot.key.load(std::memory_order_relaxed);
        if (cur == key) { slot.count.fetch_add(1, std::memory_order_relaxed); return; }
        if (cur == 0) { slot.key.store(key, std::memory_order_relaxed); slot.count.fetch_add(1, std::memory_order_relaxed); return; }
    }
}

// [wcw2k] DIAGNOSTIC: per-(mq,msg) histogram of successful EXTERNAL deliveries, printed on the
// 1/s health line. Attributes an elevated del/s (e.g. WM2000's ~700/s vs ~190/s healthy — the
// high-priority audio-chain saturation that starves the pri-8 game loop) to its source queue.
static WcwRequeueSlot wcw2k_deliver_hist[8];
static void wcw2k_note_delivery(PTR(OSMesgQueue) mq, OSMesg msg) {
    uint64_t key = ((uint64_t)(uint32_t)mq << 32) | (uint32_t)msg;
    for (auto& slot : wcw2k_deliver_hist) {
        uint64_t cur = slot.key.load(std::memory_order_relaxed);
        if (cur == key) { slot.count.fetch_add(1, std::memory_order_relaxed); return; }
        if (cur == 0) { slot.key.store(key, std::memory_order_relaxed); slot.count.fetch_add(1, std::memory_order_relaxed); return; }
    }
}

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    int64_t now_ms = wcw_steady_ms();
    while (external_messages.try_dequeue(to_send)) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            // [wcw fix] Undeliverable: retry within the grace window, then drop (hardware
            // semantics — NOBLOCK completion posts into a full queue are lost).
            if (now_ms - to_send.enqueue_ms <= requeue_grace_ms) {
                requeued_messages.push_back(to_send);
                wcw_note_requeue(to_send.mq, to_send.mesg);
            }
            else {
                wcw_extmsg_dropped.fetch_add(1);
            }
        }
        else {
            wcw_extmsg_delivered.fetch_add(1);
            wcw2k_note_delivery(to_send.mq, to_send.mesg);
        }
    }
    wcw_requeues.fetch_add((int)requeued_messages.size());
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

// [wcw] DIAGNOSTIC (env WCW_HEALTH_LOG=1, 1 line/s): game health snapshot. viswaps/s = real
// game frame rate; ext=N = external_messages backlog (undeliverable requeued messages make
// this grow — every osSendMesg/osRecvMesg on a game thread drains this queue, so a standing
// backlog is O(N) work per call and a direct slowdown mechanism); rq/s = requeue churn;
// del/s = deliveries; drop/s = orphaned messages expired by the requeue grace window. This is
// the instrumentation that root-caused the 4-minute in-match slowdown/SFX-loss (see the
// requeue_grace_ms comment above).
void wcw_health_tick() {
    static const bool enabled = getenv("WCW_HEALTH_LOG") != nullptr;
    if (!enabled) {
        return;
    }
    extern std::atomic_int wcw_viswaps;
    static int64_t last_ms = 0;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_ms == 0) { last_ms = now; return; }
    if (now - last_ms < 1000) { return; }
    last_ms = now;
    char hist[256] = "";
    for (auto& slot : wcw_requeue_hist) {
        uint64_t key = slot.key.load(std::memory_order_relaxed);
        uint32_t cnt = slot.count.exchange(0, std::memory_order_relaxed);
        if (key != 0 && cnt != 0) {
            size_t len = strlen(hist);
            snprintf(hist + len, sizeof(hist) - len, " mq=0x%X/msg=0x%X:%u",
                (uint32_t)(key >> 32), (uint32_t)key, cnt);
        }
    }
    // [wcw2k] per-mq delivery attribution (see wcw2k_deliver_hist above).
    for (auto& slot : wcw2k_deliver_hist) {
        uint64_t key = slot.key.load(std::memory_order_relaxed);
        uint32_t cnt = slot.count.exchange(0, std::memory_order_relaxed);
        if (key != 0 && cnt != 0) {
            size_t len = strlen(hist);
            snprintf(hist + len, sizeof(hist) - len, " del[0x%X/0x%X]:%u",
                (uint32_t)(key >> 32), (uint32_t)key, cnt);
        }
    }
    fprintf(stderr, "[wcw][health] vis/s=%d ext=%zu rq/s=%d del/s=%d drop/s=%d%s\n",
        wcw_viswaps.exchange(0), external_messages.size_approx(),
        wcw_requeues.exchange(0), wcw_extmsg_delivered.exchange(0),
        wcw_extmsg_dropped.exchange(0), hist);
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    // [wcw fix] Drain every available message per wake instead of processing exactly one.
    // The single-message version deadlocked: on delivery failure it re-enqueued immediately,
    // and moodycamel's consumer bias then dequeued that same producer's item right back —
    // an undeliverable requeue_if_blocked message (full 1-deep target queue) spun forever
    // and starved every other producer's messages (VI retrace included), freezing the game.
    QueuedMessage to_send;
    thread_local std::vector<QueuedMessage> requeued_messages;
    requeued_messages.clear();
    bool delivered_any = false;
    external_messages.wait_dequeue(to_send);
    int64_t now_ms = wcw_steady_ms();
    do {
        if (do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false)) {
            delivered_any = true;
            wcw2k_note_delivery(to_send.mq, to_send.mesg);
        }
        else if (to_send.requeue_if_blocked) {
            // [wcw fix] Same grace-window drop as dequeue_external_messages above.
            if (now_ms - to_send.enqueue_ms <= requeue_grace_ms) {
                requeued_messages.push_back(to_send);
                wcw_note_requeue(to_send.mq, to_send.mesg);
            }
            else {
                wcw_extmsg_dropped.fetch_add(1);
            }
        }
    } while (external_messages.try_dequeue(to_send));
    wcw_requeues.fetch_add((int)requeued_messages.size());
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
    // If nothing was deliverable, don't burn a core cycling the requeued messages.
    if (!delivered_any && !requeued_messages.empty()) {
        ultramodern::sleep_milliseconds(1);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            // [wcw fix] Same grace-window drop as dequeue_external_messages above.
            if (wcw_steady_ms() - to_send.enqueue_ms <= requeue_grace_ms) {
                external_messages.enqueue(to_send);
            }
            else {
                wcw_extmsg_dropped.fetch_add(1);
            }
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    // [wcw2k] DIAGNOSTIC: WM2000's post-attract crash traced to message values landing
    // in thread 6's stack (func_800E2704's saved-s0 slot 0x80083900 receives retrace/
    // task-done message values). Log any queue whose ring buffer lands in that region
    // to identify the creator.
    if ((uint32_t)msg >= 0x80080000u && (uint32_t)msg < 0x80084000u) {
        fprintf(stderr, "[wcw2k][mq] osCreateMesgQueue mq=0x%08X BUFFER=0x%08X count=%d (in thread-6 TCB/stack region!)\n",
            (uint32_t)mq_, (uint32_t)msg, count);
    }
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

// [wcw] DIAGNOSTIC counters: SP/DP completion delivery accounting (msg 0x29B/0x29C).
std::atomic_int wcw_spb_delivered{0}, wcw_spb_full{0}, wcw_dpc_delivered{0}, wcw_dpc_full{0};

// [wcw] DIAGNOSTIC: AI/VI event delivery accounting (enqueue timestamps set in events.cpp's
// VI thread; latency = enqueue -> do_send delivery). Drops = target queue already full.
#include <chrono>
static int64_t wcw_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<uint32_t> wcw_ai_mq{0}, wcw_vi_mq{0};
std::atomic<int64_t> wcw_ai_enq_ms{0}, wcw_vi_enq_ms{0};
std::atomic_int wcw_ai_del{0}, wcw_ai_drop{0}, wcw_vi_del{0}, wcw_vi_drop{0};
std::atomic<int64_t> wcw_ai_maxlat{0}, wcw_vi_maxlat{0};

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            if ((uint32_t)msg == 0x29B) wcw_spb_full.fetch_add(1);
            if ((uint32_t)msg == 0x29C) wcw_dpc_full.fetch_add(1);
            if ((uint32_t)mq_ == wcw_ai_mq.load()) wcw_ai_drop.fetch_add(1);
            if ((uint32_t)mq_ == wcw_vi_mq.load()) wcw_vi_drop.fetch_add(1);
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        // ([wcw] NOTE: an earlier diagnostic insertion here accidentally rebound this else to the
        // msg==0x29C check, letting blocking 0x29C sends post into a full queue. Restored.)
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            // [wcw2k] WCW_BLOCK_LOG=1: trace where threads park (wedge diagnosis)
            static const bool wcw_block_log = getenv("WCW_BLOCK_LOG") != nullptr;
            if (wcw_block_log) fprintf(stderr, "[wcw2k][block] id=%d self=0x%08X SEND mq=0x%08X\n",
                TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)ultramodern::this_thread(), (uint32_t)mq_);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    if ((uint32_t)msg == 0x29B) wcw_spb_delivered.fetch_add(1);
    if ((uint32_t)msg == 0x29C) wcw_dpc_delivered.fetch_add(1);
    if ((uint32_t)mq_ == wcw_ai_mq.load()) {
        wcw_ai_del.fetch_add(1);
        int64_t lat = wcw_now_ms() - wcw_ai_enq_ms.load();
        if (lat > wcw_ai_maxlat.load()) wcw_ai_maxlat.store(lat);
    }
    if ((uint32_t)mq_ == wcw_vi_mq.load()) {
        wcw_vi_del.fetch_add(1);
        int64_t lat = wcw_now_ms() - wcw_vi_enq_ms.load();
        if (lat > wcw_vi_maxlat.load()) wcw_vi_maxlat.store(lat);
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer full
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            // [wcw2k] WCW_BLOCK_LOG=1: trace where threads park (wedge diagnosis)
            static const bool wcw_block_log = getenv("WCW_BLOCK_LOG") != nullptr;
            if (wcw_block_log) fprintf(stderr, "[wcw2k][block] id=%d self=0x%08X RECV mq=0x%08X\n",
                TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)ultramodern::this_thread(), (uint32_t)mq_);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;

    // [wcw2k] DIAGNOSTIC: sends into thread 6's upper stack frames (the func_800E2704
    // saved-s0 slot 0x80083900 corruption — bringup session 5 part 5). Log the target
    // mq, its ring buffer, and the sender.
    if (((uint32_t)mq_ >= 0x80083800u && (uint32_t)mq_ < 0x80083A00u) ||
        ((uint32_t)mq->msg >= 0x80083800u && (uint32_t)mq->msg < 0x80083A00u)) {
        fprintf(stderr, "[wcw2k][send] mq=0x%08X buf=0x%08X valid=%d msg=0x%08X sender=0x%08X game=%d\n",
            (uint32_t)mq_, (uint32_t)mq->msg, mq->validCount, (uint32_t)msg,
            ultramodern::is_game_thread() ? (uint32_t)ultramodern::this_thread() : 0,
            (int)ultramodern::is_game_thread());
    }

    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // [wcw] DIAGNOSTIC: every frame-ready (send to 0x80040FC0) log the present pointer the game
    // left in 0x80040FFC — nonzero = func_800033A0 present-with-DL, zero = func_800033E8 sync-only.
    if ((uint32_t)mq_ == 0x80040FC0) {
        static int fn = 0;
        if (fn++ < 80) {
            uint32_t off = 0x40FFC;
            uint32_t pp = ((uint32_t)rdram[(off+0)^3] << 24) | ((uint32_t)rdram[(off+1)^3] << 16)
                        | ((uint32_t)rdram[(off+2)^3] << 8)  | ((uint32_t)rdram[(off+3)^3]);
            fprintf(stderr, "[wcw][frame#%d] presentptr=0x%08X\n", fn, pp);
        }
    }

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    
    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // [wcw] DIAGNOSTIC: trace recv calls to find where the game thread parks.
    static int rn = 0;
    if (rn < 80) fprintf(stderr, "[wcw][recv#%d] mq=0x%X block=%d validCount=%d\n", rn, (unsigned)mq_, (int)(flags == OS_MESG_BLOCK), mq ? (int)mq->validCount : -1);
#ifdef _WIN32
    if (rn == 0 && flags == OS_MESG_BLOCK) {
        void* fr[24]; unsigned short n = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);
        fprintf(stderr, "[wcw][recv#0] base=%p frames:", (void*)base);
        for (unsigned short i = 0; i < n; i++) fprintf(stderr, " +0x%llX", (unsigned long long)((uintptr_t)fr[i] - (uintptr_t)base));
        fprintf(stderr, "\n");
    }
#endif
    rn++;

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    if (rn <= 80) fprintf(stderr, "[wcw][recv#%d] -> %s\n", rn - 1, received ? "got msg" : "empty");
    return received ? 0 : -1;
}
