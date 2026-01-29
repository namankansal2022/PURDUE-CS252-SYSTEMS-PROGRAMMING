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
#include <dirent.h>
#include <time.h>
#include <arpa/inet.h>

int QueueLength = 5;
const int PoolSize = 5;
pthread_mutex_t acceptMutex = PTHREAD_MUTEX_INITIALIZER;
int masterSocketGlobal;

// Statistics tracking
pthread_mutex_t statsMutex = PTHREAD_MUTEX_INITIALIZER;
time_t serverStartTime;
int requestCount = 0;
double minServiceTime = -1;
double maxServiceTime = 0;
char minServiceURL[1024] = "";
char maxServiceURL[1024] = "";
const char* logFilePath = "http-root-dir/server.log";

// Structure to hold file information for sorting
struct FileInfo {
    char name[256];
    int isDir;
    off_t size;
    time_t mtime;
};

void processHTTPRequest(int socket);
const char* getContentType(const char* path);
int base64Decode(const char* input, char* output, int maxLen);
int checkAuthorization(const char* request);
void sanitizePath(char* path);
void printUsage();
void* threadHandler(void* arg);
void* poolThreadHandler(void* arg);
void zombieHandler(int sig);
void serveDirectoryListing(int fd, const char* dirPath, const char* urlPath, const char* sortBy, const char* order);
const char* getIconForFile(const char* filename, int isDir);
void serveStats(int fd);
void serveLogs(int fd);
void logRequest(const char* clientIP, const char* url);
void updateStats(const char* url, double serviceTime);
void serveMJPEGStream(int fd);

// Helper function to extract Content-Length from headers
int getContentLength(const char* request) {
    const char* contentLengthHeader = strstr(request, "Content-Length:");
    if (contentLengthHeader == NULL) {
        contentLengthHeader = strstr(request, "content-length:");
    }
    
    if (contentLengthHeader != NULL) {
        int length = 0;
        sscanf(contentLengthHeader, "%*s %d", &length);
        return length;
    }
    
    return 0;
}

// Helper function to check if Connection: close header is present
int shouldCloseConnection(const char* request) {
    const char* connHeader = strstr(request, "Connection:");
    if (connHeader == NULL) {
        connHeader = strstr(request, "connection:");
    }
    
    if (connHeader != NULL) {
        // Check if it says "close"
        if (strstr(connHeader, "close") != NULL || strstr(connHeader, "Close") != NULL) {
            return 1;
        }
    }
    
    return 0;
}

int main(int argc, char **argv) {
    int port = 9000;
    int mode = 0;
    
    // Initialize server start time
    serverStartTime = time(NULL);
    
    // Ignore SIGPIPE to prevent crashes when client disconnects
    signal(SIGPIPE, SIG_IGN);
    
    // Ignore SIGPIPE to prevent crashes when client disconnects during streaming
    signal(SIGPIPE, SIG_IGN);
    
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
    printf("  -f : Fork mode (create new process per request)\n");
    printf("  -t : Thread mode (create new thread per request)\n");
    printf("  -p : Thread pool mode (pool of threads)\n");
    printf("  <port> : Port number (default: 9000)\n");
    printf("\n");
    printf("Example: ./myhttpd -t 9500\n");
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
    
    const char* validCreds = "cs252:password";
    
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

// Get icon filename for a file/directory
const char* getIconForFile(const char* filename, int isDir) {
    if (isDir) {
        return "/icons/menu.gif";
    }
    
    const char* ext = strrchr(filename, '.');
    if (ext == NULL) {
        return "/icons/unknown.gif";
    }
    
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "/icons/text.gif";
    } else if (strcmp(ext, ".gif") == 0) {
        return "/icons/movie.gif";
    } else if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "/icons/movie.gif";
    } else if (strcmp(ext, ".txt") == 0) {
        return "/icons/text.gif";
    } else if (strcmp(ext, ".cc") == 0 || strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0) {
        return "/icons/text.gif";
    } else {
        return "/icons/unknown.gif";
    }
}

// Comparison functions for qsort
int compareByName(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    return strcmp(fa->name, fb->name);
}

int compareByNameDesc(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    return strcmp(fb->name, fa->name);
}

int compareBySize(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    // Directories (no size) go first in ascending order
    if (fa->isDir && !fb->isDir) return -1;
    if (!fa->isDir && fb->isDir) return 1;
    
    // Both are files or both are directories
    if (fa->size < fb->size) return -1;
    if (fa->size > fb->size) return 1;
    return strcmp(fa->name, fb->name);
}

