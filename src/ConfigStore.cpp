/**
 * @file ConfigStore.cpp
 * @brief ConfigStore: configuration files on LittleFS, config/save/lang commands, web panel API.
 */
#include "ConfigStore.h"

#include "i18n/I18n.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "StringOutput.h"

static bool isSecretPath(const std::string& path);

#define LOG_TAG     "ConfigStore"
#define LOG_ENABLED 1
#include "Log.h"

static constexpr const char* PARTITION_LABEL = "littlefs";

bool ConfigStore::mount()
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = MOUNT_POINT;
    conf.partition_label = PARTITION_LABEL;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        LOG_E("Nie udalo sie zamontowac LittleFS: %s", esp_err_to_name(err));
        return false;
    }

    struct stat st;
    if (stat(CONFIG_DIR, &st) != 0 && mkdir(CONFIG_DIR, 0775) != 0) {
        LOG_E("Nie udalo sie utworzyc %s: errno %d", CONFIG_DIR, errno);
        return false;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(PARTITION_LABEL, &total, &used);
    LOG_I("LittleFS zamontowany w %s, zajete %u / %u B", MOUNT_POINT, (unsigned)used, (unsigned)total);
    isMounted = true;
    return true;
}

void ConfigStore::makePath(char* buf, size_t size, const char* name, const char* suffix)
{
    snprintf(buf, size, "%s/%s%s", CONFIG_DIR, name, suffix);
}

char* ConfigStore::readFile(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (size >= 0) ? static_cast<char*>(malloc(size + 1)) : nullptr;
    if (buf != nullptr) {
        *len = fread(buf, 1, size, f);
        buf[*len] = '\0';
    }
    fclose(f);
    return buf;
}

ConfigStore::LoadResult ConfigStore::load(const char* name, cJSON** out)
{
    *out = nullptr;
    if (!isMounted) {
        return LoadResult::ERROR;
    }

    std::lock_guard<std::mutex> lock(mutex);
    char path[64];
    makePath(path, sizeof(path), name);

    struct stat st;
    if (stat(path, &st) != 0) {
        return LoadResult::MISSING;
    }

    size_t len = 0;
    char* text = readFile(path, &len);
    if (text == nullptr) {
        LOG_E("%s: blad odczytu", name);
        return LoadResult::ERROR;
    }

    cJSON* json = cJSON_ParseWithLength(text, len);
    free(text);
    if (json == nullptr || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        char bad[72];
        makePath(bad, sizeof(bad), name, ".bad");
        unlink(bad);
        rename(path, bad);
        LOG_E("%s: uszkodzony JSON, przeniesiono do %s.bad", name, name);
        return LoadResult::CORRUPT;
    }

    *out = json;
    return LoadResult::OK;
}

bool ConfigStore::remove(const char* name)
{
    if (!isMounted) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    char path[64];
    makePath(path, sizeof(path), name);
    struct stat st;
    return stat(path, &st) != 0 || unlink(path) == 0;
}

char* ConfigStore::readText(const char* name, size_t* len)
{
    if (!isMounted) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    char path[64];
    makePath(path, sizeof(path), name);
    return readFile(path, len);
}

bool ConfigStore::save(const char* name, const cJSON* json)
{
    char* text = cJSON_Print(json);
    if (text == nullptr) {
        LOG_E("%s: brak pamieci na serializacje", name);
        return false;
    }
    bool ok = saveText(name, text, strlen(text));
    free(text);
    return ok;
}

// Atomic write: <name>.tmp, fsync, then rename.
bool ConfigStore::saveText(const char* name, const char* text, size_t len)
{
    if (!isMounted) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    char path[64];
    char tmp[72];
    makePath(path, sizeof(path), name);
    makePath(tmp, sizeof(tmp), name, ".tmp");

    bool ok = false;
    FILE* f = fopen(tmp, "wb");
    if (f != nullptr) {
        ok = fwrite(text, 1, len, f) == len;
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        fclose(f);
    }

    if (ok && rename(tmp, path) != 0) {
        // In case rename does not overwrite an existing file.
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
        LOG_E("%s: blad zapisu", name);
        return false;
    }

    LOG_I("%s zapisany", name);
    return true;
}

bool ConfigStore::factoryReset()
{
    if (!isMounted) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    DIR* dir = opendir(CONFIG_DIR);
    if (dir == nullptr) {
        return false;
    }
    bool ok = true;
    char path[300];
    while (dirent* e = readdir(dir)) {
        snprintf(path, sizeof(path), "%s/%s", CONFIG_DIR, e->d_name);
        if (unlink(path) != 0) {
            LOG_E("Nie udalo sie usunac %s", path);
            ok = false;
        }
    }
    closedir(dir);
    LOG_W("Reset fabryczny: usunieto pliki konfiguracji");
    return ok;
}

void ConfigStore::add(IConfigurable& module)
{
    entries.push_back({ &module, {}, nullptr });
}

