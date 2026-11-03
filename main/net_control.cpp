#include "net_control.h"

#include <algorithm>
#include <cstring>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char* TAG = "net";

namespace
{
struct Command
{
  std::string line;
  std::string reply;
  SemaphoreHandle_t done; // given by the render task when reply is set
};

QueueHandle_t s_queue; // Command*
esp_netif_t* s_netif;
volatile bool s_haveAddress;
// Set once the fixed address is in use: setting it also raises GOT_IP (on
// the event loop, later), which must not be reported as DHCP.
volatile bool s_usingStatic;

void logAddress(const esp_netif_ip_info_t& ip, const char* how)
{
  ESP_LOGI(TAG, "address " IPSTR " (%s), control port %d (UDP and TCP), "
           "e.g. echo \"list\" | nc " IPSTR " %d",
           IP2STR(&ip.ip), how, CONFIG_RIVE_NET_PORT, IP2STR(&ip.ip),
           CONFIG_RIVE_NET_PORT);
}

// Switches to the fixed address from menuconfig.
void useStaticAddress(const char* why)
{
  esp_netif_dhcpc_stop(s_netif);
  esp_netif_ip_info_t ip = {};
  ip.ip.addr = esp_ip4addr_aton(CONFIG_RIVE_NET_STATIC_IP);
  ip.netmask.addr = esp_ip4addr_aton(CONFIG_RIVE_NET_STATIC_NETMASK);
  ip.gw.addr = esp_ip4addr_aton(CONFIG_RIVE_NET_STATIC_GATEWAY);
  s_usingStatic = true;
  const esp_err_t err = esp_netif_set_ip_info(s_netif, &ip);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "could not set the fixed address %s",
             CONFIG_RIVE_NET_STATIC_IP);
    return;
  }
  s_haveAddress = true;
  logAddress(ip, why);
}

#if CONFIG_RIVE_NET_DHCP_FALLBACK
void dhcpTimeout(void*)
{
  if (!s_haveAddress)
  {
    useStaticAddress("fixed: no DHCP server answered");
  }
}
#endif

// Runs one command on the render task and waits for its reply.
std::string submit(const std::string& line)
{
  Command cmd{line, "", xSemaphoreCreateBinary()};
  if (cmd.done == nullptr)
  {
    return "error out of memory\n";
  }
  Command* p = &cmd;
  std::string reply;
  if (xQueueSend(s_queue, &p, pdMS_TO_TICKS(200)) != pdTRUE)
  {
    reply = "error busy\n";
  }
  else if (xSemaphoreTake(cmd.done, pdMS_TO_TICKS(2000)) != pdTRUE)
  {
    // The render task still holds a pointer to cmd: wait for it rather than
    // free the command under it.
    xSemaphoreTake(cmd.done, portMAX_DELAY);
    reply = cmd.reply;
  }
  else
  {
    reply = cmd.reply;
  }
  vSemaphoreDelete(cmd.done);
  return reply;
}

// Each line of `text` is one command; replies are concatenated.
std::string runLines(const char* text, size_t len)
{
  std::string out;
  size_t start = 0;
  for (size_t i = 0; i <= len; ++i)
  {
    if (i == len || text[i] == '\n')
    {
      std::string line(text + start, i - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      {
        line.pop_back();
      }
      if (!line.empty())
      {
        out += submit(line);
      }
      start = i + 1;
    }
  }
  return out;
}

void udpTask(void*)
{
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONFIG_RIVE_NET_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (sock < 0 || bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0)
  {
    ESP_LOGE(TAG, "UDP port %d unavailable", CONFIG_RIVE_NET_PORT);
    vTaskSuspend(nullptr); // created with caps: can't delete itself
    return;
  }
  static char buf[1472];
  for (;;)
  {
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    const int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    if (n <= 0)
    {
      continue;
    }
    const std::string reply = runLines(buf, size_t(n));
    // Large replies (list) are cut to one datagram; use TCP for those.
    sendto(sock, reply.data(), std::min<size_t>(reply.size(), 1400), 0,
           (sockaddr*)&from, fromLen);
  }
}

void tcpClient(int fd)
{
  std::string pending;
  char buf[256];
  for (;;)
  {
    const int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
    {
      break;
    }
    pending.append(buf, size_t(n));
    if (pending.size() > 4096)
    {
      break; // no newline in sight: not our protocol
    }
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos)
    {
      const std::string reply = runLines(pending.data(), nl);
      pending.erase(0, nl + 1);
      if (!reply.empty() && send(fd, reply.data(), reply.size(), 0) < 0)
      {
        return;
      }
    }
  }
}

