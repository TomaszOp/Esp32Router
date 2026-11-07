/**
 * @file StringOutput.h
 * @brief Command output collected into a string.
 */
#pragma once

#include <string>

#include "IOutput.h"

/// @brief Command output collected into text (e.g. the /api/cmd response of the web panel); UTF-8 without conversion.
class StringOutput : public IOutput {
public:
    /// @param limit Maximum collected size in bytes; the rest is dropped and @ref truncated is set.
    explicit StringOutput(size_t limit = 32 * 1024)

        : limit(limit)
    {
    }

    /// @brief Appends data up to the limit.
    void write(const char* data, size_t len) override
    {
        if (text.size() + len > limit) {
            len = text.size() < limit ? limit - text.size() : 0;
            truncated = true;
        }
        text.append(data, len);
    }

    std::string text;       ///< Collected output.
    bool truncated = false; ///< Output exceeded the limit and was cut.

private:
    size_t limit; ///< Maximum size of text in bytes.

};
