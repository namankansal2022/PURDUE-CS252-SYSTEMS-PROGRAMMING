# CS 252 – Systems Programming

**Student:** Naman Kansal (kansal2)

---

## Overview

This repository contains my implementations for the five laboratory projects from  
**CS 252 – Systems Programming** at Purdue University.

Each lab focuses on a core operating systems or systems programming concept and is
implemented using **C, C++, and shell scripting**, with an emphasis on low-level system
behavior, correctness, and performance.

---

## Labs Included

### Lab 1 – Custom Memory Allocator
- Implemented a user-level replacement for `malloc()` and `free()`
- Managed heap memory using metadata headers and multiple free lists
- Supported block splitting, coalescing, and fragmentation reduction

---

### Lab 2 – Shell Scripting Utilities
- Developed multiple Bash scripts for common system administration tasks
- Implemented a password strength checker using regular expressions
- Built directory backup and process monitoring utilities

---

### Lab 3 – Unix Shell
- Implemented a custom Unix-style command shell in C++
- Supported command parsing, execution, pipes, redirection, and background jobs
- Used Flex (Lex) and Bison (Yacc) for lexical analysis and grammar parsing

---

### Lab 4 – Threads and Synchronization
- Explored POSIX threads, mutexes, spin locks, and semaphores
- Implemented multithreaded programs and resolved race conditions
- Studied deadlocks, bounded buffers, and buffer overflow vulnerabilities

---

### Lab 5 – HTTP Server
- Built a multithreaded HTTP 1.0 server from scratch
- Supported GET and POST requests, authentication, and CGI execution
- Implemented multiple concurrency models and MJPEG streaming

---

## Repository Structure

```text
lab1/   Custom memory allocator
lab2/   Shell scripting utilities
lab3/   Unix shell implementation
lab4/   Threading and synchronization programs
lab5/   HTTP server implementation