int compareBySizeDesc(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    // Directories (no size) go last in descending order
    if (fa->isDir && !fb->isDir) return 1;
    if (!fa->isDir && fb->isDir) return -1;
    
    // Both are files or both are directories
    if (fa->size > fb->size) return -1;
    if (fa->size < fb->size) return 1;
    return strcmp(fa->name, fb->name);
}

int compareByDate(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return strcmp(fa->name, fb->name);
}

int compareByDateDesc(const void* a, const void* b) {
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;
    
    if (fa->mtime > fb->mtime) return -1;
    if (fa->mtime < fb->mtime) return 1;
    return strcmp(fa->name, fb->name);
}

void serveDirectoryListing(int fd, const char* dirPath, const char* urlPath, const char* sortBy, const char* order) {
    DIR* dir = opendir(dirPath);
    if (dir == NULL) {
        const char *errorHeader = 
            "HTTP/1.1 404 File Not Found\r\n"
            "Server: CS 252 lab5\r\n"
            "Content-Type: text/html\r\n"
            "\r\n";
        write(fd, errorHeader, strlen(errorHeader));
        return;
    }
    
    // Collect all files
    FileInfo* files = (FileInfo*)malloc(sizeof(FileInfo) * 1000);
    int fileCount = 0;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files
        if (entry->d_name[0] == '.') continue;
        
        strcpy(files[fileCount].name, entry->d_name);
        
        char fullPath[2048];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, entry->d_name);
        
        struct stat fileStat;
        if (stat(fullPath, &fileStat) == 0) {
            files[fileCount].isDir = S_ISDIR(fileStat.st_mode);
            files[fileCount].size = fileStat.st_size;
            files[fileCount].mtime = fileStat.st_mtime;
        } else {
            files[fileCount].isDir = 0;
            files[fileCount].size = 0;
            files[fileCount].mtime = 0;
        }
        
        fileCount++;
    }
    closedir(dir);
    
    // Sort files
    if (sortBy != NULL) {
        if (strcmp(sortBy, "N") == 0) {
            if (order && strcmp(order, "D") == 0) {
                qsort(files, fileCount, sizeof(FileInfo), compareByNameDesc);
            } else {
                qsort(files, fileCount, sizeof(FileInfo), compareByName);
            }
        } else if (strcmp(sortBy, "S") == 0) {
            if (order && strcmp(order, "D") == 0) {
                qsort(files, fileCount, sizeof(FileInfo), compareBySizeDesc);
            } else {
                qsort(files, fileCount, sizeof(FileInfo), compareBySize);
            }
        } else if (strcmp(sortBy, "M") == 0) {
            if (order && strcmp(order, "D") == 0) {
                qsort(files, fileCount, sizeof(FileInfo), compareByDateDesc);
            } else {
                qsort(files, fileCount, sizeof(FileInfo), compareByDate);
            }
        }
    } else {
        qsort(files, fileCount, sizeof(FileInfo), compareByName);
    }
    
    // Build HTML response
    char response[65536];
    int len = 0;
    
    // Determine opposite order for links
    const char* newOrder = (order && strcmp(order, "D") == 0) ? "A" : "D";
    
    len += sprintf(response + len,
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n"
        "<head><title>Index of %s</title></head>\n"
        "<body>\n"
        "<h1>Index of %s</h1>\n"
        "<table>\n"
        "<tr>"
        "<th><a href=\"?C=N&O=%s\">Name</a></th>"
        "<th><a href=\"?C=M&O=%s\">Last modified</a></th>"
        "<th><a href=\"?C=S&O=%s\">Size</a></th>"
        "<th><a href=\"?C=D&O=%s\">Description</a></th>"
        "</tr>\n",
        urlPath, urlPath, newOrder, newOrder, newOrder, newOrder);
    
    // Parent directory link (if not at root)
    if (strcmp(urlPath, "/") != 0) {
        len += sprintf(response + len,
            "<tr><td><img src=\"/icons/menu.gif\" alt=\"[DIR]\"> "
            "<a href=\"../\">Parent Directory</a></td>"
            "<td>-</td><td>-</td><td>&nbsp;</td></tr>\n");
    }
    
    // List files
    for (int i = 0; i < fileCount; i++) {
        const char* icon = getIconForFile(files[i].name, files[i].isDir);
        
        char timeStr[64];
        struct tm* timeinfo = localtime(&files[i].mtime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", timeinfo);
        
        char sizeStr[32];
        if (files[i].isDir) {
            strcpy(sizeStr, "-");
        } else {
            sprintf(sizeStr, "%ld", (long)files[i].size);
        }
        
 
        char linkName[300];
        if (files[i].isDir) {
            sprintf(linkName, "%s/", files[i].name);
        } else {
            strcpy(linkName, files[i].name);
        }
        
        len += sprintf(response + len,
            "<tr>"
            "<td><img src=\"%s\" alt=\"[%s]\"> <a href=\"%s\">%s</a></td>"
            "<td>%s</td>"
            "<td align=\"right\">%s</td>"
            "<td>&nbsp;</td>"
            "</tr>\n",
            icon,
            files[i].isDir ? "DIR" : "FILE",
            linkName,
            files[i].name,
            timeStr,
            sizeStr);
    }
    
    len += sprintf(response + len,
        "</table>\n"
        "</body>\n"
        "</html>\n");
    
    write(fd, response, len);
    
    free(files);
}

