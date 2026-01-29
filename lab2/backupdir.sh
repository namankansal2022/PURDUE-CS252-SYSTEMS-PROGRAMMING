#!/bin/bash

# DO NOT REMOVE THE FOLLOWING LINES
git add $0 >> .local.git.out
git commit -a -m "Lab 2 commit" >> .local.git.out
git push >> .local.git.out || echo

# check if not enough arguments
if [ $# -lt 3 ]; then
    echo "Not enough arguments"
    exit 1
fi

# check if arguments are not exactly 3
if [ $# -ne 3 ]; then
    echo "Usage: $0 <dir> <backup-dir> <max-backups>"
    exit 1
fi

# check if source directory exists
if [ ! -d "$1" ]; then
    echo "Error: directory does not exist."
    exit 1
fi

# assign variables
src="$1"
bdir="$2"
max="$3"

# check if max is a positive integer
case "$max" in ''|*[!0-9]*) echo "Error: max-backups must be a positive integer."; exit 1;; esac
if [ "$max" -lt 1 ]; then echo "Error: max-backups must be >= 1."; exit 1; fi

# name of the source folder
name="$(basename "$src")"
mkdir -p "$bdir"

# find the last backup number
last=-1
for d in "$bdir"/"$name".*; do
  [ -d "$d" ] || continue
  s="${d##*.}"
  case "$s" in ''|*[!0-9]*) continue;; esac
  if [ "$s" -gt "$last" ]; then last="$s"; fi
done

# if no backup exists, create first one (.0)
if [ "$last" -lt 0 ]; then
  cp -r "$src" "$bdir/$name.0" || { echo "Error: backup failed."; exit 1; }
  echo "Backup created: $bdir/$name.0"
  exit 0
fi

# if nothing changed, skip
if diff -qr "$src" "$bdir/$name.$last" > /dev/null; then
  echo "No backup necessary"
  exit 0
fi

# create next backup
next=$((last + 1))
cp -r "$src" "$bdir/$name.$next" || { echo "Error: backup failed."; exit 1; }
echo "Backup created: $bdir/$name.$next"

# count how many backups exist
count=0
for d in "$bdir"/"$name".*; do
  [ -d "$d" ] || continue
  count=$((count + 1))
done

# if backups exceed max, delete oldest ones
if [ "$count" -gt "$max" ]; then
  ids=$(ls -d "$bdir"/"$name".* 2>/dev/null | awk -F'.' '{print $NF}' | grep -E '^[0-9]+$' | sort -n)
  remove=$((count - max))
  for s in $ids; do
    [ "$remove" -le 0 ] && break
    rm -rf "$bdir/$name.$s"
    remove=$((remove - 1))
  done
fi
