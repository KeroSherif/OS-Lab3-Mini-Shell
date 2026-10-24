
/*
 * CC373 - Lab 3 : Mini shell
 *
 * Layout of this file:
 *   1. helpers            (freeing, wildcard expansion, SIGCHLD log handler)
 *   2. parse()            tokens  -> Command table        (Part 1)
 *   3. Command methods    table   -> real processes       (Parts 2 + 3)
 *   4. main()             signals + read/parse/execute loop
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <glob.h>
#include <wordexp.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include "command.h"
#include "tokenizer.h"

/* ------------------------------------------------------------------ */
/* 1. helpers                                                          */
/* ------------------------------------------------------------------ */

// SimpleCommand is created with `new`, so it must be released with `delete`
// (the original clear() used free(), which is wrong for new-allocated memory).
static void freeSimpleCommand(SimpleCommand *sc)
{
    for (int j = 0; j < sc->_numberOfArguments; j++)
        free(sc->_arguments[j]);
    free(sc->_arguments);
    delete sc;
}

// Insert one word into a SimpleCommand.
static void addWord(SimpleCommand *sc, const std::string &value, bool quoted)
{
    (void)quoted; // used later by the wildcard (bonus) commit
    // strdup: clear() calls free() on every argument, so they must be heap copies
    sc->insertArgument(strdup(value.c_str()));
}

/* ------------------------------------------------------------------ */
/* 2. parse()  : tokens -> Command table                               */
/* ------------------------------------------------------------------ */

// The given tokenizer splits on spaces only, so  echo "Hello, World!"  arrives as
// two tokens:  "Hello,   and   World!"   (quotes still attached).
// Here we glue them back into ONE word and strip the quotes. Words that were
// quoted are remembered so that wildcards are not expanded inside them.
struct Item
{
    TokenType type;
    std::string value;
    bool quoted;
};

static bool mergeQuotes(const std::vector<Token> &raw, std::vector<Item> &out)
{
    for (size_t i = 0; i < raw.size(); i++)
    {
        const Token &t = raw[i];
        char q = t.value.empty() ? 0 : t.value[0];

        if (t.type != TOKEN_EOF && (q == '"' || q == '\''))
        {
            std::string text = t.value;
            // the span is closed when the accumulated text ends with the same quote
            while (!(text.size() >= 2 && text.back() == q))
            {
                if (++i >= raw.size() || raw[i].type == TOKEN_EOF)
                {
                    fprintf(stderr, "myshell: syntax error: unterminated quote\n");
                    return false;
                }
                text += " " + raw[i].value;
            }
            out.push_back({TOKEN_ARGUMENT, text.substr(1, text.size() - 2), true});
        }
        else
        {
            out.push_back({t.type, t.value, false});
        }
    }
    return true;
}

