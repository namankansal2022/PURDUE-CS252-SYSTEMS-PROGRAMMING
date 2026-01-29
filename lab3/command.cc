#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "command.hh"
#include "shell.hh"

extern "C" void tty_reset(void);

extern FILE *yyin;
extern int yyparse(void);
extern char **environ;

Command::Command() {
    _simpleCommands = std::vector<SimpleCommand *>();
    _outFile = NULL;
    _inFile = NULL;
    _errFile = NULL;
    _background = false;
    _isSubshell = false;
    _appendOut = false;
    _appendErr = false;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand) {
    _simpleCommands.push_back(simpleCommand);
}

void Command::clear() {
    for (auto simpleCommand : _simpleCommands) {
        delete simpleCommand;
    }
    _simpleCommands.clear();

    if (_outFile) delete _outFile;
    _outFile = NULL;

    if (_inFile) delete _inFile;
    _inFile = NULL;

    if (_errFile) delete _errFile;
    _errFile = NULL;

    _background = false;
    _isSubshell = false;
    _appendOut = false;
    _appendErr = false;
}

void Command::print() {
    if (getenv("SUBSHELL") || !isatty(0)) {
        return;
    }
    
    printf("\n\n");
    printf("              COMMAND TABLE                \n\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    int i = 0;
    for (auto &simpleCommand : _simpleCommands) {
        printf("  %-3d ", i++);
        simpleCommand->print();
    }

    printf("\n\n");
    printf("  Output       Input        Error        Background\n");
    printf("  ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s\n",
           _outFile ? _outFile->c_str() : "default",
           _inFile ? _inFile->c_str() : "default",
           _errFile ? _errFile->c_str() : "default",
           _background ? "YES" : "NO");
    printf("\n\n");
}

void Command::execute() {
    if (_simpleCommands.empty() && !_isSubshell) {
        Shell::prompt();
        return;
    }

    print();

    if (_isSubshell) {
        pid_t pid = fork();
        if (pid == 0) {
            yyparse();
            _exit(0);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
        clear();
        Shell::prompt();
        return;
    }

    const char *cmd = _simpleCommands[0]->_arguments[0]->c_str();
    if (strcmp(cmd, "exit") == 0) {
        if (!getenv("SUBSHELL")) {
            printf("Good bye!!\n");
        }
        tty_reset();
        exit(0);
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *path = nullptr;
        if (_simpleCommands[0]->_arguments.size() > 1)
            path = _simpleCommands[0]->_arguments[1]->c_str();
        else
            path = getenv("HOME");
        
        int saved_stderr = dup(2);
        if (_errFile) {
            int flags = O_CREAT | O_WRONLY;
            if (_appendErr) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }
            int fderr = open(_errFile->c_str(), flags, 0666);
            if (fderr >= 0) {
                dup2(fderr, 2);
                close(fderr);
            }
        }
        
        if (chdir(path) == -1) {
            fprintf(stderr, "cd: can't cd to %s\n", path);
        }
        
        dup2(saved_stderr, 2);
        close(saved_stderr);
        
        clear();
        Shell::prompt();
        return;
    }

    if (strcmp(cmd, "setenv") == 0) {
        if (_simpleCommands[0]->_arguments.size() != 3) {
            fprintf(stderr, "Usage: setenv VAR VALUE\n");
        } else {
            setenv(_simpleCommands[0]->_arguments[1]->c_str(),
                   _simpleCommands[0]->_arguments[2]->c_str(), 1);
        }
        clear();
        Shell::prompt();
        return;
    }

    if (strcmp(cmd, "unsetenv") == 0) {
        if (_simpleCommands[0]->_arguments.size() != 2) {
            fprintf(stderr, "Usage: unsetenv VAR\n");
        } else {
            unsetenv(_simpleCommands[0]->_arguments[1]->c_str());
        }
        clear();
        Shell::prompt();
        return;
    }

    if (strcmp(cmd, "source") == 0) {
        if (_simpleCommands[0]->_arguments.size() != 2) {
            fprintf(stderr, "Usage: source filename\n");
            clear();
            Shell::prompt();
            return;
        }
        FILE *f = fopen(_simpleCommands[0]->_arguments[1]->c_str(), "r");
        if (!f) {
            perror("source");
            clear();
            Shell::prompt();
            return;
        }
        FILE *prev = yyin;
        yyin = f;
        yyparse();
        yyin = prev;
        fclose(f);
        clear();
        Shell::prompt();
        return;
    }

    if (strcmp(cmd, "printenv") == 0) {
        char **env = environ;
        while (*env) {
            printf("%s\n", *env);
            env++;
        }
        clear();
        Shell::prompt();
        return;
    }

    int tmpin  = dup(0);
    int tmpout = dup(1);
    int tmperr = dup(2);

    int fdin;
    if (_inFile) {
        fdin = open(_inFile->c_str(), O_RDONLY);
        if (fdin < 0) {
            perror("open input file");
            dup2(tmpin, 0);
            dup2(tmpout, 1);
            dup2(tmperr, 2);
            close(tmpin);
            close(tmpout);
            close(tmperr);
            clear();
            Shell::prompt();
            return;
        }
    } else {
        fdin = dup(tmpin);
    }

    int ret = 0;
    int fdout;

    for (size_t i = 0; i < _simpleCommands.size(); i++) {
        dup2(fdin, 0);
        close(fdin);

        if (i == _simpleCommands.size() - 1) {
            if (_outFile) {
                int flags = O_CREAT | O_WRONLY;
                if (_appendOut) {
                    flags |= O_APPEND;
                } else {
                    flags |= O_TRUNC;
                }
                fdout = open(_outFile->c_str(), flags, 0666);
                if (fdout < 0) {
                    perror("open output file");
                    dup2(tmpin, 0);
                    dup2(tmpout, 1);
                    dup2(tmperr, 2);
                    close(tmpin);
                    close(tmpout);
                    close(tmperr);
                    clear();
                    Shell::prompt();
                    return;
                }
            } else {
                fdout = dup(tmpout);
            }
        } else {
            int fdpipe[2];
            if (pipe(fdpipe) == -1) {
                perror("pipe");
                dup2(tmpin, 0);
                dup2(tmpout, 1);
                dup2(tmperr, 2);
                close(tmpin);
                close(tmpout);
                close(tmperr);
                clear();
                Shell::prompt();
                return;
            }
            fdout = fdpipe[1];
            fdin  = fdpipe[0];
        }

        dup2(fdout, 1);
        close(fdout);

        if (i == _simpleCommands.size() - 1 && _errFile) {
            int flags = O_CREAT | O_WRONLY;
            if (_appendErr) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }
            int fderr = open(_errFile->c_str(), flags, 0666);
            if (fderr < 0) {
                perror("open error file");
            } else {
                dup2(fderr, 2);
                close(fderr);
            }
        }

        SimpleCommand *simpleCmd = _simpleCommands[i];
        std::vector<char *> argv;
        for (auto &argPtr : simpleCmd->_arguments)
            argv.push_back((char *)argPtr->c_str());
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execvp(argv[0], argv.data());
            perror("execvp");
            _exit(1);
        } else if (pid < 0) {
            perror("fork");
            continue;
        }

        ret = pid;
    }

    if (!_background) {
        int status;
        waitpid(ret, &status, 0);
        if (WIFEXITED(status)) {
            Command::_lastReturnCode = WEXITSTATUS(status);
        } else {
            Command::_lastReturnCode = 1;
        }
    } else {
        Command::_backgroundPids.insert(ret);
        Command::_lastBackgroundPid = ret;
    }

    dup2(tmpin, 0);
    dup2(tmpout, 1);
    dup2(tmperr, 2);
    close(tmpin);
    close(tmpout);
    close(tmperr);

    const char *cleanup_list = getenv("PROCSUB_CLEANUP");
    if (cleanup_list) {
        char *list_copy = strdup(cleanup_list);
        char *token = strtok(list_copy, ",");
        
        while (token) {
            char fifo[1024], dir[1024];
            int pid;
            if (sscanf(token, "%[^:]:%[^:]:%d", fifo, dir, &pid) == 3) {
                int status;
                waitpid(pid, &status, 0);
                unlink(fifo);
                rmdir(dir);
            }
            token = strtok(NULL, ",");
        }
        
        free(list_copy);
        unsetenv("PROCSUB_CLEANUP");
    }

    if (!_simpleCommands.empty() && !_simpleCommands.back()->_arguments.empty()) {
        Command::_lastArgument = *(_simpleCommands.back()->_arguments.back());
    }

    clear();
    Shell::prompt();
}

SimpleCommand *Command::_currentSimpleCommand;
std::set<pid_t> Command::_backgroundPids;
int Command::_lastReturnCode = 0;
pid_t Command::_lastBackgroundPid = 0;
std::string Command::_lastArgument = "";
std::string Command::_shellPath = "";