void tcpTask(void*)
{
  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONFIG_RIVE_NET_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (sock < 0 || bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
      listen(sock, 2) != 0)
  {
    ESP_LOGE(TAG, "TCP port %d unavailable", CONFIG_RIVE_NET_PORT);
    vTaskSuspend(nullptr); // created with caps: can't delete itself
    return;
  }
  for (;;)
  {
    const int fd = accept(sock, nullptr, nullptr);
    if (fd < 0)
    {
      continue;
    }
    // One client at a time; a session is short (a script or nc).
    tcpClient(fd);
    close(fd);
  }
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data)
{
  if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED)
  {
    ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP");
  }
  else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED)
  {
    ESP_LOGW(TAG, "Ethernet link down");
  }
  else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP)
  {
    if (s_usingStatic)
    {
      return; // useStaticAddress() logs it
    }
    const auto* e = static_cast<ip_event_got_ip_t*>(data);
    s_haveAddress = true;
    logAddress(e->ip_info, "DHCP");
  }
}
} // namespace

esp_err_t net_control_start(int core)
{
  s_queue = xQueueCreate(8, sizeof(Command*));
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
  {
    return err;
  }

  // ESP32-P4 EMAC: the IDF defaults are this board's RMII wiring (MDC 31,
  // MDIO 52, 50 MHz reference clock in on GPIO 50, TXD 34/35, TX_EN 49,
  // RXD 29/30, CRS_DV 28).
  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  eth_esp32_emac_config_t emacConfig = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emacConfig, &macConfig);
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = CONFIG_RIVE_ETH_PHY_ADDR;
  phyConfig.reset_gpio_num = CONFIG_RIVE_ETH_PHY_RESET_GPIO;
  esp_eth_phy_t* phy = esp_eth_phy_new_ip101(&phyConfig);
  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth = nullptr;
  err = esp_eth_driver_install(&ethConfig, &eth);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
    return err;
  }
  esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
  s_netif = esp_netif_new(&netifConfig);
  ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)),
                      TAG, "attach");
  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onEvent, nullptr);
#if CONFIG_RIVE_NET_STATIC
  useStaticAddress("fixed");
#endif
  ESP_RETURN_ON_ERROR(esp_eth_start(eth), TAG, "start");
#if CONFIG_RIVE_NET_DHCP_FALLBACK
  // One-shot timer (runs in the esp_timer task): no task of its own.
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = dhcpTimeout;
  timerArgs.name = "net_dhcp";
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&timerArgs, &timer) == ESP_OK)
  {
    esp_timer_start_once(timer,
                         uint64_t(CONFIG_RIVE_NET_DHCP_TIMEOUT_S) * 1000000);
  }
#endif

  // Below the render tasks: they mostly wait on sockets, and a command only
  // runs when the render task picks it up.
  // Their stacks live in PSRAM (internal RAM is scarce); socket I/O runs in
  // lwIP's own task.
  constexpr UBaseType_t kStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  xTaskCreatePinnedToCoreWithCaps(udpTask, "net_udp", 6 * 1024, nullptr, 4,
                                  nullptr, core, kStackCaps);
  xTaskCreatePinnedToCoreWithCaps(tcpTask, "net_tcp", 6 * 1024, nullptr, 4,
                                  nullptr, core, kStackCaps);
#if CONFIG_RIVE_NET_STATIC
  ESP_LOGI(TAG, "Ethernet started (IP101 at address %d), fixed address",
           CONFIG_RIVE_ETH_PHY_ADDR);
#else
  ESP_LOGI(TAG, "Ethernet started (IP101 at address %d), DHCP; %s after "
           "%d s without a server", CONFIG_RIVE_ETH_PHY_ADDR,
           CONFIG_RIVE_NET_STATIC_IP, CONFIG_RIVE_NET_DHCP_TIMEOUT_S);
#endif
  return ESP_OK;
}

void net_control_service(
    const std::function<std::string(const std::string&)>& handler)
{
  if (s_queue == nullptr)
  {
    return;
  }
  Command* cmd;
  while (xQueueReceive(s_queue, &cmd, 0) == pdTRUE)
  {
    ESP_LOGI(TAG, "> %s", cmd->line.c_str());
    cmd->reply = handler(cmd->line);
    xSemaphoreGive(cmd->done);
  }
}
