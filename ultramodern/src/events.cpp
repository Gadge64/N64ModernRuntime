#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <queue>
#include <cstring>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/extensions.h"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
};

struct ScreenUpdateAction {
    double wcw_enqueue_ms; // [wcw] DIAGNOSTIC: set at VI-thread enqueue for latency measurement
    ultramodern::renderer::ViRegs regs;
};

struct UpdateConfigAction {
};

struct DummyWorkloadAction {
    int32_t fb_address;
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, DummyWorkloadAction>;

struct ViState {
    const OSViMode* mode;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            if (next_mode == nullptr) {
                static bool warned = false;
                if (!warned) { warned = true; fprintf(stderr, "[wcw][vi] update_vi: next_state->mode is NULL (cur_state=%d field=%d game_started=%d) - skipping\n", cur_state, field, (int)ultramodern::is_game_started()); }
                return;
            }
            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                yScale = 0;
                origin = framebuffer;
            }

            // TODO implement osViFade

            { // [wcw] DIAGNOSTIC (WCW_PRESENT_LUM=1): trace the ViState dance per tick.
                static const bool wcwTlog = getenv("WCW_PRESENT_LUM") != nullptr;
                if (wcwTlog) {
                    static FILE *tf = nullptr;
                    if (tf == nullptr) tf = fopen("wcw_vistate_log.csv", "w");
                    if (tf != nullptr) {
                        double ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
                        fprintf(tf, "%.1f,%d,0x%X,0x%X,0x%X\n", ms, cur_state,
                            (unsigned)states[0].framebuffer, (unsigned)states[1].framebuffer, (unsigned)origin);
                        fflush(tf);
                    }
                }
            }
            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;
            
            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    std::lock_guard lock{ events_context.message_mutex };
    fprintf(stderr, "[wcw][osSetEventMesg] event=%d mq=0x%X msg=0x%X\n", (int)event_id, (unsigned)mq_, (unsigned)msg);

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    // [wcw fix] the retrace event registration is global on real hardware (latest osViSetEvent
    // wins), but the ViStates here are double-buffered and never copy to each other — writing
    // only next_state leaves the stale registration live on the other parity. WCW re-registers
    // its retrace (new mq/msg/rate) at the end of its attract sequence; the stale parity then
    // starved the new one via the shared remaining_retraces counter and the game deadlocked.
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mq = mq_;
    next_state->msg = msg;
    next_state->retrace_count = retrace_count;
    ViState* cur_state = events_context.vi.get_cur_state();
    cur_state->mq = mq_;
    cur_state->msg = msg;
    cur_state->retrace_count = retrace_count;
    fprintf(stderr, "[wcw][osViSetEvent] mq=0x%X msg=0x%X retrace_count=%u\n",
        (unsigned)(uint32_t)mq_, (unsigned)(uint32_t)msg, retrace_count);
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = 1;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        // If the game hasn't started yet, set a dummy VI mode and origin.
        if (!ultramodern::is_game_started()) {
            static bool odd = false;
            set_dummy_vi(odd);
            odd = !odd;

            events_context.action_queue.enqueue(DummyWorkloadAction{events_context.vi.get_next_state()->framebuffer});
        }

        // Queue a screen update for the graphics thread with the current VI register state.
        // Doing this before the VI update is equivalent to updating the screen after the previous frame's scanout finished.
        events_context.action_queue.enqueue(ScreenUpdateAction{
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0,
            events_context.vi.regs });

        // Update VI registers and swap VI modes.
        events_context.vi.update_vi();

        { // [wcw] DIAGNOSTIC: 1/s health line (game frame rate + external-message backlog).
            extern void wcw_health_tick();
            wcw_health_tick();
        }

