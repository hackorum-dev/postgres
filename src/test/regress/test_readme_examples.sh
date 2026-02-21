#!/bin/bash
#
# test_readme_examples.sh - Test pattern matching examples from README.md
#
# Extracts Pattern/Accepts/Rejects blocks and tests them against
# the pattern matching implementation. Saves test files to .test_pattern_files/
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
README="$SCRIPT_DIR/README.md"
PG_REGRESS="$SCRIPT_DIR/pg_regress"
TESTDIR="$SCRIPT_DIR/.test_pattern_files"
RED="\033[0;31m"
GREEN="\033[0;32m"
RESET="\033[0m"
# Create test directory
mkdir -p "$TESTDIR"

# Build pg_regress if needed
if [ ! -x "$PG_REGRESS" ]; then
    echo "Building pg_regress..."
    make -C "$SCRIPT_DIR" pg_regress >/dev/null 2>&1
fi

echo "=== Testing Pattern Matching Examples from README.md ==="
echo ""

# State variables
pattern_label=""
pattern_line=0
pattern_num=0
passed=0
failed=0
skipped=0

# Block parsing state
block_type=""
block_label=""
block_content=""
block_start_line=0
in_block=false

line_num=0
while IFS= read -r line || [[ -n "$line" ]]; do
    ((line_num++))

    # Check for **Pattern/Accepts/Rejects** marker
    if [[ "$line" =~ ^\*\*(Pattern|Accepts|Rejects):?[[:space:]]*([^*]*)\*\*[[:space:]]*$ ]]; then
        block_type="${BASH_REMATCH[1]}"
        block_label="${BASH_REMATCH[2]}"
        # Trim whitespace
        block_label="${block_label#"${block_label%%[![:space:]]*}"}"
        block_label="${block_label%"${block_label##*[![:space:]]}"}"
        continue
    fi

    # Check for ``` start after seeing a marker
    if [[ -n "$block_type" && "$line" == '```' && "$in_block" == false ]]; then
        in_block=true
        block_content=""
        block_start_line=$line_num
        continue
    fi

    # Check for ``` end
    if [[ "$in_block" == true && "$line" == '```' ]]; then
        in_block=false

        if [[ "$block_type" == "Pattern" ]]; then
            ((pattern_num++))
            if [[ -n "$block_label" ]]; then
                pattern_label="$block_label"
            else
                pattern_label="pattern_$pattern_num"
            fi
            pattern_line=$block_start_line

            # Save pattern file
            printf '%s' "$block_content" > "$TESTDIR/pattern-L$pattern_line.out"

        elif [[ -n "$pattern_label" ]]; then
            test_type=$(echo "$block_type" | tr '[:upper:]' '[:lower:]')
            location="README.md:$block_start_line"
            pattern_file="$TESTDIR/pattern-L$pattern_line.out"
            test_file="$TESTDIR/L$pattern_line-$test_type-L$block_start_line.out"
            diff_file="$TESTDIR/L$pattern_line-$test_type-L$block_start_line.diff"
            pattern_loc="README.md:$pattern_line"

            # Save test file
            printf '%s' "$block_content" > "$test_file"

            # Run comparison
            if "$PG_REGRESS" --compare "$pattern_file" "$test_file" > "$diff_file" ; then
                pg_regress_result=Accepts
            else
                pg_regress_result=Rejects
            fi


            if [[ "$block_type" == "$pg_regress_result" ]]; then
                test_status="$GREEN[PASS]$RESET"
                ((passed++))
            else
                test_status="$RED[FAIL]$RESET"
                ((failed++))
            fi
            echo -e "$test_status $pattern_label ($pattern_loc) $test_type ($location)"
            ((passed++))
        fi

        block_type=""
        continue
    fi

    # Accumulate block content
    if [[ "$in_block" == true ]]; then
        if [[ -n "$block_content" ]]; then
            block_content+=$'\n'
        fi
        block_content+="$line"
    fi
done < "$README"

echo ""
echo "=== Results ==="
echo -e "Passed:  $GREEN$passed$RESET"
echo -e "Failed:  $RED$failed$RESET"
echo "Files:   .test_pattern_files/"

[[ $failed -eq 0 ]]
