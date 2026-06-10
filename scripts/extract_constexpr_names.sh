#!/bin/bash
# extract_name_literals.sh
# Extracts string literals passed to Name::createConstexpr for your debug mapping file

set -euo pipefail

# Configuration
SEARCH_DIRS="${2:-.}"

# Find C/C++ files
find "$SEARCH_DIRS" -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cc" -o -name "*.cxx" \) -print0 | \
xargs -0 grep -E -o 'Name::createConstexpr\s*\(\s*("[^"]*")' | \
sed -E 's/.*Name::createConstexpr\s*\(\s*(".*")/\1/' | \
sed 's/^"//;s/"$//' | \
sort -u

# Also catch gc::Name::createConstexpr if used
find "$SEARCH_DIRS" -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.c" -o -name "*.cc" -o -name "*.cxx" \) -print0 | \
xargs -0 grep -E -o 'gc::Name::createConstexpr\s*\(\s*("[^"]*")' | \
sed -E 's/.*gc::Name::createConstexpr\s*\(\s*(".*")/\1/' | \
sed 's/^"//;s/"$//' | \
sort -u
