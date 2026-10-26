
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <google/protobuf/message_lite.h>
#include <span>
#include <string>
#include <vector>

#include "protocol/Frame.h"
#include "protocol/MessageRegistry.h"

namespace lva::state {
class ServerState;
}  // namespace lva::state

namespace lva::proto {

class Connection {
   public:
    Connection(int fd,
               lva::state::ServerState& state,
               std::string peer_label);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    int fd() const noexcept { return fd_; }

    bool closed() const noexcept { return closed_; }

    bool authenticated() const noexcept { return authenticated_; }

    // Set by SubscribeVoiceAssistantRequest; voice traffic goes only here.
    bool voice_subscribed() const noexcept { return voice_subscribed_; }

    bool OnReadable();

    bool OnWritable();

    template <typename T>
    bool Send(const T& message);

    std::function<void(std::uint32_t,
                       const ::google::protobuf::MessageLite&)>
        MakeSink();

    void Close();

    bool SendSerializedMessage(std::uint32_t msg_type_id,
                               const ::google::protobuf::MessageLite& msg);

   private:
    bool ProcessBuffer();
    bool DispatchFrame(const InboundFrame& frame);
    bool SendRaw(std::span<const std::uint8_t> bytes);

    int fd_;
    bool closed_ = false;
    bool authenticated_ = false;
    bool voice_subscribed_ = false;
    lva::state::ServerState& state_;
    std::string peer_label_;

    std::vector<std::uint8_t> read_buf_;

    std::vector<std::uint8_t> write_buf_;
};

template <typename T>
bool Connection::Send(const T& message) {
    return SendSerializedMessage(MessageId<T>(), message);
}

}  // namespace lva::proto
