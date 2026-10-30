#include "entities/NetworkSwitchEntity.h"
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "state/ServerState.h"
#include "protocol/MessageRegistry.h"
#include "util/Log.h"
#include "api.pb.h"
extern char** environ;

namespace lva::entities {
namespace {
int ApplyNtp() {
    // Read the exit status through a pipe: the process-wide SIGCHLD reaper
    // may reap the shell before this worker calls waitpid.
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    const char* args[] = {"/bin/sh", "-c",
        "\"$1\" >&2; printf '%s\\n' \"$?\"",
        "network-apply", "/usr/share/thirdreality/script/ntp-apply",
        nullptr};
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, args[0], &actions, nullptr,
                                 const_cast<char**>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);
    char status[16]{};
    ssize_t got = -1;
    if (rc == 0) {
        do { got = ::read(pipefd[0], status, sizeof(status) - 1); }
        while (got < 0 && errno == EINTR);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    ::close(pipefd[0]);
    return got > 0 && status[0] == '0' ? 0 : -1;
}
}
NetworkSwitchEntity::NetworkSwitchEntity(std::uint32_t key, state::ServerState& state,
                                       ApplyLauncher launcher)
    : Entity(key, "allow_background_internet", "Allow Background Internet Access"),
      state_(state), applied_(state.network_policy.Allowed()), launcher_(std::move(launcher)) {
    if (!launcher_) launcher_ = []() {
        return std::async(std::launch::async, ApplyNtp);
    };
    LVA_LOGI("network", "background internet initially %s", applied_ ? "enabled" : "disabled");
}

void NetworkSwitchEntity::OnListEntities(const ResponseSink& sink) {
    ListEntitiesSwitchResponse resp;
    resp.set_key(key()); resp.set_object_id(object_id()); resp.set_name(name());
    resp.set_entity_category(ENTITY_CATEGORY_CONFIG);
    resp.set_icon("mdi:cloud-outline");
    sink(lva::proto::kIdListEntitiesSwitchResponse, resp);
}
void NetworkSwitchEntity::OnSubscribeStates(const ResponseSink& sink) {
    SwitchStateResponse resp; resp.set_key(key()); resp.set_state(applied_);
    sink(lva::proto::kIdSwitchStateResponse, resp);
}
void NetworkSwitchEntity::OnCommand(const google::protobuf::MessageLite& request,
                                   std::uint32_t type, const ResponseSink& sink) {
    if (type != lva::proto::kIdSwitchCommandRequest) return;
    const auto& cmd = static_cast<const SwitchCommandRequest&>(request);
    if (cmd.key() != key()) return;
    LVA_LOGI("network", "background internet request=%d pending=%d", cmd.state(), pending_);
    if (!pending_) {
        if (!state_.network_policy.Save(cmd.state())) {
            state_.NotifyHa("network_policy", "Network settings",
                "Could not save network settings. Please retry.");
        } else {
            script_done_ = false;
            try {
                apply_ = launcher_();
                if (!apply_.valid()) {
                    FailApply("launcher returned no future");
                } else {
                    pending_ = true;
                }
            } catch (const std::exception& error) {
                FailApply(error.what());
            } catch (...) {
                FailApply("could not start network settings worker");
            }
        }
    }
    // Acknowledge the new value only after the system policy is applied.
    OnSubscribeStates(sink);
}
void NetworkSwitchEntity::FailApply(const char* reason) {
    pending_ = false;
    script_done_ = false;
    LVA_LOGW("network", "network settings not confirmed: %s", reason);
    state_.NotifyHa("network_policy", "Network settings",
        "Settings were saved, but could not be applied completely. "
        "The switch has not been confirmed; retry the command.");
}
void NetworkSwitchEntity::OnPeriodicTick(bool busy) {
    if (!pending_) return;
    if (!script_done_) {
        int result;
        try {
            if (!apply_.valid()) {
                FailApply("pending network settings have no future");
                return;
            }
            if (apply_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            result = apply_.get();
        } catch (const std::exception& error) {
            FailApply(error.what());
            return;
        } catch (...) {
            FailApply("network settings worker failed");
            return;
        }
        LVA_LOGI("network", "ntp-apply finished: %d", result);
        if (result != 0) {
            FailApply("ntp-apply returned failure");
            return;
        }
        script_done_ = true;
    }
    if (busy) return;
    pending_ = false;
    applied_ = state_.network_policy.Allowed();
    LVA_LOGI("network", "background internet applied: %d", applied_);
    if (state_.broadcast) OnSubscribeStates(state_.broadcast);
}
}
