
#pragma once

#include <cstdint>
#include <functional>

#include "entities/Entity.h"

namespace lva::entities {

// Config switch that starts / stops the in-process Sendspin client, like
// ESPHome's "sendspin" switch of type "enabled".
class SendspinSwitchEntity final : public Entity {
   public:
    using Getter = std::function<bool()>;
    using Setter = std::function<void(bool)>;

    SendspinSwitchEntity(std::uint32_t key, Getter getter, Setter setter);

    void OnListEntities(const ResponseSink& sink) override;
    void OnSubscribeStates(const ResponseSink& sink) override;
    void OnCommand(const ::google::protobuf::MessageLite& request,
                   std::uint32_t request_msg_type_id,
                   const ResponseSink& sink) override;

   private:
    Getter getter_;
    Setter setter_;
};

}  // namespace lva::entities
