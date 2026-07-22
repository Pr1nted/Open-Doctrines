#!/bin/bash

OUTPUT_FILE="combined_text_output.txt"
SEARCH_DIR="${1:-.}"

# Clear the output file
> "$OUTPUT_FILE"

echo "Fast-scanning '$SEARCH_DIR' for text files (ignoring IDE/Build folders)..."

# 1. Use grep -R to rapidly find ONLY text files in one single process.
# 2. --null separates file names safely (handles spaces).
# 3. Exclude CLion, CMake, Git, and standard build folders.
grep -RIl --null \
    --exclude-dir=".git" \
    --exclude-dir=".idea" \
    --exclude-dir="cmake-build-debug" \
    --exclude-dir="cmake-build-release" \
    --exclude-dir="build" \
    --exclude-dir=".cache" \
    "" "$SEARCH_DIR" 2>/dev/null | while IFS= read -r -d $'\0' file; do

    # Print the header and append the file contents
    echo "==================================================" >> "$OUTPUT_FILE"
    echo "FILE: $file" >> "$OUTPUT_FILE"
    echo "==================================================" >> "$OUTPUT_FILE"
    cat "$file" 2>/dev/null >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"

done

echo "Done! Output saved to '$OUTPUT_FILE'."