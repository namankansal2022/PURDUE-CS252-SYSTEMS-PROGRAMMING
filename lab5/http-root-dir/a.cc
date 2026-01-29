#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <arpa/inet.h>

time_t serverStartTime;
int totalRequests = 0;
double minServiceTime = -1;
double maxServiceTime = 0;
char minServiceURL[1024] = "";
char maxServiceURL[1024] = "";
pthread_mutex_t statsMutex = PTHREAD_MUTEX_INITIALIZER;

const char* logFilePath = "http-root-dir/server.log";

int QueueLength = 5;
const int PoolSize = 5;
pthread_mutex_t acceptMutex = PTHREAD_MUTEX_INITIALIZER;
int masterSocketGlobal;

void processHTTPRequest(int socket);
const char* getContentType(const char* path);
int base64Decode(const char* input, char* output, int maxLen);
int checkAuthorization(const char* request);
void sanitizePath(char* path);
void printUsage();
void* threadHandler(void* arg);
void* poolThreadHandler(void* arg);
void zombieHandler(int sig);
void serveDirectory(int fd, const char* dirPath, const char* urlPath, const char* sortBy, const char* order);
void serveCGI(int fd, const char* scriptPath, const char* queryString, const char* requestMethod);
void serveStats(int fd);
void serveLogs(int fd);
void logRequest(const char* clientIP, const char* url);
void updateStats(const char* url, double serviceTime);

int main(int argc, char **argv) {
    int port = 9000;
    int mode = 0;
    
    serverStartTime = time(NULL);
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            mode = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            mode = 2;
        } else if (strcmp(argv[i], "-p") == 0) {
            mode = 3;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage();
            return 0;
        } else {
            port = atoi(argv[i]);
        }
    }
    
    printf("Starting HTTP Server on port %d\n", port);
    
    struct sockaddr_in serverIPAddress;
    memset(&serverIPAddress, 0, sizeof(serverIPAddress));
    serverIPAddress.sin_family = AF_INET;
    serverIPAddress.sin_addr.s_addr = INADDR_ANY;
    serverIPAddress.sin_port = htons((u_short)port);
    
    int masterSocket = socket(PF_INET, SOCK_STREAM, 0);
    if (masterSocket < 0) {
        perror("socket");
        exit(-1);
    }
    
    int optval = 1;
    setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(int));
    
    if (bind(masterSocket, (struct sockaddr *)&serverIPAddress, sizeof(serverIPAddress))) {
        perror("bind");
        exit(-1);
    }
    
    if (listen(masterSocket, QueueLength)) {
        perror("listen");
        exit(-1);
    }
    
    if (mode == 1) {
        struct sigaction sa;
        sa.sa_handler = zombieHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);
    }
    
    if (mode == 3) {
        masterSocketGlobal = masterSocket;
        pthread_t threads[PoolSize];
        for (int i = 0; i < PoolSize; i++) {
            pthread_create(&threads[i], NULL, poolThreadHandler, NULL);
        }
        for (int i = 0; i < PoolSize; i++) {
            pthread_join(threads[i], NULL);
        }
        return 0;
    }
    
    while (1) {
        struct sockaddr_in clientIPAddress;
        int alen = sizeof(clientIPAddress);
        int slaveSocket = accept(masterSocket, (struct sockaddr *)&clientIPAddress, (socklen_t *)&alen);
        
        if (slaveSocket < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        if (mode == 0) {
            processHTTPRequest(slaveSocket);
            close(slaveSocket);
        } else if (mode == 1) {
            pid_t pid = fork();
            if (pid == 0) {
                close(masterSocket);
                processHTTPRequest(slaveSocket);
                close(slaveSocket);
                exit(0);
            } else if (pid > 0) {
                close(slaveSocket);
            }
        } else if (mode == 2) {
            pthread_t thread;
            int* sockPtr = (int*)malloc(sizeof(int));
            *sockPtr = slaveSocket;
            pthread_create(&thread, NULL, threadHandler, sockPtr);
            pthread_detach(thread);
        }
    }
    
    return 0;
}

void zombieHandler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void* threadHandler(void* arg) {
    int slaveSocket = *(int*)arg;
    free(arg);
    processHTTPRequest(slaveSocket);
    close(slaveSocket);
    return NULL;
}

