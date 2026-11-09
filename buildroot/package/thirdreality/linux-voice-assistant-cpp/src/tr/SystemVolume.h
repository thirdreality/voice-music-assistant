
#pragma once

namespace lva::tr {

// Queue the latest logical percent for the background hardware worker.
void SetSystemVolume(int percent);

void SetSystemVolumeSilent(int percent);

}  // namespace lva::tr