        { // [wcw] DIAGNOSTIC (env WCW_VI_LOG=1): scanout health — log origin changes
          // (framebuffer cycling pattern) and black scanouts (hStart==0 = VI_STATE_BLACK),
          // to characterize visual flicker. First ~200 origin changes individually + 1/s summary.
            static const bool wcw_vi_log = getenv("WCW_VI_LOG") != nullptr;
            static uint32_t last_origin = 0xFFFFFFFF; static int vis = 0, changes = 0, blacks = 0, logged = 0;
            uint32_t org = events_context.vi.regs.VI_ORIGIN_REG;
            uint32_t hs = events_context.vi.regs.VI_H_START_REG;
            vis++;
            if (hs == 0) blacks++;
            if (org != last_origin) {
                changes++;
                if (logged < 200 && wcw_vi_log) { fprintf(stderr, "[wcw][scan] vi=%llu origin 0x%08X -> 0x%08X hstart=0x%X\n", (unsigned long long)total_vis, last_origin, org, hs); logged++; }
                last_origin = org;
            }
            if (vis >= 60) {
                extern std::atomic_int wcw_ai_del, wcw_ai_drop, wcw_vi_del, wcw_vi_drop;
                extern std::atomic<int64_t> wcw_ai_maxlat, wcw_vi_maxlat;
                int ai_del = wcw_ai_del.exchange(0), ai_drop = wcw_ai_drop.exchange(0);
                int vi_del = wcw_vi_del.exchange(0), vi_drop = wcw_vi_drop.exchange(0);
                long long ai_maxlat = wcw_ai_maxlat.exchange(0), vi_maxlat = wcw_vi_maxlat.exchange(0);
                if (wcw_vi_log) {
                    fprintf(stderr, "[wcw][scan] 60 VIs: origin-changes=%d blacks=%d | ai del=%d drop=%d maxlat=%lldms | vi del=%d drop=%d maxlat=%lldms\n",
                        changes, blacks, ai_del, ai_drop, ai_maxlat, vi_del, vi_drop, vi_maxlat);
                }
                vis = 0; changes = 0; blacks = 0;
            }
        }

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            remaining_retraces--;
            
