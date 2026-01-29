%{
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <pwd.h>
#include "y.tab.hh"
#include "command.hh"

static void yyunput(int c, char *buf_ptr);

void myunputc(int c) {
  unput(c);
}

#ifdef EDIT_MODE_ON
extern "C" char * read_line();

int mygetc(FILE * f) {
  static char *p;
  char ch;
  
  if (!isatty(0)) {
    return getc(f);
  }
  
  if (p == NULL || *p == 0) {
    char * s = read_line();
    p = s;
  }
  
  ch = *p;
  p++;
  return ch;
}

#undef getc
#define getc(f) mygetc(f)
#endif
%}

%option noyywrap

%{
#include <dirent.h>
#include <regex.h>
#include <vector>
#include <algorithm>

void expandWildcard(const char *prefix, const char *suffix, std::vector<std::string> &results);
std::string wildcardToRegex(const char *pattern);

void expandWildcard(const char *prefix, const char *suffix, std::vector<std::string> &results) {
    if (*suffix == 0) {
        results.push_back(std::string(prefix));
        return;
    }

    char *s = strdup(suffix);
    char *slash = strchr(s, '/');
    char *component;
    char *rest;

    if (slash) {
        *slash = 0;
        component = s;
        rest = slash + 1;
    } else {
        component = s;
        rest = s + strlen(s);
    }

    bool has_wildcard = (strchr(component, '*') != NULL || strchr(component, '?') != NULL);
    
    if (!has_wildcard) {
        std::string new_prefix;
        if (strlen(prefix) == 0) {
            new_prefix = component;
        } else if (strcmp(prefix, "/") == 0) {
            new_prefix = std::string("/") + component;
        } else {
            new_prefix = std::string(prefix) + "/" + component;
        }
        
        expandWildcard(new_prefix.c_str(), rest, results);
        free(s);
        return;
    }

    std::string regex_pattern = wildcardToRegex(component);
    
    regex_t re;
    int result = regcomp(&re, regex_pattern.c_str(), REG_EXTENDED | REG_NOSUB);
    if (result != 0) {
        free(s);
        return;
    }

    const char *dir_to_open;
    if (strlen(prefix) == 0) {
        dir_to_open = ".";
    } else {
        dir_to_open = prefix;
    }
    
    DIR *dir = opendir(dir_to_open);
    if (!dir) {
        regfree(&re);
        free(s);
        return;
    }

    std::vector<std::string> matches;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            if (component[0] != '.') {
                continue;
            }
            if (strlen(component) == 1) {
                continue;
            }
        }
        
        if (entry->d_name[0] == '.') {
            if (component[0] != '.') {
                continue;
            }
        }

        if (regexec(&re, entry->d_name, 0, NULL, 0) == 0) {
            matches.push_back(std::string(entry->d_name));
        }
    }

    closedir(dir);
    regfree(&re);

    if (matches.empty()) {
        free(s);
        return;
    }

    std::sort(matches.begin(), matches.end());

    for (size_t i = 0; i < matches.size(); i++) {
        std::string new_prefix;
        if (strlen(prefix) == 0) {
            new_prefix = matches[i];
        } else if (strcmp(prefix, "/") == 0) {
            new_prefix = std::string("/") + matches[i];
        } else {
            new_prefix = std::string(prefix) + "/" + matches[i];
        }

        if (*rest == 0) {
            results.push_back(new_prefix);
        } else {
            expandWildcard(new_prefix.c_str(), rest, results);
        }
    }

    free(s);
}

std::string wildcardToRegex(const char *pattern) {
    std::string regex = "^";
    
    for (int i = 0; pattern[i] != 0; i++) {
        char c = pattern[i];
        
        if (c == '*') {
            regex += ".*";
        } else if (c == '?') {
            regex += ".";
        } else if (c == '.') {
            regex += "\\.";
        } else if (c == '\\' && pattern[i + 1] != 0) {
            i++;
            regex += pattern[i];
        } else if (strchr("^$[](){}+|", c)) {
            regex += "\\";
            regex += c;
        } else {
            regex += c;
        }
    }
    
    regex += "$";
    return regex;
}
%}

%%

\n { return NEWLINE; }
[ \t] { }

"2>>&" { return GREATGREATAMPERSAND; }
"2>&" { return GREATAMPERSAND; }
"2>>" { return GREATGREAT; }
"2>" { return GREAT; }
">>&" { return GREATGREATAMPERSAND; }
">&" { return GREATAMPERSAND; }
">>" { return GREATGREAT; }
">" { return GREAT; }
"|" { return PIPE; }
"&" { return AMPERSAND; }

