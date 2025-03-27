#!/bin/bash
SCRIPT_DIR="$(dirname $BASH_SOURCE[0])"
ROOT_DIR="${SCRIPT_DIR}/../.."
ABI_JSON="${SCRIPT_DIR}/native_system_abi.json"
BIN_CPP_TEMP="${SCRIPT_DIR}/system_contract_abi_bin.cpp.template"
BIN_CPP="${ROOT_DIR}/libraries/chain/eosio_contract_abi_bin.cpp"
CLIENT=${CLIENT:-"${ROOT_DIR}/build/bin/fucli"}

ABI_PACKED=$( ${CLIENT} convert pack_abi "${ABI_JSON}" )

ABI_PACKED_LEN=$(( $(echo -n ${ABI_PACKED} | wc -c) / 2 ))
echo ${ABI_PACKED} | sed 's/\(........................\)/\1\n/g' \
     | sed 's/\(..\)/0x\1, /g' | sed 's/^/   /' | sed 's/ $//' | sed '$s/,$//' \
     > /tmp/system_contract_abi_bin.txt



cat ${BIN_CPP_TEMP} \
    | sed "/@NATIVE_SYSTEM_ABI_BIN_DATA@/ {
        r /tmp/system_contract_abi_bin.txt
        d
    }"  \
    | sed "s/@NATIVE_SYSTEM_ABI_BIN_LEN@/${ABI_PACKED_LEN}/" \
    > ${BIN_CPP}


rm /tmp/system_contract_abi_bin.txt