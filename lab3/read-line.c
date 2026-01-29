#define _XOPEN_SOURCE 700

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_BUFFER_LINE 2048
#define MAX_HISTORY 100

extern void tty_raw_mode(void);

static int line_length;
static int cursor_position;
static char line_buffer[MAX_BUFFER_LINE];

static char *history[MAX_HISTORY];
static int history_length = 0;
static int history_index = 0;
static int browsing_history = 0;
static char saved_line[MAX_BUFFER_LINE];

static char last_tab_word[256] = "";

void read_line_print_usage() {
    char *usage = "\n"
    " ctrl-?       Print usage\n"
    " Backspace    Deletes character before cursor\n"
    " Delete       Deletes character at cursor\n"
    " Up arrow     Previous command in history\n"
    " Down arrow   Next command in history\n"
    " Left arrow   Move cursor left\n"
    " Right arrow  Move cursor right\n"
    " Home (ctrl-A) Move cursor to beginning\n"
    " End (ctrl-E)  Move cursor to end\n"
    " Tab          Complete path/command\n"
    " Tab Tab      List all completions\n";
    write(1, usage, strlen(usage));
}

void add_to_history(const char *line) {
    if (line == NULL || strlen(line) <= 1) {
        return;
    }
    
    char *cmd = strdup(line);
    if (cmd == NULL) return;
    
    size_t len = strlen(cmd);
    if (cmd[len - 1] == '\n') {
        cmd[len - 1] = '\0';
    }
    
    if (strlen(cmd) == 0) {
        free(cmd);
        return;
    }
    
    if (history_length > 0 && strcmp(history[history_length - 1], cmd) == 0) {
        free(cmd);
        return;
    }
    
    if (history_length < MAX_HISTORY) {
        history[history_length++] = cmd;
    } else {
        free(history[0]);
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            history[i] = history[i + 1];
        }
        history[MAX_HISTORY - 1] = cmd;
    }
}

void insert_char(char ch) {
    if (line_length >= MAX_BUFFER_LINE - 2) return;
    
    for (int i = line_length; i > cursor_position; i--) {
        line_buffer[i] = line_buffer[i - 1];
    }
    
    line_buffer[cursor_position] = ch;
    cursor_position++;
    line_length++;
    
    write(1, line_buffer + cursor_position - 1, line_length - cursor_position + 1);
    
    for (int i = 0; i < line_length - cursor_position; i++) {
        write(1, "\b", 1);
    }
}

void delete_char() {
    if (cursor_position < line_length) {
        for (int i = cursor_position; i < line_length - 1; i++) {
            line_buffer[i] = line_buffer[i + 1];
        }
        line_length--;
        
        write(1, line_buffer + cursor_position, line_length - cursor_position);
        write(1, " ", 1);
        for (int i = 0; i <= line_length - cursor_position; i++) {
            write(1, "\b", 1);
        }
    }
}

void backspace_char() {
    if (cursor_position > 0) {
        cursor_position--;
        write(1, "\b", 1);
        delete_char();
    }
}

void move_left() {
    if (cursor_position > 0) {
        cursor_position--;
        write(1, "\b", 1);
    }
}

void move_right() {
    if (cursor_position < line_length) {
        write(1, &line_buffer[cursor_position], 1);
        cursor_position++;
    }
}

void move_home() {
    while (cursor_position > 0) {
        cursor_position--;
        write(1, "\b", 1);
    }
}

void move_end() {
    while (cursor_position < line_length) {
        write(1, &line_buffer[cursor_position], 1);
        cursor_position++;
    }
}

void clear_line() {
    while (cursor_position > 0) {
        cursor_position--;
        write(1, "\b", 1);
    }
    
    for (int i = 0; i < line_length; i++) {
        write(1, " ", 1);
    }
    
    for (int i = 0; i < line_length; i++) {
        write(1, "\b", 1);
    }
    
    line_length = 0;
    cursor_position = 0;
}

void load_history(int index) {
    if (index < 0 || index >= history_length) return;
    
    clear_line();
    
    strcpy(line_buffer, history[index]);
    line_length = strlen(line_buffer);
    cursor_position = line_length;
    
    write(1, line_buffer, line_length);
}

