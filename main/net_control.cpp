#include "net_control.h"

#include <algorithm>
#include <cstring>

#include "esp_eth.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
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

#include "riv_store.h"

static const char* TAG = "net";

namespace
{
struct Command
{
  std::string line;
  std::string reply;
  SemaphoreHandle_t done; // given by the render task when reply is set
  std::function<std::string()> job; // instead of `line`: run this
};

RivLoader s_loader;

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
#if CONFIG_RIVE_NET_HTTP
  ESP_LOGI(TAG, "web page (upload a .riv, send commands): http://" IPSTR
           "%s", IP2STR(&ip.ip),
           CONFIG_RIVE_NET_HTTP_PORT == 80
               ? ""
               : (":" + std::to_string(CONFIG_RIVE_NET_HTTP_PORT)).c_str());
#endif
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

// Runs one command (or job) on the render task and waits for its reply.
std::string submit(const std::string& line,
                   std::function<std::string()> job = nullptr,
                   TickType_t wait = pdMS_TO_TICKS(2000))
{
  Command cmd{line, "", xSemaphoreCreateBinary(), std::move(job)};
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
  else if (xSemaphoreTake(cmd.done, wait) != pdTRUE)
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


#if CONFIG_RIVE_NET_HTTP
// ---- Web page and upload (HTTP) -------------------------------------------

const char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Rive player</title>
<style>
body{font:15px system-ui,sans-serif;margin:0;background:#16181b;color:#e8eaed}
main{max-width:720px;margin:0 auto;padding:20px 16px;display:grid;gap:18px}
h1{font-size:20px;margin:0}h2{font-size:15px;margin:0 0 8px;color:#9aa0a6}
section{background:#1f2226;border:1px solid #33373c;border-radius:8px;padding:14px}
button,input,textarea{font:inherit;color:inherit;background:#16181b;border:1px solid #3a3f45;border-radius:6px;padding:7px 10px}
button{cursor:pointer}button:hover{border-color:#4fb3ff}
textarea{width:100%;box-sizing:border-box;min-height:5em}
pre{white-space:pre-wrap;margin:8px 0 0;font:13px ui-monospace,Consolas,monospace;color:#c5c9ce}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
</style></head><body><main>
<h1>Rive player</h1>
<section><h2>Playing</h2><pre id="info">…</pre></section>
<section><h2>Upload a .riv</h2>
<div class="row"><input type="file" id="file" accept=".riv"><button id="up">Upload and play</button>
<button id="clear">Forget uploaded file</button></div>
<pre id="upmsg"></pre></section>
<section><h2>Commands</h2>
<div class="row"><button data-c="list">list</button><button data-c="info">info</button><button data-c="help">help</button></div>
<p><textarea id="cmd" placeholder="set formula_input 5"></textarea></p>
<button id="send">Send</button><pre id="reply"></pre></section>
</main><script>
const $=id=>document.getElementById(id);
async function cmd(t){const r=await fetch('/cmd',{method:'POST',body:t});return r.text();}
async function refresh(){$('info').textContent=await cmd('info');}
$('send').onclick=async()=>{$('reply').textContent=await cmd($('cmd').value);refresh();};
document.querySelectorAll('[data-c]').forEach(b=>b.onclick=async()=>{$('reply').textContent=await cmd(b.dataset.c);});
$('up').onclick=async()=>{const f=$('file').files[0];if(!f)return;
 $('upmsg').textContent='Uploading '+f.name+' ('+f.size+' bytes)…';
 const r=await fetch('/riv?name='+encodeURIComponent(f.name),{method:'POST',body:f});
 $('upmsg').textContent=await r.text();refresh();};
$('clear').onclick=async()=>{const r=await fetch('/riv',{method:'DELETE'});$('upmsg').textContent=await r.text();};
refresh();
</script></body></html>)HTML";

esp_err_t pageGet(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

// Reads the request body (up to `limit` bytes) into a new PSRAM buffer.
uint8_t* readBody(httpd_req_t* req, size_t limit, const char** error)
{
  const size_t size = req->content_len;
  if (size == 0)
  {
    *error = "empty request";
    return nullptr;
  }
  if (size > limit)
  {
    *error = "too large";
    return nullptr;
  }
  auto* buf = static_cast<uint8_t*>(
      heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr)
  {
    *error = "out of memory";
    return nullptr;
  }
  size_t got = 0;
  while (got < size)
  {
    const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf) + got,
                                 size - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT)
    {
      continue;
    }
    if (n <= 0)
    {
      heap_caps_free(buf);
      *error = "connection lost";
      return nullptr;
    }
    got += size_t(n);
  }
  buf[size] = 0;
  return buf;
}

esp_err_t sendText(httpd_req_t* req, const char* status, const std::string& text)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, text.data(), text.size());
}

esp_err_t cmdPost(httpd_req_t* req)
{
  const char* error = nullptr;
  uint8_t* body = readBody(req, 4096, &error);
  if (body == nullptr)
  {
    return sendText(req, "400 Bad Request", std::string("error ") + error + "\n");
  }
  const std::string reply =
      runLines(reinterpret_cast<const char*>(body), req->content_len);
  heap_caps_free(body);
  return sendText(req, "200 OK", reply);
}

std::string uploadName(httpd_req_t* req)
{
  char query[160] = {};
  char name[64] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
  {
    httpd_query_key_value(query, "name", name, sizeof(name));
  }
  // Keep it printable: the browser sends it URL-encoded; spaces etc. stay
  // as %xx, which is fine for a label.
  std::string n = name[0] ? name : "uploaded.riv";
  for (char& c : n)
  {
    if (c < 32 || c > 126)
    {
      c = '_';
    }
  }
  return n;
}

esp_err_t rivPost(httpd_req_t* req)
{
  const std::string name = uploadName(req);
  const char* error = nullptr;
  const int64_t t0 = esp_timer_get_time();
  uint8_t* data = readBody(req, riv_store_capacity(), &error);
  if (data == nullptr)
  {
    return sendText(req, "400 Bad Request",
                    std::string("error ") + error + " (limit " +
                        std::to_string(riv_store_capacity()) + " bytes)\n");
  }
  const size_t size = req->content_len;
  if (size < 4 || std::memcmp(data, "RIVE", 4) != 0)
  {
    heap_caps_free(data);
    return sendText(req, "400 Bad Request", "error not a .riv file\n");
  }
  const int64_t t1 = esp_timer_get_time();
  ESP_LOGI(TAG, "upload: %s, %u bytes in %.1f s", name.c_str(), unsigned(size),
           double(t1 - t0) / 1e6);
  if (!s_loader)
  {
    heap_caps_free(data);
    return sendText(req, "503 Service Unavailable", "error player not ready\n");
  }
  // Keep a copy to save: the loader owns `data` from here on, and the
  // player may free it if a later upload replaces it.
  auto* copy = static_cast<uint8_t*>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy)
  {
    std::memcpy(copy, data, size);
  }
  // Loading runs on the render task between frames; big files take a
  // moment, so wait longer than for a command.
  const std::string result = submit(
      "(upload " + name + ")",
      [data, size, name] { return s_loader(data, size, name); },
      pdMS_TO_TICKS(20000));
  if (!result.empty())
  {
    heap_caps_free(copy);
    return sendText(req, "422 Unprocessable Entity",
                    "error " + result + " (still playing the previous file)\n");
  }
  std::string reply = "ok playing " + name + " (" + std::to_string(size) +
                      " bytes)";
  if (copy)
  {
    const int64_t t2 = esp_timer_get_time();
    const esp_err_t err = riv_store_save(copy, size, name.c_str());
    heap_caps_free(copy);
    reply += err == ESP_OK
                 ? ", saved to flash for the next boot (" +
                       std::to_string((esp_timer_get_time() - t2) / 1000) +
                       " ms)"
                 : std::string(", NOT saved: ") + esp_err_to_name(err);
  }
  else
  {
    reply += ", not saved (out of memory for the copy)";
  }
  ESP_LOGI(TAG, "%s", reply.c_str());
  return sendText(req, "200 OK", reply + "\n");
}

esp_err_t rivDelete(httpd_req_t* req)
{
  const esp_err_t err = riv_store_clear();
  return sendText(req, err == ESP_OK ? "200 OK" : "500 Internal Server Error",
                  err == ESP_OK ? "ok the uploaded file is forgotten; the "
                                  "embedded one plays after the next reset\n"
                                : std::string("error ") + esp_err_to_name(err) +
                                      "\n");
}

void startHttp()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = CONFIG_RIVE_NET_HTTP_PORT;
  config.ctrl_port = 32769;
  config.stack_size = 8192;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK)
  {
    ESP_LOGE(TAG, "HTTP server could not start on port %d",
             CONFIG_RIVE_NET_HTTP_PORT);
    return;
  }
  static const httpd_uri_t uris[] = {
      {"/", HTTP_GET, pageGet, nullptr},
      {"/cmd", HTTP_POST, cmdPost, nullptr},
      {"/riv", HTTP_POST, rivPost, nullptr},
      {"/riv", HTTP_DELETE, rivDelete, nullptr},
  };
  for (const auto& u : uris)
  {
    httpd_register_uri_handler(server, &u);
  }
}
#endif

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
#if CONFIG_RIVE_NET_HTTP
  startHttp();
#endif
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
    cmd->reply = cmd->job ? cmd->job() : handler(cmd->line);
    xSemaphoreGive(cmd->done);
  }
}

void net_control_set_loader(RivLoader loader) { s_loader = std::move(loader); }
