CS 252 Lab 5 - HTTP Server
Student: Naman Kansal (kansal2)

FEATURES IMPLEMENTED

Base Features
- HTTP 1.0 Protocol (GET and POST)
- Basic Authentication
- Directory Listing with sorting
- CGI-BIN Support
- Statistics and Logging
- Concurrency Modes: Iterative, Fork (-f), Thread (-t), Pool (-p)
- Memory leak prevention
- Path traversal protection
- Zombie process cleanup
- SIGPIPE handling

Extra Features
- CGI-POST Support
- MJPEG Streaming

BUILDING AND RUNNING

Compile:
  make clean
  make

Run:
  ./myhttpd <port>              (iterative)
  ./myhttpd -f <port>           (fork)
  ./myhttpd -t <port>           (thread)
  ./myhttpd -p <port>           (pool)

Example:
  ./myhttpd -t 8889

Access:
  http://data.cs.purdue.edu:8889/
  Username: cs252
  Password: password

ENDPOINTS
  /                     Main index
  /stats                Statistics
  /logs                 Logs
  /cgi-bin/test-env     CGI test
  /cgi-bin/jj           Pizza form
  /stream.html          MJPEG streaming
  /dir1/                Directory browsing

TESTING

Fork Mode:
  ./myhttpd -f 8889
  ps -u $USER | grep defunct | wc -l
  Expected: 0

Thread Mode:
  ./myhttpd -t 8889
  ps -u $USER o nlwp,pid,cmd | grep myhttpd
  Expected: NLWP > 1

Pool Mode:
  ./myhttpd -p 8889
  ps -u $USER o nlwp,pid,cmd | grep myhttpd
  Expected: NLWP = 6

Valgrind:
  valgrind --leak-check=full --track-fds=yes ./myhttpd -t 8889
  Expected: definitely lost = 0 bytes

Path Traversal:
  curl --user cs252:password --path-as-is http://data.cs.purdue.edu:8889/../myhttpd.cc
  Expected: 404 File Not Found