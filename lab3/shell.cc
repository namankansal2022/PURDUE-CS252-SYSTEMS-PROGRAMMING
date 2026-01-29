#include <cstdio>
#include <cstring>
#include "shell.hh"
#include "command.hh"
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

extern FILE *yyin;
int yyparse(void);

void ctrlCHandler(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
    if (isatty(0)) {
        const char *prompt = "myshell>";
        write(STDOUT_FILENO, prompt, 9);
    }
}

void sigchldHandler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    
    if (getenv("SUBSHELL")) {
        while (waitpid(-1, &status, WNOHANG) > 0) {
        }
        return;
    }
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (Command::_backgroundPids.count(pid) > 0) {
            char buffer[64];
            int len = snprintf(buffer, sizeof(buffer), "[%d] exited.\n", pid);
            write(STDOUT_FILENO, buffer, len);
            Command::_backgroundPids.erase(pid);
        }
    }
}

void Shell::prompt() {
    if (isatty(0)) {
        const char *prompt_str = getenv("PROMPT");
        if (!prompt_str) {
            prompt_str = "myshell>";
        }
        
        for (const char *p = prompt_str; *p; p++) {
            if (*p == '\\' && *(p + 1)) {
                p++;
                switch (*p) {
                    case 'u': {
                        const char *user = getenv("USER");
                        if (user) printf("%s", user);
                        break;
                    }
                    case 'h': {
                        char hostname[256];
                        if (gethostname(hostname, sizeof(hostname)) == 0) {
                            printf("%s", hostname);
                        }
                        break;
                    }
                    case 'w': {
                        char cwd[1024];
                        if (getcwd(cwd, sizeof(cwd))) {
                            printf("%s", cwd);
                        }
                        break;
                    }
                    case 'W': {
                        char cwd[1024];
                        if (getcwd(cwd, sizeof(cwd))) {
                            char *base = strrchr(cwd, '/');
                            printf("%s", base ? base + 1 : cwd);
                        }
                        break;
                    }
                    case 'n':
                        printf("\n");
                        break;
                    case '$':
                        printf("$");
                        break;
                    case '\\':
                        printf("\\");
                        break;
                    default:
                        printf("\\%c", *p);
                        break;
                }
            } else {
                printf("%c", *p);
            }
        }
        fflush(stdout);
    }
}

int main(int argc, char *argv[]) {
    char *shellPath = realpath(argv[0], NULL);
    if (shellPath) {
        Command::_shellPath = std::string(shellPath);
        setenv("SHELL", shellPath, 1);
        free(shellPath);
    }
    
    struct sigaction sa_int;
    sa_int.sa_handler = ctrlCHandler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa_int, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    
    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchldHandler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    
    Shell::prompt();
    yyparse();
    return 0;
}

Command Shell::_currentCommand;