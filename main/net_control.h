// Network control: brings up the board's Ethernet (ESP32-P4 EMAC + IP101GRI
// PHY, DHCP) and accepts text commands on a port, UDP and TCP (one command
// per line, e.g. "set score 42"; see RivePlayer::command), plus a small web
// page (HTTP, port 80) to upload a .riv and send commands. Commands and
// uploads are queued and run on the render task between frames, so they
// never race with Rive.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"

// Starts Ethernet and the UDP/TCP listeners (non-blocking: the address is
// logged when DHCP completes).
esp_err_t net_control_start(int core);

// Runs the queued commands through `handler` and sends back its replies.
// Call from the render loop, once per frame.
void net_control_service(const std::function<std::string(const std::string&)>& handler);

// Uploaded .riv files (HTTP: POST /riv, see README) are handed to `loader`
// on the render task, between frames. It takes ownership of `data` (a PSRAM
// buffer, heap_caps_free) and returns "" on success or the reason it
// failed; on success the file is also saved to flash for the next boot.
using RivLoader =
    std::function<std::string(uint8_t* data, size_t size, const std::string& name)>;
void net_control_set_loader(RivLoader loader);
