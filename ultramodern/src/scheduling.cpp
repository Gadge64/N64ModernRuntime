#include "ultramodern/ultramodern.hpp"

void ultramodern::schedule_running_thread(RDRAM_ARG PTR(OSThread) t_) {
    debug_printf("[Scheduling] Adding thread %d to the running queue\n", TO_PTR(OSThread, t_)->id);
    // [wcw2k] thread_queue_insert orders by a raw rdram read of t->priority; repair a
    // game-wiped TCB first or garbage priority mis-sorts the thread permanently
    // (the post-transition wedge — see wcw2k_repair_thread in threads.cpp).
    ultramodern::wcw2k_repair_thread(PASS_RDRAM t_);
    thread_queue_insert(PASS_RDRAM running_queue, t_);
    TO_PTR(OSThread, t_)->state = OSThreadState::QUEUED;
}

void swap_to_thread(RDRAM_ARG OSThread *to) {
    debug_printf("[Scheduling] Thread %d giving execution to thread %d\n", TO_PTR(OSThread, ultramodern::this_thread())->id, to->id);
    // Insert this thread in the running queue.
    // [wcw2k] repair self before the priority-ordered insert (see schedule_running_thread).
    ultramodern::wcw2k_repair_thread(PASS_RDRAM ultramodern::this_thread());
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread());
    TO_PTR(OSThread, ultramodern::this_thread())->state = OSThreadState::QUEUED;
    // Unpause the target thread and wait for this one to be unpaused.
    ultramodern::resume_thread_and_wait(PASS_RDRAM to);
}

void ultramodern::check_running_queue(RDRAM_ARG1) {
    // [wcw2k] a starvation-valve-boosted thread keeps the CPU for its boost window
    // (see wcw2k_pop_starved in threads.cpp) — skipping the priority preemption here
    // is what lets it do a frame of work instead of one OS call per valve fire.
    if (ultramodern::wcw2k_thread_boosted(ultramodern::this_thread())) {
        return;
    }
    // Check if there are any threads in the running queue.
    if (!thread_queue_empty(PASS_RDRAM running_queue)) {
        // Check if the highest priority thread in the queue is higher priority than the current thread.
        // [wcw2k] repair both TCBs before the raw priority compare — a wiped garbage
        // priority on either side yields a wrong (and sticky) scheduling decision.
        PTR(OSThread) next_thread_ = ultramodern::thread_queue_peek(PASS_RDRAM running_queue);
        ultramodern::wcw2k_repair_thread(PASS_RDRAM next_thread_);
        ultramodern::wcw2k_repair_thread(PASS_RDRAM ultramodern::this_thread());
        OSThread* next_thread = TO_PTR(OSThread, next_thread_);
        OSThread* self = TO_PTR(OSThread, ultramodern::this_thread());
        if (next_thread->priority > self->priority) {
            ultramodern::thread_queue_pop(PASS_RDRAM running_queue);
            // Swap to the higher priority thread.
            swap_to_thread(PASS_RDRAM next_thread);
        }
    }
}

extern "C" void pause_self(RDRAM_ARG1) {
    while (true) {
        // Wait until an external message arrives, then allow the next thread to run.
        ultramodern::wait_for_external_message(PASS_RDRAM1);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" void yield_self(RDRAM_ARG1) {
    ultramodern::wait_for_external_message(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

extern "C" void yield_self_1ms(RDRAM_ARG1) {
    ultramodern::wait_for_external_message_timed(PASS_RDRAM1, 1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}
