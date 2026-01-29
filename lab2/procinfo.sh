#!/bin/bash

#Do not insert code here

#DO NOT REMOVE THE FOLLOWING TWO LINES
git add $0 >> .local.git.out     # add this script file to git
git commit -a -m "Lab 2 commit" >> .local.git.out   # commit with message
git push >> .local.git.out || echo   # push changes, if error just echo

# procinfo [-t secs] pattern
#
# prints PID, CMD, USER, Memory Usage, CPU time, and number of threads 
# of processes with a command # that matches "pattern"
#
# If the [-t secs] option is passed, then it will loop and print the information
# every "secs" secods.
#
# If no pattern is given, it prints an error that a pattern is missing.
#

tflag=0          # flag to check if -t option is given
interval=0       # store interval value (default 0)

# check if first argument is -t
if [ "$1" = "-t" ]; then
  tflag=1        # set flag to 1 if -t is used
  interval="$2"  # save the number of seconds given after -t
  shift 2        # remove first two arguments (-t and secs) from list
fi

pattern="$1"     # next argument is the pattern to search for
if [ -z "$pattern" ]; then   # if pattern is missing
  echo "Error: pattern missing"
  exit 1
fi

# function to show process info one time
list_once() {
  # find PIDs matching pattern, exclude grep and this script
  pids=$(ps -e -o pid,cmd | grep "$pattern" | grep -v -e "grep" -e "$0" | awk '{print $1}')

  if [ -z "$pids" ]; then   # if no process found
    echo "No matching processes"
    return
  fi

  # print header line
  printf "%8s %-15s %-12s %-10s %-10s %-8s\n" "PID" "CMD" "USER" "MEM" "CPU" "THREADS"

  # loop through each process id
  for pid in $pids; do
    if [ -d "/proc/$pid" ]; then    # check if process folder exists
      cmd=$(cat /proc/$pid/comm 2>/dev/null)   # command name
      user=$(ps -p $pid -o user=)              # user running it

      # memory usage in KB → convert to MB
      mem_kb=$(grep -m1 '^VmRSS:' /proc/$pid/status 2>/dev/null | awk '{print $2}')
      if [ -z "$mem_kb" ]; then mem_kb=0; fi
      mem_mb=$(( mem_kb / 1024 ))

      # CPU usage in seconds
      clk=$(getconf CLK_TCK 2>/dev/null)       # ticks per second
      if [ -z "$clk" ]; then clk=100; fi
      jiffies=$(awk '{print $14 + $15}' /proc/$pid/stat 2>/dev/null) # CPU ticks
      if [ -z "$jiffies" ]; then jiffies=0; fi
      cpu_secs=$(( jiffies / clk ))

      # number of threads
      threads=$(grep -m1 '^Threads:' /proc/$pid/status 2>/dev/null | awk '{print $2}')
      if [ -z "$threads" ]; then threads=0; fi

      # print process info in formatted way
      printf "%8s (%-12s) %-12s %6s MB %8s s %5s Thr\n" "$pid" "$cmd" "$user" "$mem_mb" "$cpu_secs" "$threads"
    fi
  done
}

# if -t was used, run continuously
if [ "$tflag" -eq 1 ]; then
  while true; do
    clear         # clear screen before showing again
    list_once     # show process info
    sleep "$interval"   # wait for given seconds
  done
else
  list_once       # run just once
fi
