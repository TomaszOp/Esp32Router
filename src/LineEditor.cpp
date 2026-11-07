/**
 * @file LineEditor.cpp
 * @brief LineEditor: echo, editing keys, history.
 */
#include "LineEditor.h"

#include <cstring>

static const char* PROMPT = "esp32router> ";

LineEditor::LineEditor(const CommandExecutor& executor, IOutput& out, bool sessionCanClose)
    : executor(executor),
      out(out),
      sessionCanClose(sessionCanClose)
{
}

void LineEditor::prompt()
{
    out.print(PROMPT);
}

void LineEditor::redrawLine()
{
    out.print("\r\x1b[K");
    prompt();
    out.write(line, lineLen);
}

void LineEditor::handleChar(char c)
{
    // ESC [ A / ESC [ B sequences (arrows); others are ignored.
    if (escState == 1) {
        escState = (c == '[' || c == 'O') ? 2 : 0;
        return;
    }
    if (escState == 2) {
        if (c >= '0' && c <= '9') {
            return; // sequence parameters, e.g. ESC [ 3 ~
        }
        escState = 0;
        if (c == 'A') {
            showHistory(historyPos + 1);
        } else if (c == 'B') {
            showHistory(historyPos - 1);
        }
        return;
    }

    bool afterCr = lastWasCr;
    lastWasCr = (c == '\r');

    switch (c) {
    case '\r':
        submitLine();
        return;
    case '\n':
        if (!afterCr) { // CR LF counts as one Enter
            submitLine();
        }
        return;
    case 0x1b:
        escState = 1;
        return;
    case 0x08:
    case 0x7f:
        if (lineLen > 0) {
            --lineLen;
            out.print("\b \b");
        }
        return;
    case 0x03: // Ctrl+C – discard the line
        lineLen = 0;
        historyPos = -1;
        out.print("^C\r\n");
        prompt();
        return;
    case 0x04: // Ctrl+D on an empty line – end of the session
        if (sessionCanClose && lineLen == 0) {
            out.print("\r\n");
            closing = true;
        }
        return;
    default:
        break;
    }

    if (c >= 0x20 && c < 0x7f && lineLen < LINE_SIZE - 1) {
        line[lineLen++] = c;
        out.write(&c, 1);
    }
}

void LineEditor::showHistory(int index)
{
    if (index < -1 || index >= historyCount) {
        return;
    }
    historyPos = index;
    if (index == -1) {
        lineLen = 0;
    } else {
        int slot = (historyHead - 1 - index + HISTORY_SIZE) % HISTORY_SIZE;
        lineLen = strlcpy(line, history[slot], LINE_SIZE);
    }
    redrawLine();
}

void LineEditor::submitLine()
{
    out.print("\r\n");
    line[lineLen] = '\0';

    if (lineLen > 0) {
        int last = (historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        if (historyCount == 0 || strcmp(history[last], line) != 0) {
            strlcpy(history[historyHead], line, LINE_SIZE);
            historyHead = (historyHead + 1) % HISTORY_SIZE;
            if (historyCount < HISTORY_SIZE) {
                ++historyCount;
            }
        }

        // "exit" ends a Telnet session; on COM it goes to the executor (which answers that it does not apply).
        const char* p = line;
        while (*p == ' ') {
            ++p;
        }
        if (sessionCanClose && (strcmp(p, "exit") == 0 || strcmp(p, "quit") == 0)) {
            closing = true;
            return;
        }
        executor.execute(line, out);
    }

    lineLen = 0;
    historyPos = -1;
    prompt();
}