void ConfigStore::loadEntry(Entry& e)
{
    char file[32];
    snprintf(file, sizeof(file), "%s.json", e.module->configName());

    cJSON* json = nullptr;
    LoadResult res = load(file, &json);
    switch (res) {
    case LoadResult::OK:
        LOG_I("%s wczytany", file);
        break;
    case LoadResult::MISSING:
        LOG_W("Brak %s - wartosci domyslne", file);
        break;
    case LoadResult::CORRUPT:
    case LoadResult::ERROR:
        LOG_E("%s nie zostal wczytany - wartosci domyslne", file);
        break;
    }
    e.fileExists = res == LoadResult::OK;

    e.fromFile.clear();
    e.hints.clear();
    ConfigSinks sinks;
    sinks.fromFile = &e.fromFile;
    sinks.hints = &e.hints;
    ConfigReader r(file, json, sinks);
    e.module->loadConfig(r);
    r.finish();
    cJSON_Delete(json);

    cJSON_Delete(e.snapshot);
    e.snapshot = e.module->saveConfig();
}

void ConfigStore::loadAll()
{
    for (Entry& e : entries) {
        loadEntry(e);
    }
}

// Saves a module only when its configuration differs from the file (or there is no file yet).
ConfigStore::SaveResult ConfigStore::saveEntry(Entry& e)
{
    char file[32];
    snprintf(file, sizeof(file), "%s.json", e.module->configName());

    bool extraOk = e.module->saveExtra(); // e.g. the DNS block list (checks for changes itself)
    cJSON* json = e.module->saveConfig();
    if (json == nullptr) {
        return SaveResult::FAILED;
    }
    if (e.fileExists && e.snapshot != nullptr && cJSON_Compare(json, e.snapshot, true)) {
        cJSON_Delete(json);
        return extraOk ? SaveResult::UNCHANGED : SaveResult::FAILED;
    }
    if (!save(file, json)) {
        cJSON_Delete(json);
        return SaveResult::FAILED;
    }
    cJSON_Delete(e.snapshot);
    e.snapshot = json;
    e.fileExists = true;
    e.fromFile.clear();
    collectPaths(json, "", e.fromFile);
    return extraOk ? SaveResult::WRITTEN : SaveResult::FAILED;
}

bool ConfigStore::saveAll(IOutput* out)
{
    bool ok = true;
    int unchanged = 0;
    std::string written;
    for (Entry& e : entries) {
        switch (saveEntry(e)) {
        case SaveResult::WRITTEN:
            written += written.empty() ? "" : ", ";
            written += std::string(e.module->configName()) + ".json";
            break;
        case SaveResult::UNCHANGED:
            ++unchanged;
            break;
        case SaveResult::FAILED:
            ok = false;
            break;
        }
    }
    if (out != nullptr) {
        out->printf(tr("Zapisano: %s; bez zmian: %d\r\n", "Saved: %s; unchanged: %d\r\n"), written.empty() ? tr("nic", "nothing") : written.c_str(), unchanged);
    }
    return ok;
}

// Paths of fields that differ between the current configuration and the file state (lists as a whole).
void ConfigStore::collectDiff(const cJSON* cur, const cJSON* snap, const std::string& prefix,
                              std::vector<std::string>& paths)
{
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cur)
    {
        const cJSON* s = snap ? cJSON_GetObjectItemCaseSensitive(snap, item->string) : nullptr;
        std::string path = prefix + item->string;
        if (cJSON_IsObject(item)) {
            collectDiff(item, cJSON_IsObject(s) ? s : nullptr, path + ".", paths);
        } else if (s == nullptr || !cJSON_Compare(item, s, true)) {
            paths.push_back(path);
        }
    }
}

void ConfigStore::collectPaths(const cJSON* obj, const std::string& prefix, std::vector<std::string>& paths)
{
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, obj)
    {
        if (cJSON_IsObject(item)) {
            collectPaths(item, prefix + item->string + ".", paths);
        } else {
            paths.push_back(prefix + item->string);
        }
    }
}

void ConfigStore::show(const Entry& e, IOutput& out) const
{
    out.printf("[%s.json]\r\n", e.module->configName());
    cJSON* cur = e.module->saveConfig();
    showObject(cur, e.snapshot, "", e, out);
    cJSON_Delete(cur);
}

const char* ConfigStore::sourceOf(const Entry& e, const std::string& path, const cJSON* item, const cJSON* snapItem)
{
    if (snapItem == nullptr || !cJSON_Compare(item, snapItem, true)) {
        return "zmienione, niezapisane";
    }
    if (std::find(e.fromFile.begin(), e.fromFile.end(), path) != e.fromFile.end()) {
        return "plik";
    }
    return "domyslne";
}

const char* ConfigStore::hintOf(const Entry& e, const std::string& path)
{
    for (const auto& h : e.hints) {
        if (h.path == path) {
            return h.get().c_str();
        }
    }
    return "";
}

// Value source in the console in the current language (the API keeps the code: plik / domyslne / zmienione, niezapisane).
static const char* sourceText(const char* source)
{
    if (strcmp(source, "plik") == 0) {
        return tr("plik", "file");
    }
    if (strcmp(source, "domyslne") == 0) {
        return tr("domyślne", "default");
    }
    return tr("zmienione, niezapisane", "changed, unsaved");
}