void* poolThreadHandler(void* arg) {
    while (1) {
        struct sockaddr_in clientIPAddress;
        int alen = sizeof(clientIPAddress);
        
        pthread_mutex_lock(&acceptMutex);
        int slaveSocket = accept(masterSocketGlobal, (struct sockaddr *)&clientIPAddress, (socklen_t *)&alen);
        pthread_mutex_unlock(&acceptMutex);
        
        if (slaveSocket < 0) continue;
        
        processHTTPRequest(slaveSocket);
        close(slaveSocket);
    }
    return NULL;
}

void printUsage() {
    printf("Usage: myhttpd [-f|-t|-p] [<port>]\n");
    printf("  -f : Fork mode (process per request)\n");
    printf("  -t : Thread mode (thread per request)\n");
    printf("  -p : Thread pool mode (pool of 5 threads)\n");
    printf("  <port> : Port number (default: 9000)\n");
    printf("\n");
    printf("Special URLs:\n");
    printf("  /stats : View server statistics\n");
    printf("  /logs  : View request logs\n");
}

int base64Decode(const char* input, char* output, int maxLen) {
    const char base64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int inputLen = strlen(input);
    int outputLen = 0;
    
    for (int i = 0; i < inputLen && outputLen < maxLen - 3; i += 4) {
        int val[4];
        for (int j = 0; j < 4; j++) {
            if (i + j >= inputLen || input[i + j] == '=') {
                val[j] = 0;
            } else {
                const char* p = strchr(base64Chars, input[i + j]);
                val[j] = p ? (p - base64Chars) : 0;
            }
        }
        
        output[outputLen++] = (val[0] << 2) | (val[1] >> 4);
        if (i + 2 < inputLen && input[i + 2] != '=')
            output[outputLen++] = ((val[1] & 0x0F) << 4) | (val[2] >> 2);
        if (i + 3 < inputLen && input[i + 3] != '=')
            output[outputLen++] = ((val[2] & 0x03) << 6) | val[3];
    }
    
    output[outputLen] = '\0';
    return outputLen;
}

int checkAuthorization(const char* request) {
    const char* authHeader = strstr(request, "Authorization: Basic ");
    if (authHeader == NULL) {
        return 0;
    }
    
    authHeader += 21;
    char encodedCreds[256];
    int i = 0;
    while (authHeader[i] != '\r' && authHeader[i] != '\n' && i < 255) {
        encodedCreds[i] = authHeader[i];
        i++;
    }
    encodedCreds[i] = '\0';
    
    char decodedCreds[256];
    base64Decode(encodedCreds, decodedCreds, 256);
    
    const char* validCreds = "namab:kansal2";
    
    return strcmp(decodedCreds, validCreds) == 0;
}

void sanitizePath(char* path) {
    char* p = path;
    while (*p) {
        if (strncmp(p, "/../", 4) == 0 || strcmp(p, "/..") == 0) {
            strcpy(p, p + 3);
        } else {
            p++;
        }
    }
    
    if (strstr(path, "..") != NULL) {
        strcpy(path, "/");
    }
}

void updateStats(const char* url, double serviceTime) {
    pthread_mutex_lock(&statsMutex);
    
    totalRequests++;
    
    if (minServiceTime < 0 || serviceTime < minServiceTime) {
        minServiceTime = serviceTime;
        strncpy(minServiceURL, url, sizeof(minServiceURL) - 1);
    }
    
    if (serviceTime > maxServiceTime) {
        maxServiceTime = serviceTime;
        strncpy(maxServiceURL, url, sizeof(maxServiceURL) - 1);
    }
    
    pthread_mutex_unlock(&statsMutex);
}

void logRequest(const char* clientIP, const char* url) {
    FILE* logFile = fopen(logFilePath, "a");
    if (logFile) {
        time_t now = time(NULL);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(logFile, "%s | %s | %s\n", timeStr, clientIP, url);
        fclose(logFile);
    }
}

void serveStats(int fd) {
    char response[8192];
    time_t now = time(NULL);
    double uptime = difftime(now, serverStartTime);
    
    int hours = (int)uptime / 3600;
    int minutes = ((int)uptime % 3600) / 60;
    int seconds = (int)uptime % 60;
    
    pthread_mutex_lock(&statsMutex);
    
    sprintf(response,
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n"
        "<head><title>Server Statistics</title></head>\n"
        "<body>\n"
        "<h1>Server Statistics</h1>\n"
        "<hr>\n"
        "<p><strong>Authors:</strong> Namab Kansal</p>\n"
        "<p><strong>Server Uptime:</strong> %d hours, %d minutes, %d seconds</p>\n"
        "<p><strong>Total Requests:</strong> %d</p>\n"
        "<p><strong>Min Service Time:</strong> %.6f seconds (URL: %s)</p>\n"
        "<p><strong>Max Service Time:</strong> %.6f seconds (URL: %s)</p>\n"
        "</body>\n"
        "</html>\n",
        hours, minutes, seconds,
        totalRequests,
        minServiceTime > 0 ? minServiceTime : 0.0, minServiceTime > 0 ? minServiceURL : "N/A",
        maxServiceTime, maxServiceTime > 0 ? maxServiceURL : "N/A");
    
    pthread_mutex_unlock(&statsMutex);
    
    write(fd, response, strlen(response));
}

