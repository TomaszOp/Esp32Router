/**
 * @file CommandExecutor.cpp
 * @brief CommandExecutor: command tree, abbreviations, help and usage.
 */
#include "CommandExecutor.h"

#include "i18n/I18n.h"

#include <algorithm>
#include <cstring>

#define LOG_TAG     "CommandExecutor"
#define LOG_ENABLED 1
#include "Log.h"

static constexpr int MAX_DEPTH = 4;

// Whether text starts with prefix.
static bool startsWith(const char* text, const char* prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

CommandExecutor::CommandExecutor()
{
    add(nullptr, { "help", "[polecenie]", { "lista poleceń lub opis polecenia/grupy", "list of commands or description of a command/group" }, &cmdHelp, this });
}

CommandExecutor::Node& CommandExecutor::insertSorted(Node& parent, Node&& child)
{
    auto& v = parent.children;
    auto it = std::lower_bound(v.begin(), v.end(), child.name,
                               [](const Node& n, const char* name) { return strcmp(n.name, name) < 0; });
    if (it != v.end() && strcmp(it->name, child.name) == 0) {
        LOG_E("Polecenie '%s' juz istnieje", child.name);
        return *it;
    }
    return *v.insert(it, std::move(child));
}

CommandExecutor::Node* CommandExecutor::findGroup(const char* groupPath)
{
    if (groupPath == nullptr) {
        return &root;
    }

    char buf[64];
    strlcpy(buf, groupPath, sizeof(buf));
    char* argv[MAX_DEPTH];
    int n = tokenize(buf, argv, MAX_DEPTH);

    Node* node = &root;
    for (int i = 0; i < n; ++i) {
        Node* next = nullptr;
        for (Node& c : node->children) {
            if (strcmp(c.name, argv[i]) == 0) {
                next = &c;
                break;
            }
        }
        if (next == nullptr || !next->isGroup()) {
            return nullptr;
        }
        node = next;
    }
    return node;
}

void CommandExecutor::addGroup(const char* groupPath, i18n::Text help)
{
    // The last path segment is the name of the new group, the rest is the parent group.
    const char* lastSpace = strrchr(groupPath, ' ');
    Node* parent = &root;
    const char* name = groupPath;
    if (lastSpace != nullptr) {
        char parentPath[64];
        size_t len = std::min(static_cast<size_t>(lastSpace - groupPath), sizeof(parentPath) - 1);
        memcpy(parentPath, groupPath, len);
        parentPath[len] = '\0';
        parent = findGroup(parentPath);
        name = lastSpace + 1;
    }
    if (parent == nullptr) {
        LOG_E("Brak grupy nadrzednej dla '%s'", groupPath);
        return;
    }

    Node group;
    group.name = name;
    group.help = help;
    insertSorted(*parent, std::move(group));
}

void CommandExecutor::add(const char* groupPath, const CommandDef& def)
{
    Node* parent = findGroup(groupPath);
    if (parent == nullptr) {
        LOG_E("Brak grupy '%s' dla polecenia '%s'", groupPath, def.name);
        return;
    }

    Node cmd;
    cmd.name = def.name;
    cmd.help = def.help;
    cmd.def = def;
    insertSorted(*parent, std::move(cmd));
}

// The exact name wins; otherwise an unambiguous prefix.
CommandExecutor::Match CommandExecutor::lookup(const Node& parent, const char* token, const Node*& found)
{
    const auto& v = parent.children;
    auto it = std::lower_bound(v.begin(), v.end(), token,
                               [](const Node& n, const char* t) { return strcmp(n.name, t) < 0; });
    if (it == v.end() || !startsWith(it->name, token)) {
        return Match::NOT_FOUND;
    }
    if (strcmp(it->name, token) == 0) {
        found = &*it;
        return Match::FOUND;
    }
    auto next = it + 1;
    if (next != v.end() && startsWith(next->name, token)) {
        return Match::AMBIGUOUS;
    }
    found = &*it;
    return Match::FOUND;
}

// Splits a line into tokens in place. Quotes enclose an argument with spaces ("Moja siec").
int CommandExecutor::tokenize(char* line, char** argv, int maxArgs)
{
    int argc = 0;
    char* p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (argc == maxArgs) {
            return -1;
        }
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p != '\0' && *p != '"') {
                ++p;
            }
        } else {
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t') {
                ++p;
            }
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return argc;
}

