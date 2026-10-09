#pragma once

namespace lva::tr {

// Receive locally observed Sendspin playback transitions from the existing
// PulseAudio monitor. This call is non-blocking.
void MusicAssistantObserveSendspinPlayback(bool active);

// Request that Music Assistant playback be paused for a voice interaction.
// The request only updates desired state and schedules background work; it
// never performs network I/O on the caller's thread.
void MusicAssistantPauseForVoiceAsync();

// Request restoration of Music Assistant playback after a voice interaction.
// HTTP, retry delays, and Sendspin confirmation waits run on the background
// worker rather than the satellite/event/wake thread.
void MusicAssistantResumeForVoiceAsync();

}  // namespace lva::tr
