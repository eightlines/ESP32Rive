// Network control: brings up the board's Ethernet (ESP32-P4 EMAC + IP101GRI
// PHY, DHCP) and accepts text commands on a port, UDP and TCP (one command
// per line, e.g. "set score 42"; see RivePlayer::command). Commands are
// queued and run on the render task between frames, so they never race
// with Rive.
#pragma once

#include <functional>
#include <string>

#include "esp_err.h"

// Starts Ethernet and the UDP/TCP listeners (non-blocking: the address is
// logged when DHCP completes).
esp_err_t net_control_start(int core);

// Runs the queued commands through `handler` and sends back its replies.
// Call from the render loop, once per frame.
void net_control_service(const std::function<std::string(const std::string&)>& handler);
