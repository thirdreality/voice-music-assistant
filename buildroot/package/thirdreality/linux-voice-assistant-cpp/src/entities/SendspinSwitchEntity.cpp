#include "entities/SendspinSwitchEntity.h"

#include <utility>

#include "protocol/MessageRegistry.h"
#include "util/Log.h"

#include "api.pb.h"

namespace lva::entities {

namespace {
constexpr const char* kTag = "sendspin_sw";
constexpr const char* kObjectId = "sendspin";
constexpr const char* kDisplayName = "Sendspin";
}  // namespace

SendspinSwitchEntity::SendspinSwitchEntity(std::uint32_t key, Getter getter,
                                           Setter setter)
    : Entity(key, kObjectId, kDisplayName),
      getter_(std::move(getter)),
      setter_(std::move(setter)) {}

void SendspinSwitchEntity::OnListEntities(const ResponseSink& sink) {
    ::ListEntitiesSwitchResponse resp;
    resp.set_object_id(object_id());
    resp.set_key(key());
    resp.set_name(name());
    resp.set_entity_category(::ENTITY_CATEGORY_CONFIG);
    resp.set_icon("mdi:speaker-multiple");
    sink(lva::proto::kIdListEntitiesSwitchResponse, resp);
}

void SendspinSwitchEntity::OnSubscribeStates(const ResponseSink& sink) {
    ::SwitchStateResponse resp;
    resp.set_key(key());
    resp.set_state(getter_());
    sink(lva::proto::kIdSwitchStateResponse, resp);
}

void SendspinSwitchEntity::OnCommand(
    const ::google::protobuf::MessageLite& request,
    std::uint32_t request_msg_type_id,
    const ResponseSink& sink) {
    if (request_msg_type_id != lva::proto::kIdSwitchCommandRequest) return;
    const auto& cmd = static_cast<const ::SwitchCommandRequest&>(request);
    if (cmd.key() != key()) return;

    LVA_LOGI(kTag, "command: sendspin -> %s", cmd.state() ? "on" : "off");
    setter_(cmd.state());

    ::SwitchStateResponse resp;
    resp.set_key(key());
    resp.set_state(getter_());
    sink(lva::proto::kIdSwitchStateResponse, resp);
}

}  // namespace lva::entities