void ConfigStore::printField(const char* path, const cJSON* item, const char* source, const char* hint, IOutput& out)
{
    bool secret = (strcmp(item->string, "pass") == 0 || strcmp(item->string, "secret") == 0) && cJSON_IsString(item);
    char* value = secret ? nullptr : cJSON_PrintUnformatted(item);
    const char* shown = secret ? (item->valuestring[0] ? "\"********\"" : "\"\"") : (value ? value : "?");
    out.printf("  %-20s = %-24s [%s]", path, shown, sourceText(source));
    if (hint != nullptr && hint[0] != '\0') {
        out.printf("  (%s)", hint);
    }
    out.print("\r\n");
    cJSON_free(value);
}

// Every field marked: file / default / changed (not saved in the file), with the value range.
void ConfigStore::showObject(const cJSON* cur, const cJSON* snap, const std::string& prefix, const Entry& e, IOutput& out)
{
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cur)
    {
        const cJSON* snapItem = snap ? cJSON_GetObjectItemCaseSensitive(snap, item->string) : nullptr;
        std::string path = prefix + item->string;
        if (cJSON_IsObject(item)) {
            showObject(item, snapItem, path + ".", e, out);
            continue;
        }
        printField(path.c_str(), item, sourceOf(e, path, item, snapItem), hintOf(e, path), out);
    }
}

// Module by name or unambiguous prefix (exact name wins).
ConfigStore::Entry* ConfigStore::findModule(const char* token, IOutput& out)
{
    Entry* match = nullptr;
    int count = 0;
    for (Entry& e : entries) {
        const char* name = e.module->configName();
        if (strcmp(name, token) == 0) {
            return &e;
        }
        if (strncmp(name, token, strlen(token)) == 0) {
            match = &e;
            ++count;
        }
    }
    if (count == 1) {
        return match;
    }
    out.printf(count == 0 ? tr("Nieznany moduł '%s'. Dostępne:", "Unknown module '%s'. Available:")
                          : tr("Niejednoznaczny moduł '%s'. Dostępne:", "Ambiguous module '%s'. Available:"),
               token);
    for (const Entry& e : entries) {
        out.printf(" %s", e.module->configName());
    }
    out.print("\r\n");
    return nullptr;
}

// Field by dotted path; each segment may be an unambiguous prefix.
cJSON* ConfigStore::findField(cJSON* root, const char* token, std::string& path, cJSON** parent, IOutput& out)
{
    char buf[64];
    strlcpy(buf, token, sizeof(buf));
    cJSON* node = root;
    path.clear();

    char* save = nullptr;
    for (char* part = strtok_r(buf, ".", &save); part != nullptr; part = strtok_r(nullptr, ".", &save)) {
        if (!cJSON_IsObject(node)) {
            node = nullptr;
            break;
        }
        cJSON* match = nullptr;
        int count = 0;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, node)
        {
            if (strcmp(item->string, part) == 0) {
                match = item;
                count = 1;
                break;
            }
            if (strncmp(item->string, part, strlen(part)) == 0) {
                match = item;
                ++count;
            }
        }
        if (count != 1) {
            node = nullptr;
            break;
        }
        *parent = node;
        node = match;
        path += path.empty() ? "" : ".";
        path += node->string;
    }

    if (node == nullptr || cJSON_IsObject(node)) {
        std::vector<std::string> paths;
        collectPaths(root, "", paths);
        out.printf(tr("Nieznane lub niejednoznaczne pole '%s'. Pola modułu:\r\n",
                      "Unknown or ambiguous field '%s'. Module fields:\r\n"),
                   token);
        for (const std::string& p : paths) {
            out.printf("  %s\r\n", p.c_str());
        }
        return nullptr;
    }
    return node;
}

