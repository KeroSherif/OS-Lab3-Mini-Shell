#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>
#include <vector>
#include <string>
#include <iostream>
#include "command.h"
#include "tokenizer.h"

// Helpers
static int logFd = -1;

static void freeSimpleCommand(SimpleCommand *sc)
{
    for (int j = 0; j < sc->_numberOfArguments; j++)
        free(sc->_arguments[j]);
    free(sc->_arguments);
    delete sc;
}

static void addWord(SimpleCommand *sc, const std::string &value, bool quoted)
{
    bool isProgramName = (sc->_numberOfArguments == 0);

    if (!quoted && !isProgramName && strpbrk(value.c_str(), "*?"))
    {
        glob_t g;
        memset(&g, 0, sizeof g);

        if (glob(value.c_str(), GLOB_NOCHECK, NULL, &g) == 0)
        {
            for (size_t k = 0; k < g.gl_pathc; k++)
                sc->insertArgument(strdup(g.gl_pathv[k]));
            globfree(&g);
            return;
        }
        globfree(&g);
    }

    sc->insertArgument(strdup(value.c_str()));
}

static size_t putStr(char *buf, const char *s)
{
    size_t k = 0;
    while (s[k]) { buf[k] = s[k]; k++; }
    return k;
}

static size_t putNum(char *buf, long v)
{
    char tmp[24];
    int len = 0;
    bool neg = v < 0;
    unsigned long u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
    do { tmp[len++] = '0' + (u % 10); u /= 10; } while (u);
    size_t k = 0;
    if (neg) buf[k++] = '-';
    while (len) buf[k++] = tmp[--len];
    return k;
}

// Child log (SIGCHLD)
static void logChild(pid_t pid, int status)
{
    if (logFd < 0)
        return;

    char buf[128];
    size_t n = 0;
    n += putStr(buf + n, "Child process ");
    n += putNum(buf + n, pid);
    if (WIFEXITED(status))
    {
        n += putStr(buf + n, " terminated, exit status ");
        n += putNum(buf + n, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        n += putStr(buf + n, " killed by signal ");
        n += putNum(buf + n, WTERMSIG(status));
    }
    else
    {
        n += putStr(buf + n, " terminated");
    }
    buf[n++] = '\n';
    (void)!write(logFd, buf, n);
}

static void onChildExit(int)
{
    int saved = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        logChild(pid, status);

    errno = saved;
}

// Parse
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

    Command &cmd = Command::_currentCommand;
    SimpleCommand *sc = new SimpleCommand();

    auto fail = [&](const char *msg)
    {
        fprintf(stderr, "myshell: %s\n", msg);
        freeSimpleCommand(sc);
        cmd.clear();
    };

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
            i++;
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

            cmd._outFile = strdup(f->value.c_str());
            cmd._errFile = strdup(f->value.c_str());
            cmd._out_error = 1;
            cmd._append = 1;
            i++;
            break;
        }

        case TOKEN_BACKGROUND:
            if (tokens[i + 1].type != TOKEN_EOF)
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

    if (sc->_numberOfArguments > 0)
    {
        cmd.insertSimpleCommand(sc);
    }
    else
    {
        bool emptyLine = cmd._numberOfSimpleCommands == 0 && !cmd._outFile &&
                         !cmd._inputFile && !cmd._errFile && !cmd._background;
        if (emptyLine)
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

// Command table
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
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;

    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _out_error = 0;
    _append = 0;
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
        printf("\n");
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

// Execute
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

    
    int infd = -1, outfd = -1, errfd = -1;

    auto closeRedirects = [&]()
    {
        if (infd >= 0) close(infd);
        if (outfd >= 0) close(outfd);
        if (errfd >= 0 && errfd != outfd) close(errfd);
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
            errfd = outfd;
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

    
    pid_t lastPid = -1;
    for (int i = 0; i < n; i++)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            perror("myshell: fork");
            break;
        }

        if (pid == 0)
        {

            signal(SIGINT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (i == 0 && infd >= 0)
                dup2(infd, 0);
            else if (i > 0)
                dup2(pipeFds[2 * (i - 1)], 0);

            if (i == n - 1 && outfd >= 0)
                dup2(outfd, 1);
            else if (i < n - 1)
                dup2(pipeFds[2 * i + 1], 1);

            if (errfd >= 0)
                dup2(errfd, 2);

            for (int fd : pipeFds)
                close(fd);
            closeRedirects();

            execvp(_simpleCommands[i]->_arguments[0], _simpleCommands[i]->_arguments);

            fprintf(stderr, "myshell: %s: %s\n", _simpleCommands[i]->_arguments[0], strerror(errno));
            _exit(127);
        }

        lastPid = pid;
    }

    for (int fd : pipeFds)
        close(fd);
    closeRedirects();

    if (!_background && lastPid > 0)
    {
        int status;
        pid_t r;

        while ((r = waitpid(lastPid, &status, 0)) < 0 && errno == EINTR)
            ;
        if (r == lastPid)
            logChild(r, status);
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

// Main
int main()
{

    signal(SIGINT, SIG_IGN);

    logFd = open("myshell.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (logFd < 0)
        perror("myshell: cannot open myshell.log");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onChildExit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    std::string input;
    while (true)
    {
        Command::_currentCommand.prompt();
        if (!std::getline(std::cin, input))
        {
            printf("\nGood bye!!\n");
            break;
        }
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
    return 0;
}