"<(" {
  std::string procsub_cmd;
  int paren_count = 1;
  int c;
  
  while (paren_count > 0) {
    c = yyinput();
    if (c == EOF) break;
    if (c == '(') paren_count++;
    else if (c == ')') paren_count--;
    if (paren_count > 0) procsub_cmd.push_back(c);
  }
  
  char tmpDir[] = "/tmp/procsubXXXXXX";
  if (!mkdtemp(tmpDir)) exit(1);
  
  char fifoPath[1024];
  snprintf(fifoPath, sizeof(fifoPath), "%s/fifo", tmpDir);
  
  if (mkfifo(fifoPath, 0600) == -1) {
    rmdir(tmpDir);
    exit(1);
  }
  
  int pipe_to_child[2];
  if (pipe(pipe_to_child) == -1) {
    unlink(fifoPath);
    rmdir(tmpDir);
    exit(1);
  }
  
  pid_t pid = fork();
  
  if (pid == 0) {
    close(pipe_to_child[1]);
    
    dup2(pipe_to_child[0], 0);
    close(pipe_to_child[0]);
    
    int fd = open(fifoPath, O_WRONLY);
    if (fd == -1) _exit(1);
    
    dup2(fd, 1);
    close(fd);
    
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    close(devnull);
    
    setenv("SUBSHELL", "1", 1);
    
    execl("/proc/self/exe", "shell", NULL);
    _exit(1);
    
  } else if (pid > 0) {
    close(pipe_to_child[0]);
    
    write(pipe_to_child[1], procsub_cmd.c_str(), procsub_cmd.length());
    write(pipe_to_child[1], "\nexit\n", 6);
    close(pipe_to_child[1]);
    
    char *fifo_copy = strdup(fifoPath);
    char *dir_copy = strdup(tmpDir);
    
    char env_val[2048];
    snprintf(env_val, sizeof(env_val), "%s:%s:%d", fifo_copy, dir_copy, pid);
    
    const char *existing = getenv("PROCSUB_CLEANUP");
    if (existing) {
      char combined[4096];
      snprintf(combined, sizeof(combined), "%s,%s", existing, env_val);
      setenv("PROCSUB_CLEANUP", combined, 1);
    } else {
      setenv("PROCSUB_CLEANUP", env_val, 1);
    }
    
    free(fifo_copy);
    free(dir_copy);
    
  } else {
    unlink(fifoPath);
    rmdir(tmpDir);
    exit(1);
  }
  
  yylval.cpp_string = new std::string(fifoPath);
  return WORD;
}

"<" { return LESS; }
"(" { return LPAREN; }
")" { return RPAREN; }

\$\{[^\}]+\} {
  std::string var_name(yytext + 2, yyleng - 3);
  std::string expansion;
  
  if (var_name == "$") {
    expansion = std::to_string(getpid());
  } else if (var_name == "?") {
    expansion = std::to_string(Command::_lastReturnCode);
  } else if (var_name == "!") {
    if (Command::_lastBackgroundPid > 0) expansion = std::to_string(Command::_lastBackgroundPid);
    else expansion = "";
  } else if (var_name == "_") {
    expansion = Command::_lastArgument;
  } else if (var_name == "SHELL") {
    expansion = Command::_shellPath;
  } else {
    const char *value = getenv(var_name.c_str());
    if (value) expansion = std::string(value);
    else expansion = "";
  }
  
  for (int i = expansion.length() - 1; i >= 0; i--) {
    unput(expansion[i]);
  }
}