// value == nullptr -> restore the default value (remove the field and reload).
CmdResult ConfigStore::setField(Entry& e, const char* fieldToken, const char* value, IOutput& out, bool* liveOut)
{
    cJSON* before = e.module->saveConfig();
    cJSON* cur = cJSON_Duplicate(before, true);
    std::string path;
    cJSON* parent = nullptr;
    cJSON* item = findField(cur, fieldToken, path, &parent, out);
    if (item == nullptr) {
        cJSON_Delete(cur);
        cJSON_Delete(before);
        return CmdResult::ERR_FAIL;
    }

    std::string key = item->string;
    if (value == nullptr) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key.c_str());
    } else {
        cJSON* newItem = nullptr;
        if (cJSON_IsBool(item)) {
            if (strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
                newItem = cJSON_CreateTrue();
            } else if (strcmp(value, "false") == 0 || strcmp(value, "off") == 0) {
                newItem = cJSON_CreateFalse();
            }
        } else if (cJSON_IsNumber(item)) {
            char* end = nullptr;
            long n = strtol(value, &end, 10);
            if (end != value && *end == '\0') {
                newItem = cJSON_CreateNumber(n);
            }
        } else if (cJSON_IsString(item)) {
            newItem = cJSON_CreateString(value);
        } else {
            out.print(tr("To pole jest listą – zmieniaj je poleceniami modułu\r\n",
                         "This field is a list – change it with module commands\r\n"));
            cJSON_Delete(cur);
            cJSON_Delete(before);
            return CmdResult::ERR_FAIL;
        }
        if (newItem == nullptr) {
            out.printf(tr("Błąd: %s – oczekiwano %s\r\n", "Error: %s – expected %s\r\n"), path.c_str(),
                       cJSON_IsBool(item) ? "true/false" : tr("liczby całkowitej", "an integer"));
            cJSON_Delete(cur);
            cJSON_Delete(before);
            return CmdResult::ERR_FAIL;
        }
        cJSON_ReplaceItemInObjectCaseSensitive(parent, key.c_str(), newItem);
    }

    // Validation with the same rules as when loading the file; on error back to the state before the change.
    char file[32];
    snprintf(file, sizeof(file), "%s.json", e.module->configName());
    std::vector<std::string> errors;
    ConfigSinks sinks;
    sinks.errors = &errors;
    ConfigReader r(file, cur, sinks);
    e.module->loadConfig(r);
    if (!errors.empty()) {
        ConfigReader revert(file, before, ConfigSinks());
        e.module->loadConfig(revert);
        for (const std::string& err : errors) {
            out.printf(tr("Błąd: %s\r\n", "Error: %s\r\n"), err.c_str());
        }
        cJSON_Delete(cur);
        cJSON_Delete(before);
        return CmdResult::ERR_FAIL;
    }
    cJSON_Delete(cur);
    cJSON_Delete(before);

    bool live = e.module->applyLive(path);
    if (liveOut != nullptr) {
        *liveOut = live;
    }

    // Value after the change (for unset: the default).
    cJSON* now = e.module->saveConfig();
    std::string p2;
    cJSON* parent2 = nullptr;
    cJSON* nowItem = findField(now, path.c_str(), p2, &parent2, out);
    if (nowItem != nullptr) {
        const cJSON* snapParent = e.snapshot;
        // Find the value from the last save to mark the source.
        char buf[64];
        strlcpy(buf, path.c_str(), sizeof(buf));
        const cJSON* snapItem = snapParent;
        char* save = nullptr;
        for (char* part = strtok_r(buf, ".", &save); part != nullptr && snapItem != nullptr;
             part = strtok_r(nullptr, ".", &save)) {
            snapItem = cJSON_GetObjectItemCaseSensitive(snapItem, part);
        }
        printField(path.c_str(), nowItem, sourceOf(e, path, nowItem, snapItem), nullptr, out);
    }
    cJSON_Delete(now);
    out.print(live ? tr("Działa od razu; trwale po 'save'\r\n", "Applied now; permanent after 'save'\r\n")
                   : tr("Zadziała po restarcie; trwale po 'save'\r\n", "Takes effect after restart; permanent after 'save'\r\n"));
    return CmdResult::OK;
}

void ConfigStore::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("config", { "konfiguracja (pliki JSON na LittleFS)", "configuration (JSON files on LittleFS)" });
    exec.add("config", { "show", "[modul]", { "bieżąca konfiguracja ze źródłem i zakresem każdej wartości", "current configuration with the source and range of each value" }, &cmdShow, this });
    exec.add("config", { "get", "<modul> <pole>", { "wartość jednego pola (pole.podpole)", "value of one field (field.subfield)" }, &cmdGet, this });
    exec.add("config", { "set", "<modul> <pole> <wartosc>", { "zmiana pola w RAM (trwałe po save)", "changes a field in RAM (permanent after save)" }, &cmdSet, this });
    exec.add("config", { "unset", "<modul> <pole>", { "przywrócenie wartości domyślnej pola", "restores the default value of a field" }, &cmdUnset, this });
    exec.add("config", { "fs", "", { "stan LittleFS i lista plików w /config", "LittleFS status and list of files in /config" }, &cmdFs, this });
    exec.add("config", { "cat", "<plik>", { "zawartość pliku z /config", "contents of a file in /config" }, &cmdCat, this });
    exec.add("config", { "factory-reset", "yes", { "usuwa pliki konfiguracji i restartuje", "deletes the configuration files and restarts" }, &cmdFactoryReset, this });
    exec.add("config", { "diff", "[modul]", { "pola zmienione, a niezapisane", "fields changed but not saved" }, &cmdDiff, this });
    exec.add("config", { "revert", "[modul]", { "porzuca niezapisane zmiany (wczytuje z pliku)", "discards unsaved changes (reloads from file)" }, &cmdRevert, this });
    exec.add(nullptr, { "lang", "[pl|en]", { "język panelu i konsoli (global.language, trwałe po save)", "panel and console language (global.language, permanent after save)" }, &cmdLang, this });
    exec.add(nullptr, { "save", "[modul]", { "zapisuje zmienioną konfigurację (wszystkich modułów albo jednego)", "saves the changed configuration (all modules or one)" }, &cmdSave, this });
}

