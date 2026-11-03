#include "riv_store.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char* TAG = "riv_store";

namespace
{
constexpr uint32_t kMagic = 0x46564952; // "RIVF"
constexpr size_t kDataOffset = 4096;

struct Header
{
  uint32_t magic;
  uint32_t size;
  uint32_t crc;
  char name[52];
};
static_assert(sizeof(Header) == 64, "header layout");

const esp_partition_t* partition()
{
  static const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
  return p;
}
} // namespace

size_t riv_store_capacity()
{
  const esp_partition_t* p = partition();
  return p ? p->size - kDataOffset : 0;
}

bool riv_store_load(uint8_t** data, size_t* size, std::string* name)
{
  const esp_partition_t* p = partition();
  Header h;
  if (p == nullptr || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK ||
      h.magic != kMagic || h.size == 0 || h.size > riv_store_capacity())
  {
    return false;
  }
  auto* buf = static_cast<uint8_t*>(
      heap_caps_malloc(h.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr)
  {
    ESP_LOGE(TAG, "no PSRAM for the stored file (%u bytes)", unsigned(h.size));
    return false;
  }
  if (esp_partition_read(p, kDataOffset, buf, h.size) != ESP_OK ||
      esp_rom_crc32_le(0, buf, h.size) != h.crc)
  {
    ESP_LOGW(TAG, "stored file is damaged (CRC mismatch), ignoring it");
    heap_caps_free(buf);
    return false;
  }
  h.name[sizeof(h.name) - 1] = '\0';
  *data = buf;
  *size = h.size;
  *name = h.name;
  return true;
}

esp_err_t riv_store_save(const uint8_t* data, size_t size, const char* name)
{
  const esp_partition_t* p = partition();
  if (p == nullptr)
  {
    return ESP_ERR_NOT_FOUND;
  }
  if (size > riv_store_capacity())
  {
    return ESP_ERR_INVALID_SIZE;
  }
  const size_t sector = p->erase_size;
  const size_t end = (kDataOffset + size + sector - 1) / sector * sector;
  // Erasing the header first: until the new header lands, no file is valid.
  esp_err_t err = esp_partition_erase_range(p, 0, end);
  if (err == ESP_OK)
  {
    err = esp_partition_write(p, kDataOffset, data, size);
  }
  if (err == ESP_OK)
  {
    Header h = {};
    h.magic = kMagic;
    h.size = uint32_t(size);
    h.crc = esp_rom_crc32_le(0, data, size);
    std::strncpy(h.name, name ? name : "uploaded.riv", sizeof(h.name) - 1);
    err = esp_partition_write(p, 0, &h, sizeof(h));
  }
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "saving failed: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t riv_store_clear()
{
  const esp_partition_t* p = partition();
  if (p == nullptr)
  {
    return ESP_ERR_NOT_FOUND;
  }
  return esp_partition_erase_range(p, 0, p->erase_size);
}