void serveLogs(int fd) {
    FILE* logFile = fopen(logFilePath, "r");
    
    char header[] = 
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n"
        "<head><title>Request Logs</title></head>\n"
        "<body>\n"
        "<h1>Request Logs</h1>\n"
        "<hr>\n"
        "<table border='1' cellpadding='5'>\n"
        "<tr><th>Time</th><th>Client IP</th><th>URL</th></tr>\n";
    
    write(fd, header, strlen(header));
    
    if (logFile) {
        char line[1024];
        while (fgets(line, sizeof(line), logFile)) {
            char timeStr[64], ip[64], url[512];
            if (sscanf(line, "%[^|] | %[^|] | %[^\n]", timeStr, ip, url) == 3) {
                char row[2048];
                sprintf(row, "<tr><td>%s</td><td>%s</td><td>%s</td></tr>\n", timeStr, ip, url);
                write(fd, row, strlen(row));
            }
        }
        fclose(logFile);
    }
    
    const char* footer = "</table>\n</body>\n</html>\n";
    write(fd, footer, strlen(footer));
}

void processHTTPRequest(int fd) {
    clock_t startTime = clock();
    
    const int MaxRequest = 8192;
    char request[MaxRequest + 1];
    int requestLength = 0;
    
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    getpeername(fd, (struct sockaddr*)&clientAddr, &clientLen);
    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    
    unsigned char newChar;
    unsigned char lastChar = 0;
    unsigned char secondLastChar = 0;
    unsigned char thirdLastChar = 0;
    
    while (requestLength < MaxRequest) {
        int n = read(fd, &newChar, sizeof(newChar));
        if (n <= 0) break;
        
        request[requestLength] = newChar;
        requestLength++;
        
        if (thirdLastChar == '\r' && secondLastChar == '\n' && lastChar == '\r' && newChar == '\n') {
            break;
        }
        
        thirdLastChar = secondLastChar;
        secondLastChar = lastChar;
        lastChar = newChar;
    }
    
    request[requestLength] = '\0';
    
    if (!checkAuthorization(request)) {
        const char *authResponse = 
            "HTTP/1.1 401 Unauthorized\r\n"
            "Server: CS 252 lab5\r\n"
            "WWW-Authenticate: Basic realm=\"myhttpd-cs252\"\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body><h1>401 Unauthorized</h1></body></html>\r\n";
        write(fd, authResponse, strlen(authResponse));
        return;
    }
    
    char method[16];
    char path[1024];
    char protocol[16];
    
    sscanf(request, "%s %s %s", method, path, protocol);
    
    logRequest(clientIP, path);
    
    if (strcmp(method, "GET") != 0) {
        const char *errorResponse = 
            "HTTP/1.1 400 Bad Request\r\n"
            "Server: CS 252 lab5\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Only GET method is supported\r\n";
        write(fd, errorResponse, strlen(errorResponse));
        return;
    }
    
    if (strcmp(path, "/stats") == 0) {
        serveStats(fd);
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        return;
    }
    
    if (strcmp(path, "/logs") == 0) {
        serveLogs(fd);
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        return;
    }
    
    char* queryString = strchr(path, '?');
    if (queryString) {
        *queryString = '\0';
        queryString++;
    }
    
    sanitizePath(path);
    
    if (strncmp(path, "/cgi-bin/", 9) == 0) {
        char scriptPath[2048];
        sprintf(scriptPath, "http-root-dir%s", path);
        serveCGI(fd, scriptPath, queryString, "GET");
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        return;
    }
    
    char filePath[2048] = "http-root-dir";
    
    if (strcmp(path, "/") == 0) {
        strcat(filePath, "/htdocs/index.html");
    } else {
        if (strncmp(path, "/htdocs", 7) != 0 && strncmp(path, "/icons", 6) != 0) {
            strcat(filePath, "/htdocs");
        }
        strcat(filePath, path);
    }
    
    struct stat fileStat;
    if (stat(filePath, &fileStat) == 0 && S_ISDIR(fileStat.st_mode)) {
        char sortBy[32] = "N";
        char order[8] = "A";
        
        if (queryString) {
            char* sortParam = strstr(queryString, "C=");
            if (sortParam) {
                sortBy[0] = sortParam[2];
            }
            char* orderParam = strstr(queryString, "O=");
            if (orderParam) {
                order[0] = orderParam[2];
            }
        }
        
        serveDirectory(fd, filePath, path, sortBy, order);
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        return;
    }
    
    int filefd = open(filePath, O_RDONLY);
    
    if (filefd < 0) {
        const char *errorHeader = 
            "HTTP/1.1 404 File Not Found\r\n"
            "Server: CS 252 lab5\r\n"
            "Content-Type: text/html\r\n"
            "\r\n";
        write(fd, errorHeader, strlen(errorHeader));
        
        const char *errorBody = 
            "<html><body>\r\n"
            "<h1>404 File Not Found</h1>\r\n"
            "<p>Could not find the specified URL.</p>\r\n"
            "</body></html>\r\n";
        write(fd, errorBody, strlen(errorBody));
        
        return;
    }
    
    if (fstat(filefd, &fileStat) < 0) {
        close(filefd);
        return;
    }
    
    int fileSize = fileStat.st_size;
    
    char *fileBuffer = (char *)malloc(fileSize);
    if (fileBuffer == NULL) {
        close(filefd);
        return;
    }
    
    int totalBytesRead = 0;
    while (totalBytesRead < fileSize) {
        int bytesRead = read(filefd, fileBuffer + totalBytesRead, fileSize - totalBytesRead);
        if (bytesRead <= 0) break;
        totalBytesRead += bytesRead;
    }
    
    const char *contentType = getContentType(filePath);
    
    char responseHeader[1024];
    sprintf(responseHeader, 
            "HTTP/1.1 200 Document follows\r\n"
            "Server: CS 252 lab5\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "\r\n",
            contentType, totalBytesRead);
    
    write(fd, responseHeader, strlen(responseHeader));
    
    int totalBytesWritten = 0;
    while (totalBytesWritten < totalBytesRead) {
        int bytesWritten = write(fd, fileBuffer + totalBytesWritten, totalBytesRead - totalBytesWritten);
        if (bytesWritten <= 0) break;
        totalBytesWritten += bytesWritten;
    }
    
    free(fileBuffer);
    close(filefd);
    
    clock_t endTime = clock();
    double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
    updateStats(path, serviceTime);
}