void parse(std::vector<Token> &rawTokens)
{
    std::vector<Item> tokens;
    if (!mergeQuotes(rawTokens, tokens))
        return;

    Command &cmd = Command::_currentCommand; // the real table (not a local copy)
    SimpleCommand *sc = new SimpleCommand(); // simple command being built

    // On any syntax error: print the message, throw away the half-built
    // table and go back to the prompt without executing anything.
    auto fail = [&](const char *msg)
    {
        fprintf(stderr, "myshell: %s\n", msg);
        freeSimpleCommand(sc);
        cmd.clear();
    };

    // The word after > >> < 2> >>&  is a file name. Returns NULL (after
    // reporting the error) if there is none, e.g.  "ls >"  or  "ls > | wc".
    auto fileAfter = [&](size_t i, const char *op) -> const Item *
    {
        if (i + 1 >= tokens.size() ||
            (tokens[i + 1].type != TOKEN_COMMAND && tokens[i + 1].type != TOKEN_ARGUMENT))
        {
            char msg[96];
            snprintf(msg, sizeof msg, "syntax error: missing file name after '%s'", op);
            fail(msg);
            return NULL;
        }
        return &tokens[i + 1];
    };

    for (size_t i = 0; i < tokens.size(); i++)
    {
        const Item &t = tokens[i];
        if (t.type == TOKEN_EOF)
            break;

        switch (t.type)
        {
        case TOKEN_COMMAND:
        case TOKEN_ARGUMENT:
            addWord(sc, t.value, t.quoted);
            break;

        case TOKEN_PIPE:
            if (sc->_numberOfArguments == 0)
            {
                fail("syntax error: missing command before '|'");
                return;
            }
            if (cmd._outFile)
            {
                fail("ambiguous output redirect: '>' is only allowed on the last command of a pipe");
                return;
            }
            cmd.insertSimpleCommand(sc);
            sc = new SimpleCommand();
            break;

        case TOKEN_REDIRECT:
        case TOKEN_APPEND:
        {
            const Item *f = fileAfter(i, t.type == TOKEN_REDIRECT ? ">" : ">>");
            if (!f) return;
            if (cmd._outFile)
            {
                fail("ambiguous output redirect");
                return;
            }
            cmd._outFile = strdup(f->value.c_str());
            cmd._append = (t.type == TOKEN_APPEND);
            i++; // the file name is consumed
            break;
        }

        case TOKEN_INPUT:
        {
            const Item *f = fileAfter(i, "<");
            if (!f) return;
            if (cmd._inputFile)
            {
                fail("ambiguous input redirect");
                return;
            }
            if (cmd._numberOfSimpleCommands > 0)
            {
                fail("ambiguous input redirect: '<' is only allowed on the first command of a pipe");
                return;
            }
            cmd._inputFile = strdup(f->value.c_str());
            i++;
            break;
        }

        case TOKEN_ERROR:
        {
            const Item *f = fileAfter(i, "2>");
            if (!f) return;
            if (cmd._errFile)
            {
                fail("ambiguous error redirect");
                return;
            }
            cmd._errFile = strdup(f->value.c_str());
            i++;
            break;
        }

        case TOKEN_REDIRECT_AND_ERROR:
        {
            const Item *f = fileAfter(i, ">>&");
            if (!f) return;
            if (cmd._outFile || cmd._errFile)
            {
                fail("ambiguous output redirect");
                return;
            }
            // stdout AND stderr go (appended) to the same file
            cmd._outFile = strdup(f->value.c_str());
            cmd._errFile = strdup(f->value.c_str());
            cmd._out_error = 1;
            cmd._append = 1;
            i++;
            break;
        }

        case TOKEN_BACKGROUND:
            if (tokens[i + 1].type != TOKEN_EOF) // i+1 always exists: EOF is the last token
            {
                fail("syntax error: '&' must be the last token on the line");
                return;
            }
            cmd._background = 1;
            break;

        default:
            break;
        }
    }

    // End of line: store the last simple command (if it has any words)
    if (sc->_numberOfArguments > 0)
    {
        cmd.insertSimpleCommand(sc);
    }
    else
    {
        bool emptyLine = cmd._numberOfSimpleCommands == 0 && !cmd._outFile &&
                         !cmd._inputFile && !cmd._errFile && !cmd._background;
        if (emptyLine) // user just pressed Enter
        {
            freeSimpleCommand(sc);
            return;
        }
        fail(cmd._numberOfSimpleCommands > 0 ? "syntax error: missing command after '|'"
                                             : "syntax error: missing command");
        return;
    }

    cmd.execute();
}

/* ------------------------------------------------------------------ */
/* 3. Command / SimpleCommand                                          */
/* ------------------------------------------------------------------ */

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
}

void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfAvailableArguments == _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments,
                                      _numberOfAvailableArguments * sizeof(char *));
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL; // execvp needs a NULL-terminated array

    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    // BUG FIX: the original used _numberOfSimpleCommands here, which was not
    // initialised yet (garbage size).
    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _out_error = 0; // BUG FIX: was never initialised
    _append = 0;    // BUG FIX: was never initialised
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands,
                                                    _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}

void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; i++)
        freeSimpleCommand(_simpleCommands[i]);

    if (_outFile)
        free(_outFile);
    if (_inputFile)
        free(_inputFile);
    if (_errFile)
        free(_errFile);

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _out_error = 0;
    _append = 0;
}

void Command::print()
{
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }
        printf("\n"); // one row per simple command (so pipes are readable)
    }

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default", _errFile ? _errFile : "default",
           _out_error == 1 ? _errFile : "default",
           _background ? "YES" : "NO");
    printf("\n\n");
}

// cd [dir] : runs inside the shell process (a child's chdir would be lost)
static void builtinCd(SimpleCommand *sc)
{
    if (sc->_numberOfArguments > 2)
    {
        fprintf(stderr, "cd: too many arguments\n");
        return;
    }

    const char *dir = (sc->_numberOfArguments == 2) ? sc->_arguments[1] : getenv("HOME");
    if (dir && strcmp(dir, "~") == 0)
        dir = getenv("HOME");

    if (!dir)
    {
        fprintf(stderr, "cd: HOME is not set\n");
        return;
    }
    if (chdir(dir) != 0)
        fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
}