CmdResult ConfigStore::cmdShow(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    if (argc == 1) {
        for (const Entry& e : self->entries) {
            self->show(e, out);
        }
        return CmdResult::OK;
    }
    Entry* e = self->findModule(argv[1], out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    self->show(*e, out);
    return CmdResult::OK;
}

// "module.field" in one argument (e.g. config set ddns.2.provider noip) – split into two.
static int splitModuleArg(int argc, char** argv, char** out, char* moduleBuf, size_t size)
{
    for (int i = 0; i < argc && i < 6; ++i) {
        out[i] = argv[i];
    }
    if (argc >= 2) {
        char* dot = strchr(argv[1], '.');
        if (dot != nullptr && argc < 6) {
            size_t n = std::min(static_cast<size_t>(dot - argv[1]), size - 1);
            memcpy(moduleBuf, argv[1], n);
            moduleBuf[n] = '\0';
            out[1] = moduleBuf;
            for (int i = argc; i > 2; --i) {
                out[i] = out[i - 1];
            }
            out[2] = dot + 1;
            return argc + 1;
        }
    }
    return argc;
}

CmdResult ConfigStore::cmdGet(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    char* args[7];
    char moduleBuf[24];
    argc = splitModuleArg(argc, argv, args, moduleBuf, sizeof(moduleBuf));
    argv = args;
    if (argc != 3) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* e = self->findModule(argv[1], out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    cJSON* cur = e->module->saveConfig();
    std::string path;
    cJSON* parent = nullptr;
    cJSON* item = findField(cur, argv[2], path, &parent, out);
    if (item == nullptr) {
        cJSON_Delete(cur);
        return CmdResult::ERR_FAIL;
    }
    char buf[64];
    strlcpy(buf, path.c_str(), sizeof(buf));
    const cJSON* snapItem = e->snapshot;
    char* save = nullptr;
    for (char* part = strtok_r(buf, ".", &save); part != nullptr && snapItem != nullptr;
         part = strtok_r(nullptr, ".", &save)) {
        snapItem = cJSON_GetObjectItemCaseSensitive(snapItem, part);
    }
    printField(path.c_str(), item, sourceOf(*e, path, item, snapItem), hintOf(*e, path), out);
    cJSON_Delete(cur);
    return CmdResult::OK;
}

CmdResult ConfigStore::cmdSet(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    char* args[7];
    char moduleBuf[24];
    argc = splitModuleArg(argc, argv, args, moduleBuf, sizeof(moduleBuf));
    argv = args;
    if (argc != 4) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* e = self->findModule(argv[1], out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    return self->setField(*e, argv[2], argv[3], out);
}

// "lang" without an argument – current language; with an argument – like "config set global language".
CmdResult ConfigStore::cmdLang(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc == 1) {
        out.printf(tr("Język: %s (pl/en)\r\n", "Language: %s (pl/en)\r\n"), i18n::code(i18n::language()));
        return CmdResult::OK;
    }
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* e = self->findModule("global", out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    return self->setField(*e, "language", argv[1], out);
}

CmdResult ConfigStore::cmdUnset(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    char* args[7];
    char moduleBuf[24];
    argc = splitModuleArg(argc, argv, args, moduleBuf, sizeof(moduleBuf));
    argv = args;
    if (argc != 3) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* e = self->findModule(argv[1], out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    return self->setField(*e, argv[2], nullptr, out);
}

CmdResult ConfigStore::cmdSave(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    if (argc == 1) {
        if (!self->saveAll(&out)) {
            out.print(tr("Błąd zapisu części konfiguracji (szczegóły w logu)\r\n", "Error writing part of the configuration (details in the log)\r\n"));
            return CmdResult::ERR_FAIL;
        }
        return CmdResult::OK;
    }
    Entry* e = self->findModule(argv[1], out);
    if (e == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    switch (self->saveEntry(*e)) {
    case SaveResult::WRITTEN:
        out.printf(tr("Zapisano: %s.json\r\n", "Saved: %s.json\r\n"), e->module->configName());
        return CmdResult::OK;
    case SaveResult::UNCHANGED:
        out.printf(tr("%s.json: bez zmian\r\n", "%s.json: unchanged\r\n"), e->module->configName());
        return CmdResult::OK;
    case SaveResult::FAILED:
        break;
    }
    out.print(tr("Błąd zapisu (szczegóły w logu)\r\n", "Write error (details in the log)\r\n"));
    return CmdResult::ERR_FAIL;
}

CmdResult ConfigStore::cmdDiff(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* only = nullptr;
    if (argc == 2 && (only = self->findModule(argv[1], out)) == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    int total = 0;
    for (Entry& e : self->entries) {
        if (only != nullptr && &e != only) {
            continue;
        }
        cJSON* cur = e.module->saveConfig();
        std::vector<std::string> paths;
        collectDiff(cur, e.snapshot, "", paths);
        for (const std::string& p : paths) {
            // Current value and value from the file; passwords masked.
            const cJSON* now = cur;
            const cJSON* was = e.snapshot;
            char buf[64];
            strlcpy(buf, p.c_str(), sizeof(buf));
            char* save = nullptr;
            for (char* part = strtok_r(buf, ".", &save); part != nullptr; part = strtok_r(nullptr, ".", &save)) {
                now = now ? cJSON_GetObjectItemCaseSensitive(now, part) : nullptr;
                was = was ? cJSON_GetObjectItemCaseSensitive(was, part) : nullptr;
            }
            bool secret = isSecretPath(p);
            char* a = (was && !secret) ? cJSON_PrintUnformatted(was) : nullptr;
            char* b = (now && !secret) ? cJSON_PrintUnformatted(now) : nullptr;
            out.printf("  %s.%s: %s -> %s\r\n", e.module->configName(), p.c_str(),
                       secret ? "***" : (a ? a : "-"), secret ? "***" : (b ? b : "-"));
            cJSON_free(a);
            cJSON_free(b);
            ++total;
        }
        cJSON_Delete(cur);
    }
    if (total == 0) {
        out.print(tr("Brak niezapisanych zmian\r\n", "No unsaved changes\r\n"));
    }
    return CmdResult::OK;
}

// Reload from the file; fields applied immediately (applyLive) go back immediately.
void ConfigStore::revertEntry(Entry& e, int* live, int* boot, size_t* changedCount)
{
    cJSON* before = e.module->saveConfig();
    std::vector<std::string> changed;
    collectDiff(before, e.snapshot, "", changed);
    cJSON_Delete(before);
    e.module->revertExtra();
    *changedCount = changed.size();
    if (changed.empty()) {
        return;
    }
    loadEntry(e);
    for (const std::string& p : changed) {
        if (e.module->applyLive(p)) {
            ++*live;
        } else {
            ++*boot;
        }
    }
}

CmdResult ConfigStore::cmdRevert(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->entriesMutex);
    Entry* only = nullptr;
    if (argc == 2 && (only = self->findModule(argv[1], out)) == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    for (Entry& e : self->entries) {
        if (only != nullptr && &e != only) {
            continue;
        }
        int live = 0, boot = 0;
        size_t changed = 0;
        self->revertEntry(e, &live, &boot, &changed);
        if (changed > 0) {
            out.printf(tr("%s: przywrócono pól: %u (od razu: %d, po restarcie: %d)\r\n", "%s: restored %u fields (applied now: %d, after restart: %d)\r\n"), e.module->configName(),
                       (unsigned)changed, live, boot);
        }
    }
    out.print(tr("Gotowe\r\n", "Done\r\n"));
    return CmdResult::OK;
}

CmdResult ConfigStore::cmdFs(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    if (!self->isMounted) {
        out.print(tr("LittleFS nie jest zamontowany\r\n", "LittleFS is not mounted\r\n"));
        return CmdResult::ERR_FAIL;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(PARTITION_LABEL, &total, &used);
    out.printf(tr("LittleFS: zajęte %u / %u B\r\n", "LittleFS: used %u / %u B\r\n"), (unsigned)used, (unsigned)total);

    std::lock_guard<std::mutex> lock(self->mutex);
    DIR* dir = opendir(CONFIG_DIR);
    if (dir == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    int count = 0;
    char path[300];
    while (dirent* e = readdir(dir)) {
        struct stat st = {};
        snprintf(path, sizeof(path), "%s/%s", CONFIG_DIR, e->d_name);
        stat(path, &st);
        out.printf("  %-20s %6ld B\r\n", e->d_name, (long)st.st_size);
        ++count;
    }
    closedir(dir);
    if (count == 0) {
        out.print(tr("  (brak plików – wszystkie moduły używają wartości domyślnych)\r\n", "  (no files – all modules use default values)\r\n"));
    }
    return CmdResult::OK;
}

CmdResult ConfigStore::cmdCat(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc != 2 || strchr(argv[1], '/') != nullptr) {
        return CmdResult::ERR_ARGS;
    }

    std::lock_guard<std::mutex> lock(self->mutex);
    char path[64];
    makePath(path, sizeof(path), argv[1]);
    size_t len = 0;
    char* text = self->readFile(path, &len);
    if (text == nullptr) {
        out.printf(tr("Brak pliku %s\r\n", "No file %s\r\n"), argv[1]);
        return CmdResult::ERR_FAIL;
    }
    // LF -> CRLF conversion for the terminal.
    for (char* line = text; line != nullptr && *line != '\0';) {
        char* nl = strchr(line, '\n');
        size_t n = nl ? static_cast<size_t>(nl - line) : strlen(line);
        out.write(line, n);
        out.print("\r\n");
        line = nl ? nl + 1 : nullptr;
    }
    free(text);
    return CmdResult::OK;
}

CmdResult ConfigStore::cmdFactoryReset(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ConfigStore*>(ctx);
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        out.print(tr("Potwierdź: config factory-reset yes\r\n", "Confirm: config factory-reset yes\r\n"));
        return CmdResult::ERR_ARGS;
    }
    if (!self->factoryReset()) {
        out.print(tr("Błąd resetu fabrycznego\r\n", "Factory reset error\r\n"));
        return CmdResult::ERR_FAIL;
    }
    out.print(tr("Usunięto konfigurację, restart z wartościami domyślnymi...\r\n", "Configuration deleted, restarting with default values...\r\n"));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return CmdResult::OK;
}

// ---- web panel API ----

// Field type name for the panel: bool / number / string / list.
static const char* jsonType(const cJSON* item)
{
    if (cJSON_IsBool(item)) {
        return "bool";
    }
    if (cJSON_IsNumber(item)) {
        return "number";
    }
    if (cJSON_IsString(item)) {
        return "string";
    }
    return "list";
}

// Fields whose values are never shown (passwords, DDNS secrets).
static bool isSecretPath(const std::string& path)
{
    auto endsWith = [&path](const char* s) {
        size_t n = strlen(s);
        return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
    };
    return endsWith("pass") || endsWith("secret");
}

// Value to display: passwords replaced with empty text (with information whether they are set).
static cJSON* publicValue(const std::string& path, const cJSON* item)
{
    if (isSecretPath(path) && cJSON_IsString(item)) {
        return cJSON_CreateString("");
    }
    return cJSON_Duplicate(item, true);
}

// Item at a dotted path in a JSON object (nullptr when missing).
static const cJSON* lookupPath(const cJSON* root, const std::string& path)
{
    char buf[64];
    strlcpy(buf, path.c_str(), sizeof(buf));
    char* save = nullptr;
    const cJSON* node = root;
    for (char* part = strtok_r(buf, ".", &save); part != nullptr && node != nullptr; part = strtok_r(nullptr, ".", &save)) {
        node = cJSON_GetObjectItemCaseSensitive(node, part);
    }
    return node;
}

static void collectLeaves(const cJSON* obj, const std::string& prefix,
                          std::vector<std::pair<std::string, const cJSON*>>& leaves)
{
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, obj)
    {
        std::string path = prefix + item->string;
        if (cJSON_IsObject(item)) {
            collectLeaves(item, path + ".", leaves);
        } else {
            leaves.emplace_back(path, item);
        }
    }
}

cJSON* ConfigStore::moduleJson(const char* name)
{
    std::lock_guard<std::mutex> lock(entriesMutex);
    for (Entry& e : entries) {
        if (strcmp(e.module->configName(), name) != 0) {
            continue;
        }
        cJSON* cur = e.module->saveConfig();
        std::vector<std::pair<std::string, const cJSON*>> leaves;
        collectLeaves(cur, "", leaves);

        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "module", name);
        cJSON* fields = cJSON_AddArrayToObject(root, "fields");
        for (const auto& leaf : leaves) {
            const std::string& path = leaf.first;
            cJSON* f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "path", path.c_str());
            cJSON_AddStringToObject(f, "type", jsonType(leaf.second));
            cJSON_AddItemToObject(f, "value", publicValue(path, leaf.second));
            if (isSecretPath(path)) {
                cJSON_AddBoolToObject(f, "secret", true);
                cJSON_AddBoolToObject(f, "set", cJSON_IsString(leaf.second) && leaf.second->valuestring[0] != '\0');
            }
            cJSON_AddStringToObject(f, "source", sourceOf(e, path, leaf.second, lookupPath(e.snapshot, path)));
            cJSON_AddStringToObject(f, "hint", hintOf(e, path));
            cJSON_AddBoolToObject(f, "live", e.module->isLive(path));
            cJSON_AddItemToArray(fields, f);
        }
        cJSON_Delete(cur);
        return root;
    }
    return nullptr;
}

// Field changes like "config set" (same rules); null = restore the default value.
cJSON* ConfigStore::setFieldsJson(const char* name, const cJSON* fields)
{
    std::lock_guard<std::mutex> lock(entriesMutex);
    Entry* entry = nullptr;
    for (Entry& e : entries) {
        if (strcmp(e.module->configName(), name) == 0) {
            entry = &e;
        }
    }
    if (entry == nullptr || !cJSON_IsObject(fields)) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* results = cJSON_AddArrayToObject(root, "results");
    bool allOk = true;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, fields)
    {
        char value[96];
        const char* v = value;
        if (cJSON_IsNull(item)) {
            v = nullptr;
        } else if (cJSON_IsBool(item)) {
            strlcpy(value, cJSON_IsTrue(item) ? "true" : "false", sizeof(value));
        } else if (cJSON_IsNumber(item)) {
            snprintf(value, sizeof(value), "%ld", static_cast<long>(item->valuedouble));
        } else if (cJSON_IsString(item)) {
            strlcpy(value, item->valuestring, sizeof(value));
        } else {
            v = "";
        }

        StringOutput out(512);
        bool live = false;
        CmdResult r = (cJSON_IsArray(item) || cJSON_IsObject(item))
                          ? (out.print(tr("To pole jest listą – zmieniaj je poleceniami modułu",
                                          "This field is a list – change it with module commands")),
                             CmdResult::ERR_FAIL)
                          : setField(*entry, item->string, v, out, &live);
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "path", item->string);
        cJSON_AddBoolToObject(res, "ok", r == CmdResult::OK);
        cJSON_AddBoolToObject(res, "live", live);
        cJSON_AddStringToObject(res, "message", out.text.c_str());
        cJSON_AddItemToArray(results, res);
        allOk = allOk && r == CmdResult::OK;
    }
    cJSON_AddBoolToObject(root, "ok", allOk);
    return root;
}

cJSON* ConfigStore::diffJson()
{
    std::lock_guard<std::mutex> lock(entriesMutex);
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(root, "changes");
    int count = 0;
    for (Entry& e : entries) {
        cJSON* cur = e.module->saveConfig();
        std::vector<std::string> paths;
        collectDiff(cur, e.snapshot, "", paths);
        for (const std::string& p : paths) {
            cJSON* c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "module", e.module->configName());
            cJSON_AddStringToObject(c, "path", p.c_str());
            bool secret = isSecretPath(p);
            const cJSON* was = lookupPath(e.snapshot, p);
            const cJSON* now = lookupPath(cur, p);
            cJSON_AddItemToObject(c, "old", secret || !was ? cJSON_CreateString(secret ? "***" : "-")
                                                           : cJSON_Duplicate(was, true));
            cJSON_AddItemToObject(c, "new", secret || !now ? cJSON_CreateString(secret ? "***" : "-")
                                                           : cJSON_Duplicate(now, true));
            cJSON_AddBoolToObject(c, "live", e.module->isLive(p));
            cJSON_AddItemToArray(list, c);
            ++count;
        }
        if (e.module->extraDirty()) {
            cJSON* c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "module", e.module->configName());
            cJSON_AddStringToObject(c, "path", "(plik dodatkowy)");
            cJSON_AddStringToObject(c, "old", "-");
            cJSON_AddStringToObject(c, "new", "zmieniony");
            cJSON_AddBoolToObject(c, "live", true);
            cJSON_AddItemToArray(list, c);
            ++count;
        }
        cJSON_Delete(cur);
    }
    cJSON_AddNumberToObject(root, "count", count);
    return root;
}