const char* getContentType(const char* path) {
    const char *ext = strrchr(path, '.');
    
    if (ext == NULL) return "text/plain";
    
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    } else if (strcmp(ext, ".gif") == 0) {
        return "image/gif";
    } else if (strcmp(ext, ".png") == 0) {
        return "image/png";
    } else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    } else if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    } else if (strcmp(ext, ".css") == 0) {
        return "text/css";
    } else if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    } else if (strcmp(ext, ".txt") == 0) {
        return "text/plain";
    } else {
        return "application/octet-stream";
    }
}

typedef struct {
    char name[256];
    int isDir;
    off_t size;
    time_t mtime;
} FileEntry;

int compareByName(const void* a, const void* b) {
    FileEntry* fa = (FileEntry*)a;
    FileEntry* fb = (FileEntry*)b;
    if (fa->isDir != fb->isDir) return fb->isDir - fa->isDir;
    return strcmp(fa->name, fb->name);
}

int compareBySize(const void* a, const void* b) {
    FileEntry* fa = (FileEntry*)a;
    FileEntry* fb = (FileEntry*)b;
    if (fa->isDir != fb->isDir) return fb->isDir - fa->isDir;
    return (fa->size > fb->size) - (fa->size < fb->size);
}

int compareByMtime(const void* a, const void* b) {
    FileEntry* fa = (FileEntry*)a;
    FileEntry* fb = (FileEntry*)b;
    if (fa->isDir != fb->isDir) return fb->isDir - fa->isDir;
    return (fa->mtime > fb->mtime) - (fa->mtime < fb->mtime);
}

