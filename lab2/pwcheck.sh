#!/bin/bash

# DO NOT REMOVE THE FOLLOWING LINES
git add $0 >> .local.git.out
git commit -a -m "Lab 2 commit" >> .local.git.out
git push >> .local.git.out || echo

# Print usage instructions and exit
usage() {
    echo "./pwcheck.sh [-f passwordFile] [password1 password2 password3...]"
    exit 1
}

# Variables
file=""          # filename if -f option is used
pwlist=()        # list of passwords to check

# Parse command line options (-f for file)
while getopts "f:" opt; do
  case $opt in
    f) file="$OPTARG" ;;
    *) usage ;;
  esac
done
shift $((OPTIND -1))

# If -f option was given, read passwords from file
if [[ -n "$file" ]]; then
    if [[ -f "$file" ]]; then
        while IFS= read -r line; do
            pwlist+=("$line")
        done < "$file"
    else
        echo "Error: File $file not found."
        exit 1
    fi
fi

# Add any extra passwords passed directly on the command line
for pw in "$@"; do
    pwlist+=("$pw")
done

# If no passwords provided at all, show usage
if [[ ${#pwlist[@]} -eq 0 ]]; then
    usage
fi

# Print table header
printf "%-21s    %s\n" "Password" "Score"
printf "%-21s    %s\n" "---------------------" "-----"

# Main password checking loop
for pw in "${pwlist[@]}"; do
    len=${#pw}

    # Check length validity
    if (( len < 6 || len > 32 )); then
        printf "%-21s    %s\n" "$pw" "Error: Password length invalid."
        continue
    fi

    # Collect errors if requirements not met
    errors=()
    [[ ! $pw =~ [0-9] ]] && errors+=("Error: Password should include at least one number \"0-9\"")
    [[ ! $pw =~ [#$+%@^*/-] ]] && errors+=("Error: Password should include at least one of \"#$+%@^*-/\"")
    ([[ ! $pw =~ [A-Z] ]] || [[ ! $pw =~ [a-z] ]]) && errors+=("Error: Passwords should have at least one Uppercase and lowercase alphabetic character.")

    # If errors found, print them and skip scoring
    if (( ${#errors[@]} > 0 )); then
        for err in "${errors[@]}"; do
            printf "%-21s    %s\n" "$pw" "$err"
        done
        continue
    fi

    # Score calculation
    score=$len
    digits=$(grep -o -E '[0-9]' <<<"$pw" | wc -l)
    alphas=$(grep -o -E '[A-Za-z]' <<<"$pw" | wc -l)
    specials=$(grep -o -E '[#$+%@^*/-]' <<<"$pw" | wc -l)
    (( score += 2*digits + alphas + 2*specials ))

    # Penalties for bad patterns
    grep -q -E '([A-Za-z0-9])\1+' <<<"$pw" && (( score -= 10 ))   # repeated chars
    grep -q -E '[a-z]{3,}' <<<"$pw" && (( score -= 3 ))           # 3+ lowercase in a row
    grep -q -E '[A-Z]{3,}' <<<"$pw" && (( score -= 3 ))           # 3+ uppercase in a row
    grep -q -E '[0-9]{3,}' <<<"$pw" && (( score -= 3 ))           # 3+ digits in a row

    # Print final password and score
    printf "%-21s    %d\n" "$pw" "$score"
done
