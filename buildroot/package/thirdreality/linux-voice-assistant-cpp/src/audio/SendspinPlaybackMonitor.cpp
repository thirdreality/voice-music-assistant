#include "audio/SendspinPlaybackMonitor.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <pulse/pulseaudio.h>

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "sendspin-monitor";

bool IsSendspinInput(const pa_sink_input_info* info) {
    if (info == nullptr || info->proplist == nullptr) return false;

    const char* app =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    const char* binary =
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    const char* media =
        pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME);

    return (app != nullptr && std::strcmp(app, "sendspin-client") == 0) ||
           (binary != nullptr &&
            std::strcmp(binary, "sendspin-client") == 0) ||
           (media != nullptr && std::strcmp(media, "Sendspin") == 0);
}

}  // namespace

SendspinPlaybackMonitor::SendspinPlaybackMonitor(Callback callback)
    : callback_(std::move(callback)) {}

SendspinPlaybackMonitor::~SendspinPlaybackMonitor() {
    Stop();
}

bool SendspinPlaybackMonitor::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    stop_requested_.store(false, std::memory_order_release);

    try {
        thread_ = std::thread(&SendspinPlaybackMonitor::ThreadMain, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void SendspinPlaybackMonitor::Stop() {
    stop_requested_.store(true, std::memory_order_release);

    if (thread_.joinable()) {
        thread_.join();
    }

    running_.store(false, std::memory_order_release);
}

void SendspinPlaybackMonitor::PublishState() {
    const bool active = !sendspin_inputs_.empty();

    if (active == published_active_) return;

    published_active_ = active;

    LVA_LOGI(kTag, "Sendspin playback %s",
             active ? "active" : "idle");

    if (callback_) {
        callback_(active);
    }
}

void SendspinPlaybackMonitor::ClearState() {
    sendspin_inputs_.clear();
    PublishState();
}

void SendspinPlaybackMonitor::QuerySinkInput(unsigned index) {
    if (context_ == nullptr) return;

    pa_operation* op =
        pa_context_get_sink_input_info(context_,
                                       index,
                                       &SendspinPlaybackMonitor::OnSinkInputInfo,
                                       this);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
}

void SendspinPlaybackMonitor::OnContextState(pa_context* /*ctx*/,
                                             void* /*userdata*/) {
    // ThreadMain polls pa_context_get_state(). Nothing is required here;
    // the callback is installed so PulseAudio wakes its mainloop promptly.
}

void SendspinPlaybackMonitor::OnSubscription(
    pa_context* ctx,
    pa_subscription_event_type_t event_type,
    uint32_t index,
    void* userdata) {
    auto* self = static_cast<SendspinPlaybackMonitor*>(userdata);
    if (self == nullptr) return;

    const auto type =
        static_cast<pa_subscription_event_type_t>(event_type);

    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) !=
        PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        return;
    }

    const auto operation =
        static_cast<pa_subscription_event_type_t>(
            type & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

    if (operation == PA_SUBSCRIPTION_EVENT_REMOVE) {
        self->sendspin_inputs_.erase(index);
        self->PublishState();
        return;
    }

    pa_operation* op =
        pa_context_get_sink_input_info(ctx,
                                       index,
                                       &SendspinPlaybackMonitor::OnSinkInputInfo,
                                       self);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
}

void SendspinPlaybackMonitor::OnSinkInputInfo(
    pa_context* /*ctx*/,
    const pa_sink_input_info* info,
    int eol,
    void* userdata) {

    auto* self = static_cast<SendspinPlaybackMonitor*>(userdata);
    if (self == nullptr || eol != 0 || info == nullptr) return;

    if (IsSendspinInput(info)) {
        self->sendspin_inputs_.insert(info->index);
    } else {
        self->sendspin_inputs_.erase(info->index);
    }

    self->PublishState();
}

void SendspinPlaybackMonitor::ThreadMain() {
    using namespace std::chrono_literals;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        mainloop_ = pa_mainloop_new();
        if (mainloop_ == nullptr) {
            LVA_LOGW(kTag, "pa_mainloop_new failed");
            std::this_thread::sleep_for(1s);
            continue;
        }

        context_ = pa_context_new(pa_mainloop_get_api(mainloop_),
                                  "linux-voice-assistant-sendspin-monitor");
        if (context_ == nullptr) {
            LVA_LOGW(kTag, "pa_context_new failed");
            pa_mainloop_free(mainloop_);
            mainloop_ = nullptr;
            std::this_thread::sleep_for(1s);
            continue;
        }

        pa_context_set_state_callback(
            context_,
            &SendspinPlaybackMonitor::OnContextState,
            this);

        if (pa_context_connect(context_,
                               nullptr,
                               PA_CONTEXT_NOFLAGS,
                               nullptr) < 0) {
            LVA_LOGW(kTag, "PulseAudio connect failed: %s",
                     pa_strerror(pa_context_errno(context_)));

            pa_context_unref(context_);
            context_ = nullptr;
            pa_mainloop_free(mainloop_);
            mainloop_ = nullptr;

            std::this_thread::sleep_for(1s);
            continue;
        }

        bool ready = false;
        bool failed = false;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            int retval = 0;
            if (pa_mainloop_iterate(mainloop_, 0, &retval) < 0) {
                failed = true;
                break;
            }

            const pa_context_state_t state = pa_context_get_state(context_);

            if (state == PA_CONTEXT_READY) {
                ready = true;
                break;
            }

            if (state == PA_CONTEXT_FAILED ||
                state == PA_CONTEXT_TERMINATED) {
                failed = true;
                break;
            }

            std::this_thread::sleep_for(20ms);
        }

        if (ready &&
            !stop_requested_.load(std::memory_order_acquire)) {

            pa_context_set_subscribe_callback(
                context_,
                &SendspinPlaybackMonitor::OnSubscription,
                this);

            if (pa_operation* op =
                    pa_context_subscribe(
                        context_,
                        PA_SUBSCRIPTION_MASK_SINK_INPUT,
                        nullptr,
                        nullptr)) {
                pa_operation_unref(op);
            }

            // Catch playback that was already active before the monitor
            // connected.
            if (pa_operation* op =
                    pa_context_get_sink_input_info_list(
                        context_,
                        &SendspinPlaybackMonitor::OnSinkInputInfo,
                        this)) {
                pa_operation_unref(op);
            }

            LVA_LOGI(kTag, "PulseAudio Sendspin monitor connected");

            while (!stop_requested_.load(std::memory_order_acquire)) {
                int retval = 0;

                if (pa_mainloop_iterate(mainloop_, 0, &retval) < 0) {
                    failed = true;
                    break;
                }

                const pa_context_state_t state =
                    pa_context_get_state(context_);

                if (state == PA_CONTEXT_FAILED ||
                    state == PA_CONTEXT_TERMINATED) {
                    failed = true;
                    break;
                }

                std::this_thread::sleep_for(20ms);
            }
        }

        ClearState();

        if (context_ != nullptr) {
            pa_context_disconnect(context_);
            pa_context_unref(context_);
            context_ = nullptr;
        }

        if (mainloop_ != nullptr) {
            pa_mainloop_free(mainloop_);
            mainloop_ = nullptr;
        }

        if (failed &&
            !stop_requested_.load(std::memory_order_acquire)) {
            LVA_LOGW(kTag,
                     "PulseAudio monitor disconnected; retrying");
            std::this_thread::sleep_for(1s);
        }
    }

    ClearState();
    running_.store(false, std::memory_order_release);
}

}  // namespace lva::audio
