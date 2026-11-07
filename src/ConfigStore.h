/**
 * @file ConfigStore.h
 * @brief JSON configuration files on LittleFS and the "config"/"save"/"lang" console commands.
 */
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"

/**
 * @brief Owner of the configuration files in /lfs/config on the LittleFS partition.
 *
 * ConfigStore does not know the fields of the modules: it offers reading a file as cJSON,
 * atomic writes (.tmp + rename) and factory reset. Modules (IConfigurable) register through
 * add(); loadAll()/saveAll() read and write their <name>.json files. It also implements the
 * generic "config show/get/set/unset/diff/revert", "save" and "lang" commands and the JSON API
 * used by the web panel.
 *
 * Thread safety: commands may arrive at the same time from COM, Telnet and the web panel;
 * the module list is guarded by entriesMutex, file operations by mutex.
 */
class ConfigStore {
public:
    static constexpr const char* MOUNT_POINT = "/lfs";       ///< VFS mount point of the LittleFS partition.
    static constexpr const char* CONFIG_DIR = "/lfs/config"; ///< Directory with the configuration files.

    /// Result of load().
    enum class LoadResult {
        OK,      ///< File read and parsed.
        MISSING, ///< No such file.
        CORRUPT, ///< File could not be parsed – it was moved to <name>.bad.
        ERROR,   ///< Read error or no file system.
    };

    /**
     * @brief Mounts LittleFS (formatting the partition if it cannot be mounted) and creates /config.
     * @return true when the file system is available.
     */
    bool mount();
    /// @return true when LittleFS is mounted.
    bool mounted() const { return isMounted; }

    /**
     * @brief Reads and parses a JSON file from /config.
     * @param name File name in /config, e.g. "global.json".
     * @param out  On OK receives the parsed object; the caller frees it with cJSON_Delete.
     */
    LoadResult load(const char* name, cJSON** out);
    /// @brief Writes a JSON file atomically (temporary file, fsync, rename). @return true on success.
    bool save(const char* name, const cJSON* json);
    /**
     * @brief Reads a text file from /config.
     * @return Buffer allocated with malloc (freed by the caller) or nullptr; *len receives the size.
     */
    char* readText(const char* name, size_t* len);
    /// @brief Writes a text file in /config atomically. @return true on success.
    bool saveText(const char* name, const char* data, size_t len);
    /// @brief Deletes a file from /config. @return true when the file is gone.
    bool remove(const char* name);
    /// @brief Deletes all configuration files (the caller restarts the device). @return true on success.
    bool factoryReset();

    /// @brief Registers a module with its own file. Only at start-up, before loadAll().
    void add(IConfigurable& module);
    /// @brief Loads the files of all registered modules (missing file or field = default value).
    void loadAll();
    /**
     * @brief Saves modules whose configuration differs from the file (or whose file does not exist).
     * @param out Optional output for the summary ("Saved: …; unchanged: N").
     * @return false when any file could not be written.
     */
    bool saveAll(IOutput* out = nullptr);

    /// @brief Registers the "config", "save" and "lang" commands.
    void registerCommands(CommandExecutor& exec);

    /// @name Web panel API (returned JSON is freed by the caller; nullptr = unknown module)
    /// @{
    /// Fields of a module with value, source, hint and whether a change applies immediately.
    cJSON* moduleJson(const char* name);
    /// Changes fields like "config set"; null value = default. Returns per-field results.
    cJSON* setFieldsJson(const char* name, const cJSON* fields);
    /// Fields changed but not saved, in all modules.
    cJSON* diffJson();
    /// Saves one module or all (name == nullptr).
    cJSON* saveJson(const char* name);
    /// Discards unsaved changes of one module or all (name == nullptr).
    cJSON* revertJson(const char* name);
    /// Backup: saved /config files (without web_auth.json) as one JSON object.
    cJSON* backupJson();
    /**
     * @brief Restores files from a backup object (the caller restarts the device).
     * @return Number of written files or -1 on error (error receives the reason).
     */
    int restoreJson(const cJSON* backup, std::string& error);
    /// @}

private:
    /// Registered module and the state of its file.
    struct Entry {
        IConfigurable* module;             ///< The module (owned by Router).
        std::vector<std::string> fromFile; ///< Field paths read from the file / saved.
        cJSON* snapshot = nullptr;         ///< Configuration after loading or the last save (for diff).
        bool fileExists = false;           ///< The file exists on LittleFS (save creates it).
        std::vector<ConfigHint> hints;     ///< Field path -> allowed values (pl and en).
    };

    /// Module by name or unambiguous prefix (exact name wins); prints the reason when not found.
    Entry* findModule(const char* token, IOutput& out);
    /// Loads the module file, collects hints and the snapshot.
    void loadEntry(Entry& e);
    /// Result of saveEntry().
    enum class SaveResult { WRITTEN, UNCHANGED, FAILED };
    /// Saves one module when its configuration differs from the snapshot.
    SaveResult saveEntry(Entry& e);
    /// Collects paths of fields that differ between the current configuration and the snapshot.
    static void collectDiff(const cJSON* cur, const cJSON* snap, const std::string& prefix,
                            std::vector<std::string>& paths);
    /// Finds a field by "a.b" path (segments may be unambiguous prefixes); prints the field list when not found.
    static cJSON* findField(cJSON* root, const char* token, std::string& path, cJSON** parent, IOutput& out);
    /// Prints one "config show" line: path = value [source] (hint); passwords are masked.
    static void printField(const char* path, const cJSON* item, const char* source, const char* hint, IOutput& out);
    /// Source code of a value: "plik", "domyslne" or "zmienione, niezapisane" (kept as API codes).
    static const char* sourceOf(const Entry& e, const std::string& path, const cJSON* item, const cJSON* snapItem);
    /// Allowed values of a field in the current language ("" when unknown).
    static const char* hintOf(const Entry& e, const std::string& path);
    /**
     * @brief Changes one field (value == nullptr restores the default), validated by the module's loadConfig().
     * @param live Optional: receives whether the change was applied immediately (applyLive).
     */
    CmdResult setField(Entry& e, const char* fieldToken, const char* value, IOutput& out, bool* live = nullptr);
    /// Reloads the module file; counts fields applied now / after restart.
    void revertEntry(Entry& e, int* live, int* boot, size_t* changedCount);

    /// Prints the whole module ("config show").
    void show(const Entry& e, IOutput& out) const;
    /// Prints all fields of an object recursively.
    static void showObject(const cJSON* cur, const cJSON* snap, const std::string& prefix, const Entry& e, IOutput& out);
    /// Collects the paths of all leaf fields of an object.
    static void collectPaths(const cJSON* obj, const std::string& prefix, std::vector<std::string>& paths);

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdShow(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdSave(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdLang(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdRevert(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdDiff(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdGet(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdSet(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdUnset(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdFs(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdCat(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdFactoryReset(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    /// Builds "/lfs/config/<name><suffix>".
    static void makePath(char* buf, size_t size, const char* name, const char* suffix = "");
    /// Reads a whole file into a malloc buffer (zero-terminated); nullptr on error.
    char* readFile(const char* path, size_t* len);

    std::vector<Entry> entries; ///< Registered modules in registration order.
    std::mutex entriesMutex;    ///< save/show may arrive at the same time from COM, Telnet and the panel.
    std::mutex mutex;           ///< File operations.
    bool isMounted = false;     ///< LittleFS is mounted.
};
