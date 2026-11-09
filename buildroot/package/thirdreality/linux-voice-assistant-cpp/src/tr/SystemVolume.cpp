#include "tr/SystemVolume.h"

#include "tr/DeviceEffects.h"

namespace lva::tr {

void SetSystemVolume(int percent) {
    QueueSystemVolume(percent);
}

void SetSystemVolumeSilent(int percent) {
    QueueSystemVolume(percent);
}

}  // namespace lva::tr