void updateStats(const char* url, double serviceTime) {
    pthread_mutex_lock(&statsMutex);
    
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
    pthread_mutex_lock(&statsMutex);
    
    FILE* logFile = fopen(logFilePath, "a");
    if (logFile) {
        time_t now = time(NULL);
        struct tm* timeinfo = localtime(&now);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
        
        fprintf(logFile, "%s %s %s\n", timeStr, clientIP, url);
        fclose(logFile);
    }
    
    pthread_mutex_unlock(&statsMutex);
}

void serveStats(int fd) {
    char response[8192];
    int len = 0;
    
    pthread_mutex_lock(&statsMutex);
    
    time_t now = time(NULL);
    double uptime = difftime(now, serverStartTime);
    int hours = (int)(uptime / 3600);
    int minutes = ((int)uptime % 3600) / 60;
    int seconds = (int)uptime % 60;
    
    len += sprintf(response + len,
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n"
        "<head><title>Server Statistics</title></head>\n"
        "<body>\n"
        "<h1>Server Statistics</h1>\n"
        "<table border=\"1\" cellpadding=\"5\">\n"
        "<tr><td><b>Student Name:</b></td><td>Namab Kansal</td></tr>\n"
        "<tr><td><b>Server Uptime:</b></td><td>%02d:%02d:%02d</td></tr>\n"
        "<tr><td><b>Number of Requests:</b></td><td>%d</td></tr>\n",
        hours, minutes, seconds, requestCount);
    
    if (minServiceTime >= 0) {
        len += sprintf(response + len,
            "<tr><td><b>Min Service Time:</b></td><td>%.6f seconds (%s)</td></tr>\n",
            minServiceTime, minServiceURL);
    } else {
        len += sprintf(response + len,
            "<tr><td><b>Min Service Time:</b></td><td>N/A</td></tr>\n");
    }
    
    len += sprintf(response + len,
        "<tr><td><b>Max Service Time:</b></td><td>%.6f seconds (%s)</td></tr>\n"
        "</table>\n"
        "</body>\n"
        "</html>\n",
        maxServiceTime, maxServiceURL);
    
    pthread_mutex_unlock(&statsMutex);
    
    write(fd, response, len);
}

void serveLogs(int fd) {
    char response[65536];
    int len = 0;
    
    len += sprintf(response + len,
        "HTTP/1.1 200 Document follows\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<html>\n"
        "<head><title>Server Logs</title></head>\n"
        "<body>\n"
        "<h1>Server Logs</h1>\n"
        "<table border=\"1\" cellpadding=\"5\">\n"
        "<tr><th>Time</th><th>Client IP</th><th>URL</th></tr>\n");
    
    pthread_mutex_lock(&statsMutex);
    
    FILE* logFile = fopen(logFilePath, "r");
    if (logFile) {
        char line[2048];
        while (fgets(line, sizeof(line), logFile)) {
            // Parse log line: timestamp client_ip url
            char timestamp[64], clientIP[64], url[1024];
            if (sscanf(line, "%s %s %s %[^\n]", timestamp, timestamp + 11, clientIP, url) >= 3) {
                // Reconstruct timestamp
                char fullTimestamp[128];
                snprintf(fullTimestamp, sizeof(fullTimestamp), "%s %s", timestamp, timestamp + 11);
                len += sprintf(response + len,
                    "<tr><td>%s</td><td>%s</td><td>%s</td></tr>\n",
                    fullTimestamp, clientIP, url);
            }
        }
        fclose(logFile);
    } else {
        len += sprintf(response + len,
            "<tr><td colspan=\"3\">No logs available</td></tr>\n");
    }
    
    pthread_mutex_unlock(&statsMutex);
    
    len += sprintf(response + len,
        "</table>\n"
        "</body>\n"
        "</html>\n");
    
    write(fd, response, len);
}