void serveDirectory(int fd, const char* dirPath, const char* urlPath, const char* sortBy, const char* order) {
    DIR* dir = opendir(dirPath);
    if (!dir) {
        const char* error = "HTTP/1.1 404 Not Found\r\n\r\nDirectory not found\n";
        write(fd, error, strlen(error));
        return;
    }
    
    FileEntry entries[1024];
    int count = 0;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) && count < 1024) {
        if (strcmp(entry->d_name, ".") == 0) continue;
        
        strncpy(entries[count].name, entry->d_name, 255);
        entries[count].name[255] = '\0';
        
        char fullPath[2048];
        sprintf(fullPath, "%s/%s", dirPath, entry->d_name);
        
        struct stat st;
        if (stat(fullPath, &st) == 0) {
            entries[count].isDir = S_ISDIR(st.st_mode);
            entries[count].size = st.st_size;
            entries[count].mtime = st.st_mtime;
            count++;
        }
    }
    closedir(dir);
    
    if (sortBy[0] == 'N') {
        qsort(entries, count, sizeof(FileEntry), compareByName);
    } else if (sortBy[0] == 'S') {
        qsort(entries, count, sizeof(FileEntry), compareBySize);
    } else if (sortBy[0] == 'M') {
        qsort(entries, count, sizeof(FileEntry), compareByMtime);
    }
    
    if (order[0] == 'D') {
        for (int i = 0; i < count / 2; i++) {
            FileEntry temp = entries[i];
            entries[i] = entries[count - 1 - i];
            entries[count - 1 - i] = temp;
        }
    }
    
    char header[2048];
    sprintf(header,
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n<head><title>Index of %s</title></head>\n"
        "<body>\n<h1>Index of %s</h1>\n<hr>\n"
        "<table>\n"
        "<tr><th><a href=\"?C=N;O=%s\">Name</a></th>"
        "<th><a href=\"?C=M;O=%s\">Last Modified</a></th>"
        "<th><a href=\"?C=S;O=%s\">Size</a></th></tr>\n",
        urlPath, urlPath,
        order[0] == 'A' ? "D" : "A",
        order[0] == 'A' ? "D" : "A",
        order[0] == 'A' ? "D" : "A");
    
    write(fd, header, strlen(header));
    
    if (strcmp(urlPath, "/") != 0) {
        const char* parent = "<tr><td><img src=\"/icons/back.gif\" alt=\"[DIR]\"> <a href=\"..\">Parent Directory</a></td><td>-</td><td>-</td></tr>\n";
        write(fd, parent, strlen(parent));
    }
    
    for (int i = 0; i < count; i++) {
        char row[4096];
        char* icon = entries[i].isDir ? "/icons/folder.gif" : "/icons/text.gif";
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", localtime(&entries[i].mtime));
        
        if (entries[i].isDir) {
            sprintf(row, "<tr><td><img src=\"%s\" alt=\"[DIR]\"> <a href=\"%s/\">%s/</a></td><td>%s</td><td>-</td></tr>\n",
                icon, entries[i].name, entries[i].name, timeStr);
        } else {
            sprintf(row, "<tr><td><img src=\"%s\" alt=\"[FILE]\"> <a href=\"%s\">%s</a></td><td>%s</td><td>%ld</td></tr>\n",
                icon, entries[i].name, entries[i].name, timeStr, (long)entries[i].size);
        }
        write(fd, row, strlen(row));
    }
    
    const char* footer = "</table>\n<hr>\n</body>\n</html>\n";
    write(fd, footer, strlen(footer));
}

void serveCGI(int fd, const char* scriptPath, const char* queryString, const char* requestMethod) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }
    
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        
        setenv("REQUEST_METHOD", requestMethod, 1);
        if (queryString) {
            setenv("QUERY_STRING", queryString, 1);
        }
        
        if (queryString && strchr(queryString, '+')) {
            char* argv[64];
            int argc = 1;
            argv[0] = (char*)scriptPath;
            
            char queryBuf[1024];
            strncpy(queryBuf, queryString, sizeof(queryBuf) - 1);
            queryBuf[sizeof(queryBuf) - 1] = '\0';
            
            char* token = strtok(queryBuf, "+");
            while (token && argc < 63) {
                argv[argc++] = token;
                token = strtok(NULL, "+");
            }
            argv[argc] = NULL;
            
            execvp(scriptPath, argv);
        } else {
            execl(scriptPath, scriptPath, NULL);
        }
        
        perror("execl");
        exit(1);
    } else {
        close(pipefd[1]);
        
        const char* header = 
            "HTTP/1.1 200 Document follows\r\n"
            "Server: CS 252 lab5\r\n";
        write(fd, header, strlen(header));
        
        char buffer[4096];
        ssize_t bytesRead;
        while ((bytesRead = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            write(fd, buffer, bytesRead);
        }
        
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
    }
}