            std::lock_guard lock{ events_context.message_mutex };
            ViState* cur_state = events_context.vi.get_cur_state();
            { // [wcw] DIAGNOSTIC (env WCW_VI_LOG=1): report VI/AI retrace-queue registration once a
              // second, plus the fixed-segment game-mode state var @ 0x80034834 (0=overlay A,
              // 1=overlay B — it advances 0->1 when B loads then sticks, i.e. the overlay-B internal
              // state is the stall). Read from rdram with the recomp byte-swap (byte at offset X = rdram[X^3]).
                static const bool wcw_vi_log = getenv("WCW_VI_LOG") != nullptr;
                static int tick = 0;
                if (((tick++ % 60) == 0) && wcw_vi_log) {
                    uint8_t* rd = events_context.rdram;
                    auto rd32 = [rd](uint32_t off) -> uint32_t {
                        return ((uint32_t)rd[(off+0)^3] << 24) | ((uint32_t)rd[(off+1)^3] << 16)
                             | ((uint32_t)rd[(off+2)^3] << 8)  | ((uint32_t)rd[(off+3)^3]);
                    };
                    extern std::atomic_int wcw_spb_delivered, wcw_spb_full, wcw_dpc_delivered, wcw_dpc_full;
                    extern std::atomic_int wcw_spc_calls, wcw_dpc_calls;
                    fprintf(stderr, "[wcw][vi] tick %d: gamemode=0x%X fb=0x%08X | state=0x%X hstart=0x%X origin=0x%X dpc=%d | vimq=0x%X vimsg=0x%X rc=%d aimq=0x%X\n",
                        tick, rd32(0x34834), (unsigned)cur_state->framebuffer,
                        cur_state->state, events_context.vi.regs.VI_H_START_REG, events_context.vi.regs.VI_ORIGIN_REG,
                        wcw_dpc_calls.load(),
                        (unsigned)cur_state->mq, (unsigned)cur_state->msg, cur_state->retrace_count,
                        (unsigned)events_context.ai.mq);
                }
            }
            // [wcw fix] was `== 0` with an unclamped reload: if cur_state->retrace_count ever reads
            // 0 once (e.g. mid VI-state swap during a mode change / osViBlack fade), the counter
            // reloads to 0, goes negative, and `== 0` never fires again — retrace events stop
            // permanently and every game thread eventually starves (frozen game, live process).
            // WCW hit this deterministically at the end of its attract sequence.
            if (remaining_retraces <= 0) {
                if (cur_state->mq != NULLPTR) {
                    // Send a message to the VI queue, and do not set it to be requeued if the queue was full.
                    // The worst case scenario is that the game misses a VI message and has to wait a little longer for the next.
                    { // [wcw] DIAGNOSTIC: stamp enqueue time for delivery-latency measurement (see mesgqueue.cpp)
                        extern std::atomic<uint32_t> wcw_vi_mq; extern std::atomic<int64_t> wcw_vi_enq_ms;
                        wcw_vi_mq.store((uint32_t)cur_state->mq);
                        wcw_vi_enq_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    }
                    ultramodern::enqueue_external_message(cur_state->mq, cur_state->msg, false, false);
                }
                remaining_retraces = (cur_state->retrace_count > 0) ? cur_state->retrace_count : 1;
            }
            if (events_context.ai.mq != NULLPTR) {
                // Send a message to the VI queue, and do not set it to be requeued if the queue was full for the same reason as the VI message above.
                { // [wcw] DIAGNOSTIC: stamp enqueue time for delivery-latency measurement (see mesgqueue.cpp)
                    extern std::atomic<uint32_t> wcw_ai_mq; extern std::atomic<int64_t> wcw_ai_enq_ms;
                    wcw_ai_mq.store((uint32_t)events_context.ai.mq);
                    wcw_ai_enq_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                }
                ultramodern::enqueue_external_message(events_context.ai.mq, events_context.ai.msg, false, false);
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

// [wcw] DIAGNOSTIC counters: completion-message accounting (find where SP/DP dones get lost).
std::atomic_int wcw_spc_calls{0}, wcw_dpc_calls{0};

void sp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    wcw_spc_calls.fetch_add(1);
    ultramodern::enqueue_external_message(events_context.sp.mq, events_context.sp.msg, false, true);
}

void dp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    wcw_dpc_calls.fetch_add(1);
    ultramodern::enqueue_external_message(events_context.dp.mq, events_context.dp.msg, false, true);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            fprintf(stderr, "Failed to execute task type: %" PRIu32 "\n", task->t.type);
            ULTRAMODERN_QUICK_EXIT();
        }

        // Tell the game that the RSP has completed
        sp_complete();
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action first_action;
        if (events_context.action_queue.wait_dequeue_timed(first_action, 1ms)) {
            // [wcw fix] Drain the whole queue and drop STALE ScreenUpdateActions (keep only the
            // newest). Each ScreenUpdateAction is a snapshot of the VI regs at one 60Hz tick; a
            // standing backlog in this queue (SpTask display-list processing shares the thread)
            // otherwise delays scanout by that backlog — measured at 5-6 VIs (~90ms), enough that
            // the presented framebuffer overlapped the game re-clearing/redrawing it for its NEXT
            // frame -> 1 of 3 presents was pure black (constant visible flicker). Presenting only
            // the freshest VI snapshot bounds scanout latency to ~1 action regardless of backlog.
            thread_local std::vector<Action> action_batch;
            action_batch.clear();
            action_batch.push_back(std::move(first_action));
            {
                Action extra;
                while (events_context.action_queue.try_dequeue(extra)) {
                    action_batch.push_back(std::move(extra));
                }
            }
            size_t last_screen_update = SIZE_MAX;
            for (size_t i = 0; i < action_batch.size(); i++) {
                if (std::get_if<ScreenUpdateAction>(&action_batch[i]) != nullptr) {
                    last_screen_update = i;
                }
            }
            for (size_t action_index = 0; action_index < action_batch.size(); action_index++) {
                Action &action = action_batch[action_index];
                if ((std::get_if<ScreenUpdateAction>(&action) != nullptr) && (action_index != last_screen_update)) {
                    continue; // stale screen update — a newer VI snapshot is in this batch
                }
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Tell the game that the RSP completed instantly. This will allow it to queue other task types, but it won't
                // start another graphics task until the RDP is also complete. Games usually preserve the RSP inputs until the RDP
                // is finished as well, so sending this early shouldn't be an issue in most cases.
                // If this causes issues then the logic can be replaced with responding to yield requests.
                sp_complete();
                ultramodern::measure_input_latency();

                PTR(u64) displaylist = task_action->task.t.data_ptr;
                ultramodern::extensions::on_displaylist_submitted(displaylist);

                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                renderer_context->send_dl(&task_action->task);
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();
                { // [wcw] DIAGNOSTIC (WCW_PRESENT_LUM=1): per-task display-list processing cost on this thread.
                    static const bool wcwDlog = getenv("WCW_PRESENT_LUM") != nullptr;
                    if (wcwDlog) {
                        static FILE *df = nullptr;
                        if (df == nullptr) df = fopen("wcw_dl_log.csv", "w");
                        if (df != nullptr) {
                            double now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
                            fprintf(df, "%.1f,%.2f\n", now, std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count() / 1000.0);
                            fflush(df);
                        }
                    }
                }

                dp_complete();
                // TODO hook the parsed event up to the actual parsing point when a callback is added to RT64.
                ultramodern::extensions::on_displaylist_parsed(displaylist);
                ultramodern::extensions::on_displaylist_completed(displaylist);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                { // [wcw] DIAGNOSTIC (WCW_PRESENT_LUM=1): VI-enqueue -> gfx-thread processing latency.
                    static const bool wcwUlog = getenv("WCW_PRESENT_LUM") != nullptr;
                    if (wcwUlog) {
                        static FILE *uf = nullptr;
                        if (uf == nullptr) uf = fopen("wcw_supd_log.csv", "w");
                        if (uf != nullptr) {
                            double now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
                            fprintf(uf, "%.1f,%.1f\n", now, now - screen_update_action->wcw_enqueue_ms);
                            fflush(uf);
                        }
                    }
                }
                events_context.vi.update_screen_regs = screen_update_action->regs;
                renderer_context->update_screen();
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {
                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);
            }
            } // [wcw fix] end of drained-batch loop
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    next_state->control = next_state->mode->comRegs.ctrl;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