CmdResult CommandExecutor::execute(char* line, IOutput& out) const
{
    char* argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);
    if (argc < 0) {
        out.printf(tr("Za dużo argumentów (max %d)\r\n", "Too many arguments (max %d)\r\n"), MAX_ARGS);
        return CmdResult::ERR_ARGS;
    }
    if (argc == 0) {
        return CmdResult::OK;
    }

    const Node* path[MAX_DEPTH];
    int depth = 0;
    const Node* node = &root;
    int i = 0;

    while (node->isGroup() && i < argc) {
        if (strcmp(argv[i], "?") == 0) {
            printList(*node, out);
            return CmdResult::OK;
        }
        const Node* child = nullptr;
        Match m = lookup(*node, argv[i], child);
        if (m == Match::AMBIGUOUS) {
            printAmbiguous(*node, argv[i], out);
            return CmdResult::ERR_UNKNOWN;
        }
        if (m == Match::NOT_FOUND) {
            if (node == &root) {
                out.printf(tr("Nieznane polecenie '%s'. Wpisz 'help'.\r\n", "Unknown command '%s'. Type 'help'.\r\n"), argv[i]);
            } else {
                out.printf(tr("Nieznane podpolecenie '%s' w '%s'. Dostępne:\r\n", "Unknown subcommand '%s' in '%s'. Available:\r\n"), argv[i],
                           node->name);
                printList(*node, out);
            }
            return CmdResult::ERR_UNKNOWN;
        }
        if (depth == MAX_DEPTH) {
            return CmdResult::ERR_UNKNOWN;
        }
        path[depth++] = child;
        node = child;
        ++i;
    }

    if (node->isGroup()) {
        // A bare group name -> list of subcommands.
        printList(*node, out);
        return CmdResult::OK;
    }

    if (i < argc && strcmp(argv[i], "?") == 0) {
        printUsage(path, depth, out);
        return CmdResult::OK;
    }

    // argv[0] for the handler is the command name.
    CmdResult r = node->def.fn(node->def.ctx, argc - i + 1, argv + i - 1, out);
    if (r == CmdResult::ERR_ARGS) {
        printUsage(path, depth, out);
    }
    return r;
}

void CommandExecutor::printList(const Node& group, IOutput& out)
{
    for (const Node& c : group.children) {
        if (c.isGroup()) {
            out.printf("  %-12s ...                  %s\r\n", c.name, c.help ? c.help.get() : "");
        } else {
            out.printf("  %-12s %-20s %s\r\n", c.name, c.def.args ? c.def.args : "", c.help ? c.help.get() : "");
        }
    }
}

void CommandExecutor::printUsage(const Node* const* path, int depth, IOutput& out)
{
    out.print(tr("Użycie:", "Usage:"));
    for (int i = 0; i < depth; ++i) {
        out.printf(" %s", path[i]->name);
    }
    const Node* cmd = path[depth - 1];
    if (cmd->def.args != nullptr && cmd->def.args[0] != '\0') {
        out.printf(" %s", cmd->def.args);
    }
    out.printf("\r\n  %s\r\n", cmd->help ? cmd->help.get() : "");
}

void CommandExecutor::printAmbiguous(const Node& parent, const char* token, IOutput& out)
{
    out.printf(tr("Niejednoznaczne '%s':", "Ambiguous '%s':"), token);
    for (const Node& c : parent.children) {
        if (startsWith(c.name, token)) {
            out.printf(" %s", c.name);
        }
    }
    out.print("\r\n");
}

CmdResult CommandExecutor::cmdHelp(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<CommandExecutor*>(ctx);
    if (argc == 1) {
        out.print(tr("Polecenia ('<grupa>' lub '<grupa> ?' pokazuje podpolecenia, dozwolone skróty):\r\n",
                     "Commands ('<group>' or '<group> ?' shows subcommands, abbreviations allowed):\r\n"));
        printList(self->root, out);
        return CmdResult::OK;
    }

    const Node* path[MAX_DEPTH];
    int depth = 0;
    const Node* node = &self->root;
    for (int i = 1; i < argc && node->isGroup() && depth < MAX_DEPTH; ++i) {
        const Node* child = nullptr;
        Match m = lookup(*node, argv[i], child);
        if (m == Match::AMBIGUOUS) {
            printAmbiguous(*node, argv[i], out);
            return CmdResult::ERR_UNKNOWN;
        }
        if (m == Match::NOT_FOUND) {
            out.printf(tr("Nieznane polecenie '%s'\r\n", "Unknown command '%s'\r\n"), argv[i]);
            return CmdResult::ERR_UNKNOWN;
        }
        path[depth++] = child;
        node = child;
    }

    if (node->isGroup()) {
        out.printf("%s\r\n", node->help ? node->help.get() : "");
        printList(*node, out);
    } else {
        printUsage(path, depth, out);
    }
    return CmdResult::OK;
}