void Command::execute()
{
#ifndef NO_TABLE
    print();
#endif

    int n = _numberOfSimpleCommands;
    if (n == 0)
    {
        clear();
        return;
    }

    /* ---- built-ins: executed by the shell itself, NO fork ---- */
    if (n == 1)
    {
        char **args = _simpleCommands[0]->_arguments;
        if (strcmp(args[0], "exit") == 0)
        {
            printf("Good bye!!\n");
            clear();
            exit(0);
        }
        if (strcmp(args[0], "cd") == 0)
        {
            builtinCd(_simpleCommands[0]);
            clear();
            return;
        }
    }

    /* ---- open the redirection files (in the parent, so that an error
          such as "no such file" is reported once and nothing is started) ---- */
    int infd = -1, outfd = -1, errfd = -1;

    auto closeRedirects = [&]()
    {
        if (infd >= 0) close(infd);
        if (outfd >= 0) close(outfd);
        if (errfd >= 0 && errfd != outfd) close(errfd); // >>& : same descriptor twice
        infd = outfd = errfd = -1;
    };

    if (_inputFile)
    {
        infd = open(_inputFile, O_RDONLY | O_CLOEXEC);
        if (infd < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _inputFile, strerror(errno));
            clear();
            return;
        }
    }
    if (_outFile)
    {
        int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (_append ? O_APPEND : O_TRUNC);
        outfd = open(_outFile, flags, 0644);
        if (outfd < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _outFile, strerror(errno));
            closeRedirects();
            clear();
            return;
        }
    }
    if (_errFile)
    {
        if (_out_error)
            errfd = outfd; // >>& : stderr shares the same open file as stdout
        else
        {
            errfd = open(_errFile, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (errfd < 0)
            {
                fprintf(stderr, "myshell: %s: %s\n", _errFile, strerror(errno));
                closeRedirects();
                clear();
                return;
            }
        }
    }

    /* ---- one pipe between every two neighbouring commands ---- */
    std::vector<int> pipeFds(2 * (n > 1 ? n - 1 : 0));
    for (int i = 0; i < n - 1; i++)
    {
        if (pipe(&pipeFds[2 * i]) < 0)
        {
            perror("myshell: pipe");
            for (int k = 0; k < 2 * i; k++)
                close(pipeFds[k]);
            closeRedirects();
            clear();
            return;
        }
    }

    /* ---- one child per simple command ---- */
    pid_t lastPid = -1;
    for (int i = 0; i < n; i++)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("myshell: fork");
            break;
        }

        if (pid == 0) // ---------------- child ----------------
        {
            // The shell ignores Ctrl-C, and "ignored" survives execvp, so the
            // child must go back to the default action or it could not be killed.
            signal(SIGINT, SIG_DFL);

            // stdin : first command <- file, the others <- previous pipe
            if (i == 0 && infd >= 0)
                dup2(infd, 0);
            else if (i > 0)
                dup2(pipeFds[2 * (i - 1)], 0);

            // stdout : last command -> file, the others -> next pipe
            if (i == n - 1 && outfd >= 0)
                dup2(outfd, 1);
            else if (i < n - 1)
                dup2(pipeFds[2 * i + 1], 1);

            // stderr : every command of the line
            if (errfd >= 0)
                dup2(errfd, 2);

            // Close every original descriptor. If one write end stayed open
            // somewhere, the reader (grep, wc ...) would never see EOF and hang.
            for (int fd : pipeFds)
                close(fd);
            closeRedirects();

            execvp(_simpleCommands[i]->_arguments[0], _simpleCommands[i]->_arguments);

            // only reached if execvp failed
            fprintf(stderr, "myshell: %s: %s\n", _simpleCommands[i]->_arguments[0], strerror(errno));
            _exit(127); // _exit, not exit: don't flush the parent's copied stdio buffers
        }

        lastPid = pid; // ---------------- parent ----------------
    }

    // The parent must close its copies too (same EOF reason as above).
    for (int fd : pipeFds)
        close(fd);
    closeRedirects();

    if (!_background && lastPid > 0)
    {
        int status;
        while (waitpid(lastPid, &status, 0) < 0 && errno == EINTR)
            ;
    }

    clear();
}

void Command::prompt()
{
    printf("myshell>");
    fflush(stdout);
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

/* ------------------------------------------------------------------ */
/* 4. main                                                             */
/* ------------------------------------------------------------------ */

int main()
{
    // Ctrl-C must not kill the shell
    signal(SIGINT, SIG_IGN);

    std::string input;
    while (true)
    {
        Command::_currentCommand.prompt();
        if (!std::getline(std::cin, input)) // Ctrl-D (EOF)
        {
            printf("\nGood bye!!\n");
            break;
        }
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
    return 0;
}