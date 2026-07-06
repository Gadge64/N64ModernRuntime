#include <thread>
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
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            requeued_messages.push_back(to_send);
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
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
    do {
        if (do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false)) {
            delivered_any = true;
        }
        else if (to_send.requeue_if_blocked) {
            requeued_messages.push_back(to_send);
        }
    } while (external_messages.try_dequeue(to_send));
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
            external_messages.enqueue(to_send);
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
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