void serveMJPEGStream(int fd) {
   
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  
    const char* header = 
        "HTTP/1.1 200 OK\r\n"
        "Server: CS 252 lab5\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    
   
    ssize_t n = write(fd, header, strlen(header));
    if (n <= 0) {
        return;
    }
    

    for (int i = 1; i <= 150; i += 3) {
        char framePath[512];
        snprintf(framePath, sizeof(framePath), "http-root-dir/stream/frame%d.jpg", i);
        
        
        int framefd = open(framePath, O_RDONLY);
        if (framefd < 0) {
            continue;
        }
        
        
        struct stat frameStat;
        if (fstat(framefd, &frameStat) < 0) {
            close(framefd);
            continue;
        }
        
        off_t frameSize = frameStat.st_size;
        
        
        if (frameSize <= 0 || frameSize > 2097152) {
            close(framefd);
            continue;
        }
        
        
        char* frameBuffer = (char*)malloc(frameSize);
        if (frameBuffer == NULL) {
            close(framefd);
            continue;
        }
        
        
        ssize_t totalRead = 0;
        while (totalRead < frameSize) {
            ssize_t bytesRead = read(framefd, frameBuffer + totalRead, frameSize - totalRead);
            if (bytesRead <= 0) {
                break;
            }
            totalRead += bytesRead;
        }
        close(framefd);
        
        
        if (totalRead != frameSize) {
            free(frameBuffer);
            continue;
        }
        
        
        char frameHeader[512];
        int headerLen = snprintf(frameHeader, sizeof(frameHeader),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %ld\r\n"
            "\r\n",
            (long)frameSize);
        
     
        if (write(fd, frameHeader, headerLen) <= 0) {
            free(frameBuffer);
            return;
        }
        
        
        if (write(fd, frameBuffer, frameSize) <= 0) {
            free(frameBuffer);
            return;
        }

        free(frameBuffer);
        
        
        usleep(50000);
    }
    

    const char* endBoundary = "--frame--\r\n";
    write(fd, endBoundary, strlen(endBoundary));
    
    
    fsync(fd);
}