void get_current_word(char *word, int *word_start) {
    *word_start = cursor_position;
    
    while (*word_start > 0 && line_buffer[*word_start - 1] != ' ' && 
           line_buffer[*word_start - 1] != '\t') {
        (*word_start)--;
    }
    
    int word_len = cursor_position - *word_start;
    strncpy(word, line_buffer + *word_start, word_len);
    word[word_len] = '\0';
}

int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
        return S_ISDIR(statbuf.st_mode);
    }
    return 0;
}

void find_command_completions(const char *prefix, char matches[][256], int *match_count) {
    const char *path_env = getenv("PATH");
    if (!path_env) return;
    
    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    char *dir = strtok(path_copy, ":");
    size_t prefix_len = strlen(prefix);
    
    while (dir && *match_count < 256) {
        DIR *d = opendir(dir);
        if (!d) {
            dir = strtok(NULL, ":");
            continue;
        }
        
        struct dirent *entry;
        while ((entry = readdir(d)) && *match_count < 256) {
            if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
                int duplicate = 0;
                for (int i = 0; i < *match_count; i++) {
                    if (strcmp(matches[i], entry->d_name) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    strncpy(matches[*match_count], entry->d_name, 255);
                    matches[*match_count][255] = '\0';
                    (*match_count)++;
                }
            }
        }
        closedir(d);
        dir = strtok(NULL, ":");
    }
}

void find_file_completions(const char *prefix, char matches[][256], int *match_count) {
    char dir_path[1024] = ".";
    char file_prefix[256];
    
    const char *last_slash = strrchr(prefix, '/');
    if (last_slash) {
        size_t dir_len = last_slash - prefix;
        if (dir_len == 0) {
            strcpy(dir_path, "/");
        } else {
            strncpy(dir_path, prefix, dir_len);
            dir_path[dir_len] = '\0';
        }
        strcpy(file_prefix, last_slash + 1);
    } else {
        strcpy(file_prefix, prefix);
    }
    
    char expanded_dir[1024];
    if (dir_path[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(expanded_dir, sizeof(expanded_dir), "%s%s", home, dir_path + 1);
        } else {
            strcpy(expanded_dir, dir_path);
        }
    } else {
        strcpy(expanded_dir, dir_path);
    }
    
    DIR *d = opendir(expanded_dir);
    if (!d) return;
    
    size_t prefix_len = strlen(file_prefix);
    struct dirent *entry;
    
    while ((entry = readdir(d)) && *match_count < 256) {
        if (entry->d_name[0] == '.' && prefix_len == 0) {
            continue;
        }
        
        if (strncmp(entry->d_name, file_prefix, prefix_len) == 0) {
            if (last_slash) {
                if (strcmp(dir_path, "/") == 0) {
                    snprintf(matches[*match_count], 256, "/%s", entry->d_name);
                } else {
                    snprintf(matches[*match_count], 256, "%s/%s", dir_path, entry->d_name);
                }
            } else {
                strncpy(matches[*match_count], entry->d_name, 255);
                matches[*match_count][255] = '\0';
            }
            
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", expanded_dir, entry->d_name);
            if (is_directory(full_path)) {
                strcat(matches[*match_count], "/");
            }
            
            (*match_count)++;
        }
    }
    closedir(d);
}

void reprint_prompt() {
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
                default:
                    printf("%c", *p);
                    break;
            }
        } else {
            printf("%c", *p);
        }
    }
    fflush(stdout);
}

void handle_tab_completion(int double_tab) {
    char word[256];
    int word_start;
    
    get_current_word(word, &word_start);
    
    if (strlen(word) == 0) {
        return;
    }
    
    char matches[256][256];
    int match_count = 0;
    
    int is_first_word = 1;
    for (int i = 0; i < word_start; i++) {
        if (line_buffer[i] != ' ' && line_buffer[i] != '\t') {
            is_first_word = 0;
            break;
        }
    }
    
    if (is_first_word && strchr(word, '/') == NULL) {
        find_command_completions(word, matches, &match_count);
    }
    
    char file_matches[256][256];
    int file_match_count = 0;
    find_file_completions(word, file_matches, &file_match_count);
    
    for (int i = 0; i < file_match_count && match_count < 256; i++) {
        strcpy(matches[match_count++], file_matches[i]);
    }
    
    if (match_count == 0) {
        write(1, "\a", 1);
    } else if (match_count == 1 && !double_tab) {
        while (cursor_position > word_start) {
            backspace_char();
        }
        
        const char *completion = matches[0];
        for (size_t i = 0; i < strlen(completion); i++) {
            insert_char(completion[i]);
        }
    } else {
        if (double_tab) {
            write(1, "\n", 1);
            
            for (int i = 0; i < match_count; i++) {
                printf("%s\n", matches[i]);
            }
            
            reprint_prompt();
            write(1, line_buffer, line_length);
            
            for (int i = cursor_position; i < line_length; i++) {
                write(1, "\b", 1);
            }
        } else {
            size_t common_len = strlen(matches[0]);
            for (int i = 1; i < match_count; i++) {
                size_t j = 0;
                while (j < common_len && matches[0][j] == matches[i][j]) {
                    j++;
                }
                common_len = j;
            }
            
            if (common_len > strlen(word)) {
                while (cursor_position > word_start) {
                    backspace_char();
                }
                
                for (size_t i = 0; i < common_len; i++) {
                    insert_char(matches[0][i]);
                }
            } else {
                write(1, "\a", 1);
            }
        }
    }
}

