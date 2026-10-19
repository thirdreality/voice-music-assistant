#pragma once

namespace lva::tr {

// Fire-and-forget datagrams to sendspin-client on
// /tmp/sendspin-control.sock; dropped while it is not running.

void SendspinMusicToggle();
void SendspinPause();

void SendspinLedToggle();

void SendspinDuck();
void SendspinUnduck();

}  // namespace lva::tr