\$\( {
  std::string subshell_cmd;
  int paren_count = 1;
  int c;
  
  while (paren_count > 0) {
    c = yyinput();
    if (c == EOF) break;
    if (c == '(') paren_count++;
    else if (c == ')') paren_count--;
    if (paren_count > 0) subshell_cmd.push_back(c);
  }
  
  int pipe_to_child[2];
  int pipe_from_child[2];
  
  if (pipe(pipe_to_child) == -1 || pipe(pipe_from_child) == -1) exit(1);
  
  pid_t pid = fork();
  
  if (pid == 0) {
    dup2(pipe_to_child[0], 0);
    close(pipe_to_child[0]);
    close(pipe_to_child[1]);
    
    dup2(pipe_from_child[1], 1);
    close(pipe_from_child[0]);
    close(pipe_from_child[1]);
    
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    close(devnull);
    
    setenv("SUBSHELL", "1", 1);
    
    execl("/proc/self/exe", "shell", NULL);
    _exit(1);
    
  } else if (pid > 0) {
    close(pipe_to_child[0]);
    close(pipe_from_child[1]);
    
    write(pipe_to_child[1], subshell_cmd.c_str(), subshell_cmd.length());
    write(pipe_to_child[1], "\n", 1);
    write(pipe_to_child[1], "exit\n", 5);
    close(pipe_to_child[1]);
    
    char buffer[4096];
    std::string output;
    ssize_t bytes_read;
    
    while ((bytes_read = read(pipe_from_child[0], buffer, sizeof(buffer))) > 0) {
      output.append(buffer, bytes_read);
    }
    close(pipe_from_child[0]);
    
    int status;
    waitpid(pid, &status, 0);
    
    for (size_t i = 0; i < output.length(); i++) {
      if (output[i] == '\n' || output[i] == '\t') output[i] = ' ';
    }
    
    while (!output.empty() && output.back() == ' ') output.pop_back();
    
    for (int i = output.length() - 1; i >= 0; i--) {
      unput(output[i]);
    }
    
  } else {
    exit(1);
  }
}

~[^ \t\n\|\&\<\>\$\\]* {
  std::string expansion;
  std::string text(yytext);
  
  if (text.length() == 1 || text[1] == '/') {
    const char *home = getenv("HOME");
    if (home) {
      expansion = std::string(home);
      if (text.length() > 1) expansion += text.substr(1);
    } else expansion = text;
  } else {
    size_t slash_pos = text.find('/');
    std::string username;
    std::string rest;
    
    if (slash_pos != std::string::npos) {
      username = text.substr(1, slash_pos - 1);
      rest = text.substr(slash_pos);
    } else {
      username = text.substr(1);
      rest = "";
    }
    
    struct passwd *pw = getpwnam(username.c_str());
    if (pw && pw->pw_dir) expansion = std::string(pw->pw_dir) + rest;
    else expansion = text;
  }
  
  for (int i = expansion.length() - 1; i >= 0; i--) {
    unput(expansion[i]);
  }
}

\"([^\\\"]|\\.)*\" {
  std::string text;
  for (int i = 1; i < yyleng - 1; i++) {
    if (yytext[i] == '\\' && i + 1 < yyleng - 1) {
      i++;
      text.push_back(yytext[i]);
    } else {
      text.push_back(yytext[i]);
    }
  }
  yylval.cpp_string = new std::string(text);
  return WORD;
}

\'([^\\\']|\\.)*\' {
  std::string text;
  for (int i = 1; i < yyleng - 1; i++) {
    if (yytext[i] == '\\' && i + 1 < yyleng - 1) {
      i++;
      text.push_back(yytext[i]);
    } else {
      text.push_back(yytext[i]);
    }
  }
  yylval.cpp_string = new std::string(text);
  return WORD;
}

([^ \t\n\|\&\<\>\$\\]|\\.)+ {
  std::string text;
  bool has_wildcard = false;
  
  for (int i = 0; i < yyleng; i++) {
    if (yytext[i] == '\\' && i + 1 < yyleng) {
      i++;
      text.push_back(yytext[i]);
    } else {
      if (yytext[i] == '*' || yytext[i] == '?') {
        has_wildcard = true;
      }
      text.push_back(yytext[i]);
    }
  }
  
  if (has_wildcard) {
    std::vector<std::string> results;
    
    if (text[0] == '/') {
      size_t first_slash = text.find('/', 1);
      if (first_slash == std::string::npos) {
        expandWildcard("/", text.c_str() + 1, results);
      } else {
        std::string first_comp = text.substr(1, first_slash - 1);
        std::string rest = text.substr(first_slash + 1);
        
        if (first_comp.find('*') != std::string::npos || first_comp.find('?') != std::string::npos) {
          expandWildcard("/", text.c_str() + 1, results);
        } else {
          expandWildcard(("/" + first_comp).c_str(), rest.c_str(), results);
        }
      }
    } else {
      expandWildcard("", text.c_str(), results);
    }
    
    if (results.empty()) {
      yylval.cpp_string = new std::string(text);
      return WORD;
    }
    
    std::sort(results.begin(), results.end());
    
    for (int i = results.size() - 1; i >= 0; i--) {
      if (i < (int)results.size() - 1) {
        unput(' ');
      }
      for (int j = results[i].length() - 1; j >= 0; j--) {
        unput(results[i][j]);
      }
    }
  } else {
    yylval.cpp_string = new std::string(text);
    return WORD;
  }
}

%%