char *read_line() {
    if (!isatty(0)) {
        if (fgets(line_buffer, MAX_BUFFER_LINE, stdin) == NULL) {
            return NULL;
        }
        return line_buffer;
    }
    
    tty_raw_mode();
    
    line_length = 0;
    cursor_position = 0;
    browsing_history = 0;
    history_index = history_length;
    
    int tab_pressed_count = 0;
    
    while (1) {
        char ch;
        if (read(0, &ch, 1) <= 0) {
            return NULL;
        }
        
        if (ch >= 32 && ch < 127) {
            tab_pressed_count = 0;
            
            if (!browsing_history && line_length == 0) {
                saved_line[0] = '\0';
            }
            browsing_history = 0;
            insert_char(ch);
        }
        else if (ch == 10 || ch == 13) {
            write(1, "\n", 1);
            tab_pressed_count = 0;
            break;
        }
        else if (ch == 31) {
            read_line_print_usage();
            line_buffer[0] = 0;
            tab_pressed_count = 0;
            break;
        }
        else if (ch == 9) {
            tab_pressed_count++;
            
            char current_word[256];
            int word_start;
            get_current_word(current_word, &word_start);
            
            int is_double_tab = (tab_pressed_count >= 2);
            
            handle_tab_completion(is_double_tab);
            
            strcpy(last_tab_word, current_word);
            
            if (is_double_tab) {
                tab_pressed_count = 0;
            }
        }
        else if (ch == 4) {
            delete_char();
            tab_pressed_count = 0;
        }
        else if (ch == 8 || ch == 127) {
            backspace_char();
            tab_pressed_count = 0;
        }
        else if (ch == 1) {
            move_home();
            tab_pressed_count = 0;
        }
        else if (ch == 5) {
            move_end();
            tab_pressed_count = 0;
        }
        else if (ch == 27) {
            tab_pressed_count = 0;
            char ch1, ch2;
            read(0, &ch1, 1);
            read(0, &ch2, 1);
            
            if (ch1 == 91) {
                if (ch2 == 65) {
                    if (!browsing_history) {
                        strncpy(saved_line, line_buffer, line_length);
                        saved_line[line_length] = '\0';
                        browsing_history = 1;
                    }
                    
                    if (history_index > 0) {
                        history_index--;
                        load_history(history_index);
                    }
                }
                else if (ch2 == 66) {
                    if (browsing_history) {
                        if (history_index < history_length - 1) {
                            history_index++;
                            load_history(history_index);
                        } else {
                            browsing_history = 0;
                            history_index = history_length;
                            clear_line();
                            strcpy(line_buffer, saved_line);
                            line_length = strlen(line_buffer);
                            cursor_position = line_length;
                            write(1, line_buffer, line_length);
                        }
                    }
                }
                else if (ch2 == 67) {
                    move_right();
                }
                else if (ch2 == 68) {
                    move_left();
                }
                else if (ch2 == 51) {
                    char ch3;
                    read(0, &ch3, 1);
                    if (ch3 == 126) {
                        delete_char();
                    }
                }
                else if (ch2 == 72) {
                    move_home();
                }
                else if (ch2 == 70) {
                    move_end();
                }
            }
        }
    }
    
    line_buffer[line_length] = '\n';
    line_length++;
    line_buffer[line_length] = '\0';
    
    if (line_length > 1) {
        add_to_history(line_buffer);
    }
    
    return line_buffer;
}