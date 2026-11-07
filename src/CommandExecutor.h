/**
 * @file CommandExecutor.h
 * @brief Command interpreter shared by the COM console, Telnet and the web panel terminal.
 */
#pragma once

#include <vector>

#include "IOutput.h"
#include "i18n/I18n.h"

/// Result of a command handler.
enum class CmdResult {
    OK,
    ERR_ARGS,    ///< Wrong arguments – the executor prints the command syntax.
    ERR_UNKNOWN, ///< Unknown command.
    ERR_FAIL,    ///< The command failed (the handler prints the reason itself).
};

/// Command handler: argv[0] is the command name (as typed, may be abbreviated), argv[1..argc-1] the arguments.
using CommandHandler = CmdResult (*)(void* ctx, int argc, char** argv, IOutput& out);

/// Command definition registered with CommandExecutor::add().
struct CommandDef {
    const char* name;  ///< Command name.
    const char* args;  ///< Argument description for the syntax, e.g. "<mac> <ip> [nazwa]".
    i18n::Text help;   ///< { "po polsku", "in English" }.
    CommandHandler fn; ///< Handler.
    void* ctx;         ///< Usually a pointer to the module (this).
};

/**
 * @brief Command interpreter as a tree: groups (wifi, dhcp, nat…) -> subcommands.
 *
 * The children of each node are sorted; lookup is a binary search and unambiguous abbreviations
 * are accepted (e.g. "dh le" -> "dhcp leases").
 *
 * Registration only at start-up, before the consoles start. Afterwards the tree is only read,
 * so execute() may be called from many tasks at the same time without a mutex.
 */
class CommandExecutor {
public:
    static constexpr int MAX_ARGS = 16; ///< Maximum number of tokens in a line.

    /// Registers the built-in "help" command.
    CommandExecutor();

    /// @brief Adds a group. @param groupPath Group name; for nested groups a path with spaces, e.g. "wifi ap".
    void addGroup(const char* groupPath, i18n::Text help);
    /// @brief Adds a command. @param groupPath nullptr -> top-level command.
    void add(const char* groupPath, const CommandDef& def);

    /**
     * @brief Parses the line in place (the buffer is modified) and executes the command.
     *
     * Prints the syntax on ERR_ARGS, a hint on an unknown command, the subcommand list for a bare group.
     */
    CmdResult execute(char* line, IOutput& out) const;

private:
    /// Tree node: group or command.
    struct Node {
        const char* name = nullptr;  ///< Name.
        i18n::Text help;             ///< Description.
        CommandDef def = {};         ///< def.fn == nullptr -> group.
        std::vector<Node> children;  ///< Sorted by name.

        /// @return true for a group.
        bool isGroup() const { return def.fn == nullptr; }
    };

    /// Result of lookup().
    enum class Match { FOUND, NOT_FOUND, AMBIGUOUS };

    /// Group node by path (nullptr = root).
    Node* findGroup(const char* groupPath);
    /// Inserts a child keeping the order (logs a duplicate).
    static Node& insertSorted(Node& parent, Node&& child);
    /// Child by exact name or unambiguous prefix.
    static Match lookup(const Node& parent, const char* token, const Node*& found);
    /// Splits a line into tokens in place; quotes keep spaces inside an argument. @return Number of tokens or -1 when too many.
    static int tokenize(char* line, char** argv, int maxArgs);

    /// Prints the children of a group with their arguments and descriptions.
    static void printList(const Node& group, IOutput& out);
    /// Prints "Usage: <path> <args>" and the description.
    static void printUsage(const Node* const* path, int depth, IOutput& out);
    /// Prints the commands matching an ambiguous prefix.
    static void printAmbiguous(const Node& parent, const char* token, IOutput& out);

    /// "help [command]" handler (ctx = this).
    static CmdResult cmdHelp(void* ctx, int argc, char** argv, IOutput& out);

    Node root; ///< Root of the command tree.
};
