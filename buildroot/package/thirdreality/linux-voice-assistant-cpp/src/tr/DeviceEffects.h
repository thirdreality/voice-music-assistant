#pragma once

#include <string>

namespace lva::tr {

// Start before any audio/LED producers. Stop after producers have stopped.
// Queue operations never spawn processes or wait for the hardware helpers.
bool StartDeviceEffects();
void StopDeviceEffects();
void QueueSystemVolume(int percent);
void QueueLedAnimation(bool to_idle, std::string array_arg,
                       bool volume_feedback = false, bool deduplicate = true);

}  // namespace lva::tr
