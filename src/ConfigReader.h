/**
 * @file ConfigReader.h
 * @brief IConfigurable interface and ConfigReader – typed reading of configuration fields with defaults.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"

#include "i18n/I18n.h"

class ConfigReader;

/**
 * @brief A module that has its own configuration file /config/<configName()>.json.
 *
 * The module knows its fields and default values (include/Defaults.h); ConfigStore only reads
 * and writes the files. Each module keeps a "configured" copy (loaded, changed by "config set",
 * saved) and an "active" copy used by running tasks.
 */
class IConfigurable {
public:
    virtual ~IConfigurable() = default;

    /// @return Module name = file name without ".json" (e.g. "dhcp").
    virtual const char* configName() const = 0;
    /**
     * @brief Reads the fields from the file; missing or invalid fields get default values.
     *
     * Also called by "config set" to validate a change and by "config revert".
     */
    virtual void loadConfig(ConfigReader& r) = 0;
    /// @return Current configuration from RAM as a new JSON object (freed by the caller).
    virtual cJSON* saveConfig() const = 0;
    /**
     * @brief Called after "config set/unset" of a field.
     * @param path Field path with dots, e.g. "log.level".
     * @return true when the module applied the change immediately; false = takes effect after restart (default).
     */
    virtual bool applyLive(const std::string& path) { return false; }
    /// @return Whether a change of the field applies immediately (same as applyLive would return) – for the web panel.
    virtual bool isLive(const std::string& path) const { return false; }
    /// @brief Saves extra module files (e.g. the DNS block list); called by "save" after the JSON. @return true on success.
    virtual bool saveExtra() { return true; }
    /// @brief Discards unsaved changes in extra files (called by "config revert").
    virtual void revertExtra() {}
    /// @return Whether extra files have unsaved changes (unsaved-changes bar in the panel).
    virtual bool extraDirty() const { return false; }
};

/// Description of the allowed values of a field in both languages ("config show", panel forms).
struct ConfigHint {
    std::string path; ///< Field path with dots.
    std::string pl;   ///< Polish text.
    std::string en;   ///< English text.

    /// @return The text in the current language.
    const std::string& get() const { return i18n::language() == i18n::Lang::EN ? en : pl; }
};

/// What ConfigReader collects while reading (all pointers optional).
struct ConfigSinks {
    std::vector<std::string>* fromFile = nullptr; ///< Fields read correctly from the file.
    std::vector<std::string>* errors = nullptr;   ///< Rejected fields with the reason (for "config set").
    std::vector<ConfigHint>* hints = nullptr;     ///< Field -> allowed values (pl and en).
};

/**
 * @brief Reads fields of a JSON object with the default-value policy.
 *
 * - no file                    -> default values, no per-field logs,
 * - missing field              -> default value (info log),
 * - wrong type / out of range  -> default value (warning with file and field name),
 * - unknown fields             -> ignored (warning in finish()).
 *
 * Fields read correctly, errors and descriptions of allowed values go to ConfigSinks.
 * A reader for a nested object is created with object().
 */
class ConfigReader {
public:
    /**
     * @param file   File name for logs (e.g. "dhcp.json").
     * @param obj    Object to read (nullptr = no file: all defaults).
     * @param sinks  Where to collect fields, errors and hints.
     * @param prefix Path prefix of nested objects ("log.").
     */
    ConfigReader(const char* file, const cJSON* obj, const ConfigSinks& sinks, std::string prefix = "");

    /**
     * @brief Reads a text field.
     * @param maxLen Maximum length in bytes.
     * @param valid  Optional validator.
     * @param hint   Allowed values for "config show" (default "text, max N"), e.g. { "1-32 znaki", "1-32 chars" }.
     */
    std::string str(const char* key, const std::string& def, size_t maxLen, bool (*valid)(const std::string&) = nullptr,
                    i18n::Text hint = {});
    /// @brief Reads an integer field in the range min..max.
    long integer(const char* key, long def, long min, long max);
    /// @brief Reads a true/false field.
    bool boolean(const char* key, bool def);
    /// @brief Reads an IPv4 address given as text. @return Address in network byte order (esp_ip4_addr_t.addr).
    uint32_t ipv4(const char* key, uint32_t def);
    /// @brief Reader for a nested object (missing object = all defaults without separate logs).
    ConfigReader object(const char* key);
    /// @brief List (JSON array) – the module checks the entries and reports a bad one with rejectEntry(). @return nullptr when missing.
    const cJSON* list(const char* key);
    /// @brief Reports a skipped list entry (it disappears on "save").
    void rejectEntry(const char* key, int index, const char* why);
    /// @brief Rejects a field after a cross-field check (e.g. pool outside the subnet).
    void reject(const char* key, const char* why) { rejected(key, why); }

    /// @brief Warns about unknown fields. Called after all fields of the object were read.
    void finish();

private:
    /// Looks up a field and remembers it as known; logs a missing field.
    const cJSON* field(const char* key);
    /// Records a field read correctly from the file.
    void accepted(const char* key);
    /// Logs a rejected field (ASCII) and records the reason for "config set".
    void rejected(const char* key, const std::string& why);
    /// Records the allowed values of a field in both languages.
    void hint(const char* key, std::string pl, std::string en);

    const char* file;               ///< File name for logs.
    const cJSON* obj;               ///< Object being read (may be nullptr).
    ConfigSinks sinks;              ///< Where results are collected.
    std::string prefix;             ///< Path prefix of this object.
    std::vector<std::string> known; ///< Field names read so far (unknown-field check).
};