cJSON* ConfigStore::saveJson(const char* name)
{
    std::lock_guard<std::mutex> lock(entriesMutex);
    cJSON* root = cJSON_CreateObject();
    cJSON* written = cJSON_AddArrayToObject(root, "written");
    int unchanged = 0;
    bool ok = true;
    for (Entry& e : entries) {
        if (name != nullptr && strcmp(e.module->configName(), name) != 0) {
            continue;
        }
        switch (saveEntry(e)) {
        case SaveResult::WRITTEN:
            cJSON_AddItemToArray(written, cJSON_CreateString(e.module->configName()));
            break;
        case SaveResult::UNCHANGED:
            ++unchanged;
            break;
        case SaveResult::FAILED:
            ok = false;
            break;
        }
    }
    cJSON_AddNumberToObject(root, "unchanged", unchanged);
    cJSON_AddBoolToObject(root, "ok", ok);
    return root;
}

cJSON* ConfigStore::revertJson(const char* name)
{
    std::lock_guard<std::mutex> lock(entriesMutex);
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(root, "reverted");
    for (Entry& e : entries) {
        if (name != nullptr && strcmp(e.module->configName(), name) != 0) {
            continue;
        }
        int live = 0, boot = 0;
        size_t changed = 0;
        revertEntry(e, &live, &boot, &changed);
        if (changed > 0) {
            cJSON* r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "module", e.module->configName());
            cJSON_AddNumberToObject(r, "live", live);
            cJSON_AddNumberToObject(r, "boot", boot);
            cJSON_AddItemToArray(list, r);
        }
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return root;
}

