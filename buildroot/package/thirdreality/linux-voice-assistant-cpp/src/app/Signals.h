#pragma once
namespace lva::proto {
class ApiServer;
}
namespace lva::audio {
class WakeWordEngine;
}
namespace lva::app {
void InstallSignalHandlers();
void SetSignalServer(lva::proto::ApiServer* server);
void SetSignalWakeEngine(lva::audio::WakeWordEngine* engine);
int ShutdownSignal();
}  // namespace lva::app