// [wcw] DIAGNOSTIC: per-second game-frame counter, reported in the [wcw][health] line
// (mesgqueue.cpp). osViSwapBuffer fires once per rendered game frame, so a drop in this
// rate is a direct measure of "the game slowed down" regardless of host present rate.
std::atomic_int wcw_viswaps{0};

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    std::lock_guard lock{ events_context.message_mutex };
    wcw_viswaps.fetch_add(1);
    // [wcw] DIAGNOSTIC: log swaps — if this never fires, VI scans out nothing (black screen)
    // regardless of what the RDP rendered.
    { static int n = 0; if (n++ < 30) fprintf(stderr, "[wcw][viswap#%d] fb=0x%08X\n", n, (unsigned)frameBufPtr); }
    { // [wcw] DIAGNOSTIC (WCW_PRESENT_LUM=1): timestamped swap log for present correlation.
        static const bool wcwVlog = getenv("WCW_PRESENT_LUM") != nullptr;
        if (wcwVlog) {
            static FILE *vf = nullptr;
            if (vf == nullptr) vf = fopen("wcw_swap_log.csv", "w");
            if (vf != nullptr) {
                double ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
                fprintf(vf, "%.1f,0x%X\n", ms, (unsigned)frameBufPtr);
                fflush(vf);
            }
        }
    }
    events_context.vi.get_next_state()->framebuffer = frameBufPtr;
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    std::lock_guard lock{ events_context.message_mutex };
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = mode;
    next_state->control = next_state->mode->comRegs.ctrl;
}

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* control_out = &next_state->control;
    if ((func & OS_VI_GAMMA_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        *control_out |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        *control_out &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        *control_out |= VI_CTRL_DITHER_FILTER_ON;
        *control_out &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        *control_out &= ~VI_CTRL_DITHER_FILTER_ON;
        *control_out |= next_state->mode->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_BLACK;
    } else {
        *state_out &= ~VI_STATE_BLACK;
    }
}

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.get_next_state()->framebuffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.get_cur_state()->framebuffer;
}

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    // Send gfx tasks to the graphics action queue
    if (task->t.type == M_GFXTASK) {
        events_context.action_queue.enqueue(SpTaskAction{ *task });
    }
    // Set all other tasks as the RSP task
    else {
        events_context.sp_task_queue.enqueue(task);
    }
}

void ultramodern::send_si_message() {
    ultramodern::enqueue_external_message(events_context.si.mq, events_context.si.msg, false, true);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    events_context.vi.thread = std::thread{ vi_thread_func };
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