void processHTTPRequest(int fd) {
    // Start timing
    clock_t startTime = clock();
    
    const int MaxRequest = 8192;
    char request[MaxRequest + 1];
    int requestLength = 0;
    
    unsigned char newChar;
    unsigned char lastChar = 0;
    unsigned char secondLastChar = 0;
    unsigned char thirdLastChar = 0;
    
    // Get client IP address
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    char clientIP[INET_ADDRSTRLEN] = "unknown";
    
    if (getpeername(fd, (struct sockaddr*)&clientAddr, &clientLen) == 0) {
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    }
    
    // Increment request count
    pthread_mutex_lock(&statsMutex);
    requestCount++;
    pthread_mutex_unlock(&statsMutex);
    
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
    
    // Log the request
    logRequest(clientIP, path);
    
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        const char *errorResponse = 
            "HTTP/1.1 400 Bad Request\r\n"
            "Server: CS 252 lab5\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Only GET and POST methods are supported\r\n";
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
    
  
    if (strcmp(path, "/stream/play") == 0) {
        serveMJPEGStream(fd);
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        return;
    }
    
    
    char originalPath[1024];
    strcpy(originalPath, path);

    char queryStr[1024] = "";
    char* queryString = strchr(path, '?');
    char sortBy[16] = "N";
    char order[16] = "A";
    
    if (queryString != NULL) {
        *queryString = '\0';
        queryString++;
        strcpy(queryStr, queryString);
        
        
        char queryStrCopy[1024];
        strcpy(queryStrCopy, queryString);
        char* token = strtok(queryStrCopy, "&");
        while (token != NULL) {
            if (strncmp(token, "C=", 2) == 0) {
                strncpy(sortBy, token + 2, sizeof(sortBy) - 1);
            } else if (strncmp(token, "O=", 2) == 0) {
                strncpy(order, token + 2, sizeof(order) - 1);
            }
            token = strtok(NULL, "&");
        }
    }
    
    sanitizePath(path);
    

    if (strncmp(path, "/cgi-bin/", 9) == 0) {
        
        char scriptPath[1024];
        
        snprintf(scriptPath, sizeof(scriptPath), "http-root-dir%s", path);
        
        
        if (access(scriptPath, X_OK) != 0) {
            const char *errorHeader = 
                "HTTP/1.1 404 File Not Found\r\n"
                "Server: CS 252 lab5\r\n"
                "Content-Type: text/html\r\n"
                "\r\n";
            write(fd, errorHeader, strlen(errorHeader));
            
            const char *errorBody = 
                "<html><body>\r\n"
                "<h1>404 CGI Script Not Found</h1>\r\n"
                "</body></html>\r\n";
            write(fd, errorBody, strlen(errorBody));
            return;
        }
        
       
        char postData[8192] = "";
        int contentLength = 0;
        
        if (strcmp(method, "POST") == 0) {
            contentLength = getContentLength(request);
            
            if (contentLength > 0 && contentLength < sizeof(postData)) {
                
                int totalRead = 0;
                while (totalRead < contentLength) {
                    int n = read(fd, postData + totalRead, contentLength - totalRead);
                    if (n <= 0) break;
                    totalRead += n;
                }
                postData[totalRead] = '\0';
            }
        }
        
        
        int pipefd[2];
        if (strcmp(method, "POST") == 0 && contentLength > 0) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                return;
            }
        }
        
       
        pid_t pid = fork();
        
        if (pid == 0) {
           
            
          
            setenv("REQUEST_METHOD", method, 1);
            setenv("QUERY_STRING", queryStr, 1);
            
            if (strcmp(method, "POST") == 0) {
                char contentLengthStr[32];
                sprintf(contentLengthStr, "%d", contentLength);
                setenv("CONTENT_LENGTH", contentLengthStr, 1);
                
                
                if (contentLength > 0) {
                    close(pipefd[1]); 
                    dup2(pipefd[0], STDIN_FILENO);
                    close(pipefd[0]);
                }
            }
            
            
            dup2(fd, STDOUT_FILENO);
            
            
            const char *header = 
                "HTTP/1.1 200 Document follows\r\n"
                "Server: CS 252 lab5\r\n";
            write(STDOUT_FILENO, header, strlen(header));
            
           
            execl(scriptPath, scriptPath, NULL);
            
           
            perror("execl");
            exit(1);
        } else if (pid > 0) {
         
            
           
            if (strcmp(method, "POST") == 0 && contentLength > 0) {
                close(pipefd[0]); 
                write(pipefd[1], postData, strlen(postData));
                close(pipefd[1]); 
            }
            
         
            int status;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
        
        
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(path, serviceTime);
        
        return;
    }
    
    
    int hadTrailingSlash = 0;
    int pathLen = strlen(path);
    if (pathLen > 1 && path[pathLen - 1] == '/') {
        path[pathLen - 1] = '\0';
        hadTrailingSlash = 1;
    }
    
    char filePath[2048] = "http-root-dir";
    
    if (strcmp(path, "/") == 0 || strlen(path) == 0) {
        strcat(filePath, "/htdocs/index.html");
    } else {
        if (strncmp(path, "/htdocs", 7) != 0 && strncmp(path, "/icons", 6) != 0 && strncmp(path, "/cgi-bin", 8) != 0) {
            strcat(filePath, "/htdocs");
        }
        strcat(filePath, path);
    }
    
   
    struct stat fileStat;
    if (stat(filePath, &fileStat) == 0 && S_ISDIR(fileStat.st_mode)) {
        
        char urlPath[1024];
        strcpy(urlPath, path);
        if (!hadTrailingSlash && strcmp(path, "/") != 0) {
            strcat(urlPath, "/");
        }
        serveDirectoryListing(fd, filePath, urlPath, sortBy, order);
        
       
        clock_t endTime = clock();
        double serviceTime = ((double)(endTime - startTime)) / CLOCKS_PER_SEC;
        updateStats(urlPath, serviceTime);
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