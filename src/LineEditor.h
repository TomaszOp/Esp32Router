/**
 * @file LineEditor.h
 * @brief Line editing shared by the COM console and Telnet sessions.
 */
#pragma once

#include <cstddef>

#include "CommandExecutor.h"
#include "IOutput.h"

/**
 * @brief Shared line editing for the COM console and Telnet sessions.
 *
 * Echo, Backspace, Ctrl+C, CR/LF/CRLF as Enter, history of recent lines (up/down arrows).
 * A finished line is passed to CommandExecutor. Only printable ASCII is accepted.
 * One instance per console/session; used only by its own task.
 */
class LineEditor {
public:
    /**
     * @param executor        Command interpreter.
     * @param out             Where echo and command output go.
     * @param sessionCanClose true for Telnet: "exit" / Ctrl+D may end the session.
     */
    LineEditor(const CommandExecutor& executor, IOutput& out, bool sessionCanClose);

    /// @brief Prints the prompt "esp32router> ".
    void prompt();
    /// @brief Processes one received character (escape sequences, editing keys, Enter).
    void handleChar(char c);

    /// @return Telnet session: "exit" or Ctrl+D on an empty line ends the session.
    bool closeRequested() const { return closing; }

private:
    static constexpr size_t LINE_SIZE = 128; ///< Line buffer (127 characters + terminator).
    static constexpr int HISTORY_SIZE = 10;  ///< Number of remembered lines.

    /// Executes the line, stores it in the history, prints a new prompt.
    void submitLine();
    /// Shows history entry index (-1 = empty current line).
    void showHistory(int index);
    /// Clears the terminal line and prints the prompt with the current line.
    void redrawLine();

    const CommandExecutor& executor; ///< Command interpreter.
    IOutput& out;                    ///< Echo and command output.
    const bool sessionCanClose;      ///< Session may be closed by the user (Telnet).
    bool closing = false;            ///< Close requested.

    char line[LINE_SIZE] = {}; ///< Current line.
    size_t lineLen = 0;        ///< Length of the current line.
    bool lastWasCr = false;    ///< Previous character was CR (CR LF counts as one Enter).
    int escState = 0;          ///< 0 = normal character, 1 = after ESC, 2 = after ESC [.

    char history[HISTORY_SIZE][LINE_SIZE] = {}; ///< Ring of recent lines.
    int historyCount = 0;                        ///< Number of stored lines.
    int historyHead = 0;                         ///< Index of the next entry.
    int historyPos = -1;                         ///< -1 = editing the current line.
};
