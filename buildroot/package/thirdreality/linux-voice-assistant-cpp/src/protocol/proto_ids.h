
#pragma once

#include <cstdint>

namespace lva::proto {

inline constexpr std::uint32_t kIdHelloRequest                          = 1;
inline constexpr std::uint32_t kIdHelloResponse                         = 2;
inline constexpr std::uint32_t kIdAuthenticationRequest                 = 3;
inline constexpr std::uint32_t kIdAuthenticationResponse                = 4;
inline constexpr std::uint32_t kIdDisconnectRequest                     = 5;
inline constexpr std::uint32_t kIdDisconnectResponse                    = 6;
inline constexpr std::uint32_t kIdPingRequest                           = 7;
inline constexpr std::uint32_t kIdPingResponse                          = 8;
inline constexpr std::uint32_t kIdDeviceInfoRequest                     = 9;
inline constexpr std::uint32_t kIdDeviceInfoResponse                    = 10;

inline constexpr std::uint32_t kIdListEntitiesRequest                   = 11;
inline constexpr std::uint32_t kIdListEntitiesDoneResponse              = 19;
inline constexpr std::uint32_t kIdSubscribeStatesRequest                = 20;
inline constexpr std::uint32_t kIdSubscribeHomeAssistantStatesRequest   = 38;

inline constexpr std::uint32_t kIdListEntitiesSwitchResponse            = 17;
inline constexpr std::uint32_t kIdSwitchStateResponse                   = 26;
inline constexpr std::uint32_t kIdSwitchCommandRequest                  = 33;

inline constexpr std::uint32_t kIdListEntitiesButtonResponse            = 61;
inline constexpr std::uint32_t kIdButtonCommandRequest                  = 62;

inline constexpr std::uint32_t kIdListEntitiesNumberResponse            = 49;
inline constexpr std::uint32_t kIdNumberStateResponse                   = 50;
inline constexpr std::uint32_t kIdNumberCommandRequest                  = 51;

inline constexpr std::uint32_t kIdListEntitiesSelectResponse            = 52;
inline constexpr std::uint32_t kIdSelectStateResponse                   = 53;
inline constexpr std::uint32_t kIdSelectCommandRequest                  = 54;

inline constexpr std::uint32_t kIdListEntitiesMediaPlayerResponse       = 63;
inline constexpr std::uint32_t kIdMediaPlayerStateResponse              = 64;
inline constexpr std::uint32_t kIdMediaPlayerCommandRequest             = 65;

inline constexpr std::uint32_t kIdListEntitiesEventResponse             = 107;
inline constexpr std::uint32_t kIdEventResponse                         = 108;

inline constexpr std::uint32_t kIdListEntitiesUpdateResponse            = 116;
inline constexpr std::uint32_t kIdUpdateStateResponse                   = 117;
inline constexpr std::uint32_t kIdUpdateCommandRequest                  = 118;

inline constexpr std::uint32_t kIdSubscribeHomeassistantServicesRequest = 34;
inline constexpr std::uint32_t kIdHomeassistantActionRequest            = 35;
inline constexpr std::uint32_t kIdHomeassistantActionResponse           = 130;

inline constexpr std::uint32_t kIdGetTimeRequest                        = 36;
inline constexpr std::uint32_t kIdGetTimeResponse                       = 37;

inline constexpr std::uint32_t kIdSubscribeVoiceAssistantRequest         = 89;
inline constexpr std::uint32_t kIdVoiceAssistantRequest                 = 90;
inline constexpr std::uint32_t kIdVoiceAssistantResponse                = 91;
inline constexpr std::uint32_t kIdVoiceAssistantEventResponse           = 92;
inline constexpr std::uint32_t kIdVoiceAssistantAudio                   = 106;
inline constexpr std::uint32_t kIdVoiceAssistantTimerEventResponse      = 115;
inline constexpr std::uint32_t kIdVoiceAssistantAnnounceRequest         = 119;
inline constexpr std::uint32_t kIdVoiceAssistantAnnounceFinished        = 120;
inline constexpr std::uint32_t kIdVoiceAssistantConfigurationRequest    = 121;
inline constexpr std::uint32_t kIdVoiceAssistantConfigurationResponse   = 122;
inline constexpr std::uint32_t kIdVoiceAssistantSetConfiguration        = 123;

}  // namespace lva::proto
