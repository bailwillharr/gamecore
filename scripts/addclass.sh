#!/bin/sh

if [ "$#" -ne "1" ]; then
    echo "usage $0 CamelCaseClassName"
    exit 1
fi

if [ ! -d "gamecore/src" ] && [ ! -d "gamecore/include/gamecore" ] && [ ! -f "gamecore/CMakeLists.txt" ]; then
    echo "must be run from repo root!"
    exit 1
fi

class_name=$1
snake_case=$(printf "%s" "${class_name}" | sed 's/\([A-Z]\)/_\L\1/g' | sed 's/^_//')

source_filename="gc_${snake_case}.cpp"
include_filename="gc_${snake_case}.h"

source_filepath="gamecore/src/${source_filename}"
include_filepath="gamecore/include/gamecore/${include_filename}"

if [ -e "${source_filepath}" ]; then
    echo "${source_filepath} cannot already exist!"
    exit 1
fi

if [ -e "${include_filepath}" ]; then
    echo "${include_filepath} cannot already exist!"
    exit 1
fi

echo "creating ${source_filepath}"

cat >"${source_filepath}" <<__EOF__
#include "gamecore/${include_filename}"

namespace gc {

${class_name}::${class_name}() {}

} // namespace gc
__EOF__

cat >"${include_filepath}" <<__EOF__
#pragma once

namespace gc {

class ${class_name} {
public:
    ${class_name}();
};

} // namespace gc
__EOF__

cmake_file="gamecore/CMakeLists.txt"

sed "/^set(SRC_FILES/,/^[)]/ {
  /^[)]/ i\\
  \"src/${source_filename}\"
}" "$cmake_file" >"$cmake_file.tmp" && mv "$cmake_file.tmp" "$cmake_file"

echo "creating ${include_filepath}"

sed "/^set(INCLUDE_FILES/,/^[)]/ {
  /^[)]/ i\\
  \"include/gamecore/${include_filename}\"
}" "$cmake_file" >"$cmake_file.tmp" && mv "$cmake_file.tmp" "$cmake_file"
