#!/bin/bash

# variables
SCRIPT_ROOT="$(dirname $BASH_SOURCE[0])"
GIT_ROOT=${GIT_ROOT:-"${SCRIPT_ROOT}/.."}

echo 'Start update sys test contracts...'

files=($( cd "$GIT_ROOT/build" && find libraries/testing/contracts -type f \( -name "*.wasm" -o -name "*.abi" \) | grep -v ./CMakeFiles ))
files=(${files[@]} $( cd "$GIT_ROOT/" && find unittests/contracts -type f \( -name "*.wasm" -o -name "*.abi" \) | grep -v ./CMakeFiles ))

for f in ${files[*]}
do
    cp -v "$GIT_ROOT/build/${f}" "$GIT_ROOT/${f}"
done

echo 'Done update sys test contracts.'