// Files included in a backup and accepted on restore.
static bool backupFileAllowed(const char* name)
{
    size_t n = strlen(name);
    bool json = n > 5 && strcmp(name + n - 5, ".json") == 0;
    return (json && strcmp(name, "web_auth.json") != 0) || strcmp(name, "dns_block.txt") == 0;
}

// Saved files (not the RAM state): *.json without web_auth.json, plus dns_block.txt.
cJSON* ConfigStore::backupJson()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mutex);
        DIR* dir = opendir(CONFIG_DIR);
        if (dir == nullptr) {
            return nullptr;
        }
        while (dirent* d = readdir(dir)) {
            if (backupFileAllowed(d->d_name)) {
                names.emplace_back(d->d_name);
            }
        }
        closedir(dir);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "esp32router-config");
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON* files = cJSON_AddObjectToObject(root, "files");
    for (const std::string& n : names) {
        size_t len = 0;
        char* text = readText(n.c_str(), &len);
        if (text == nullptr) {
            continue;
        }
        cJSON* json = n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 ? cJSON_ParseWithLength(text, len)
                                                                            : nullptr;
        cJSON_AddItemToObject(files, n.c_str(), json ? json : cJSON_CreateString(text));
        free(text);
    }
    return root;
}

int ConfigStore::restoreJson(const cJSON* backup, std::string& error)
{
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(backup, "type");
    const cJSON* files = cJSON_GetObjectItemCaseSensitive(backup, "files");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "esp32router-config") != 0 || !cJSON_IsObject(files)) {
        error = tr("To nie jest plik kopii konfiguracji Esp32Router", "This is not an Esp32Router configuration backup");
        return -1;
    }
    // Check everything first, then write.
    const cJSON* f = nullptr;
    cJSON_ArrayForEach(f, files)
    {
        bool json = strlen(f->string) > 5 && strcmp(f->string + strlen(f->string) - 5, ".json") == 0;
        if (!backupFileAllowed(f->string) || (json && !cJSON_IsObject(f)) || (!json && !cJSON_IsString(f))) {
            error = std::string(tr("Nieprawidłowy wpis w kopii: ", "Invalid entry in backup: ")) + f->string;
            return -1;
        }
    }
    int count = 0;
    cJSON_ArrayForEach(f, files)
    {
        bool ok = cJSON_IsObject(f) ? save(f->string, f) : saveText(f->string, f->valuestring, strlen(f->valuestring));
        if (!ok) {
            error = std::string(tr("Błąd zapisu ", "Write error ")) + f->string;
            return -1;
        }
        ++count;
    }
    LOG_W("Przywrocono konfiguracje z kopii (%d plikow)", count);
    return count;
}
