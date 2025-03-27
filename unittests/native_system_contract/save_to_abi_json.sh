#!/bin/bash
SCRIPT_DIR="$(dirname $BASH_SOURCE[0])"
ROOT_DIR="$(dirname ${SCRIPT_DIR}/../..)"
ABI_JSON="${SCRIPT_DIR}/native_system_abi.json"
BIN_CPP="${ROOT_DIR}/libraries/chain/eosio_contract_abi_bin.cpp"
CLIENT=${CLIENT:-"${ROOT_DIR}/build/bin/fucli"}

ABI_PACKED=$( cat ${BIN_CPP} | tr '\n' ' ' | sed -n 's/^.*eosio_abi_bin[^{}]*{\([^{}]*\)}.*$/\1/p' | sed 's/\(,\| \|0x\)//g' )

${CLIENT} convert unpack_abi "${ABI_PACKED}" > ${ABI_JSON}
