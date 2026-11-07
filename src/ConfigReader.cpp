/**
 * @file ConfigReader.cpp
 * @brief ConfigReader: typed field reading with the default-value policy.
 */
#include "ConfigReader.h"

#include <cstdio>
#include <cstring>

#include "esp_netif.h"

#define LOG_TAG     "Config"
#define LOG_ENABLED 1
#include "Log.h"

ConfigReader::ConfigReader(const char* file, const cJSON* obj, const ConfigSinks& sinks, std::string prefix)
    : file(file),
      obj(obj),
      sinks(sinks),
      prefix(std::move(prefix))
{
}

const cJSON* ConfigReader::field(const char* key)
{
    known.emplace_back(key);
    if (obj == nullptr) {
        return nullptr;
    }
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == nullptr) {
        LOG_I("%s: brak pola %s%s - wartosc domyslna", file, prefix.c_str(), key);
    }
    return item;
}

void ConfigReader::accepted(const char* key)
{
    if (sinks.fromFile != nullptr) {
        sinks.fromFile->push_back(prefix + key);
    }
}

void ConfigReader::rejected(const char* key, const std::string& why)
{
    LOG_W("%s: %s%s - %s, przyjeto wartosc domyslna", file, prefix.c_str(), key, i18n::ascii(why).c_str());
    if (sinks.errors != nullptr) {
        sinks.errors->push_back(prefix + key + " - " + why);
    }
}

void ConfigReader::hint(const char* key, std::string pl, std::string en)
{
    if (sinks.hints != nullptr) {
        sinks.hints->push_back({ prefix + key, std::move(pl), std::move(en) });
    }
}

std::string ConfigReader::str(const char* key, const std::string& def, size_t maxLen, bool (*valid)(const std::string&),
                              i18n::Text hintText)
{
    if (hintText) {
        hint(key, hintText.pl, hintText.en);
    } else {
        hint(key, "tekst, max " + std::to_string(maxLen), "text, max " + std::to_string(maxLen));
    }

    const cJSON* item = field(key);
    if (item == nullptr) {
        return def;
    }
    if (!cJSON_IsString(item)) {
        rejected(key, tr("oczekiwano tekstu", "text expected"));
        return def;
    }
    std::string v = item->valuestring;
    if (v.size() > maxLen) {
        rejected(key, tr("za długie (max ", "too long (max ") + std::to_string(maxLen) + ")");
        return def;
    }
    if (valid != nullptr && !valid(v)) {
        rejected(key, hintText ? std::string(tr("nieprawidłowa wartość, dozwolone: ", "invalid value, allowed: ")) +
                                     hintText.get()
                               : tr("nieprawidłowa wartość", "invalid value"));
        return def;
    }
    accepted(key);
    return v;
}

long ConfigReader::integer(const char* key, long def, long min, long max)
{
    char range[32];
    snprintf(range, sizeof(range), "%ld-%ld", min, max);
    hint(key, range, range);

    const cJSON* item = field(key);
    if (item == nullptr) {
        return def;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble != static_cast<double>(static_cast<long>(item->valuedouble))) {
        rejected(key, tr("oczekiwano liczby całkowitej", "integer expected"));
        return def;
    }
    long v = static_cast<long>(item->valuedouble);
    if (v < min || v > max) {
        rejected(key, std::string(tr("wartość poza zakresem (", "value out of range (")) + range + ")");
        return def;
    }
    accepted(key);
    return v;
}

bool ConfigReader::boolean(const char* key, bool def)
{
    hint(key, "true/false", "true/false");
    const cJSON* item = field(key);
    if (item == nullptr) {
        return def;
    }
    if (!cJSON_IsBool(item)) {
        rejected(key, tr("oczekiwano true/false", "true/false expected"));
        return def;
    }
    accepted(key);
    return cJSON_IsTrue(item);
}

uint32_t ConfigReader::ipv4(const char* key, uint32_t def)
{
    hint(key, "adres IPv4", "IPv4 address");
    const cJSON* item = field(key);
    if (item == nullptr) {
        return def;
    }
    esp_ip4_addr_t ip = {};
    if (!cJSON_IsString(item) || esp_netif_str_to_ip4(item->valuestring, &ip) != ESP_OK) {
        rejected(key, tr("oczekiwano adresu IPv4", "IPv4 address expected"));
        return def;
    }
    accepted(key);
    return ip.addr;
}

ConfigReader ConfigReader::object(const char* key)
{
    const cJSON* item = field(key);
    if (item != nullptr && !cJSON_IsObject(item)) {
        rejected(key, tr("oczekiwano obiektu", "object expected"));
        item = nullptr;
    }
    // No object -> nested fields get default values without separate logs.
    return ConfigReader(file, item, sinks, prefix + key + ".");
}

const cJSON* ConfigReader::list(const char* key)
{
    hint(key, "lista – zmiana poleceniami modułu", "list – change with module commands");
    const cJSON* item = field(key);
    if (item == nullptr) {
        return nullptr;
    }
    if (!cJSON_IsArray(item)) {
        rejected(key, tr("oczekiwano listy", "list expected"));
        return nullptr;
    }
    accepted(key);
    return item;
}

void ConfigReader::rejectEntry(const char* key, int index, const char* why)
{
    LOG_W("%s: %s%s[%d] - %s, wpis pominiety (zniknie przy save)", file, prefix.c_str(), key, index,
          i18n::ascii(why).c_str());
    if (sinks.errors != nullptr) {
        sinks.errors->push_back(prefix + key + "[" + std::to_string(index) + "] - " + why);
    }
}

void ConfigReader::finish()
{
    if (obj == nullptr) {
        return;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, obj)
    {
        bool isKnown = false;
        for (const std::string& k : known) {
            if (k == item->string) {
                isKnown = true;
                break;
            }
        }
        if (!isKnown) {
            LOG_W("%s: nieznane pole %s%s - pominiete (zniknie przy save)", file, prefix.c_str(), item->string);
        }
    }
}
