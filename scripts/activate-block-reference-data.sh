#!/usr/bin/env bash

set -Eeuo pipefail

FEATURE_CODENAME="BLOCK_REFERENCE_DATA"
FEATURE_DESCRIPTION_DIGEST="6fe9ee791b80476589a767b93d01ba778c5f774d366e8f377140d96cb7b3e28c"

MODE="${1:-check}"
NODE_URL="${NODE_URL:-}"
PRODUCER_URLS="${PRODUCER_URLS:-${NODE_URL}}"
VERIFY_NODE_URLS="${VERIFY_NODE_URLS:-${NODE_URL}}"
EXPECTED_CHAIN_ID="${EXPECTED_CHAIN_ID:-}"
SYSTEM_ACCOUNT="${SYSTEM_ACCOUNT:-flon}"
SYSTEM_PERMISSION="${SYSTEM_PERMISSION:-active}"
WALLET_URL="${WALLET_URL:-unix:///tmp/fuwal.sock}"
FUCLI_BIN="${FUCLI_BIN:-fucli}"
ACTIVATION_TIMEOUT_SECONDS="${ACTIVATION_TIMEOUT_SECONDS:-900}"
POLL_INTERVAL_SECONDS="${POLL_INTERVAL_SECONDS:-3}"
CURL_CONNECT_TIMEOUT_SECONDS="${CURL_CONNECT_TIMEOUT_SECONDS:-5}"
CURL_TIMEOUT_SECONDS="${CURL_TIMEOUT_SECONDS:-20}"
CONFIRM_ACTIVATION="${CONFIRM_ACTIVATION:-}"

usage() {
   cat <<'EOF'
Usage:
  NODE_URL=<chain-api> PRODUCER_URLS=<private-producer-api>[,...] \
    ./scripts/activate-block-reference-data.sh check

  NODE_URL=<chain-api> PRODUCER_URLS=<private-producer-api>[,...] \
    VERIFY_NODE_URLS=<chain-api>[,...] EXPECTED_CHAIN_ID=<chain-id> \
    WALLET_URL=unix:///tmp/fuwal.sock CONFIRM_ACTIVATION=BLOCK_REFERENCE_DATA \
    ./scripts/activate-block-reference-data.sh activate

  NODE_URL=<chain-api> VERIFY_NODE_URLS=<chain-api>[,...] \
    ./scripts/activate-block-reference-data.sh verify

Modes:
  check     Read-only checks on chain identity, system ABI, feature support,
            and current activation state.
  activate  Submit flon::activate, then wait until the activation block is
            irreversible on every VERIFY_NODE_URLS endpoint.
  verify    Read-only verification that the feature is activated and irreversible.

Notes:
  - PRODUCER_URLS must reach producer_api_plugin on every block-producing node.
    Keep those endpoints private; do not expose producer write APIs publicly.
  - The wallet must already be unlocked. This script never accepts or reads a
    wallet password.
  - FullOn may take up to 12 seconds to produce an idle block. The default
    activation timeout is 900 seconds so activation can also advance into LIB.
EOF
}

log() {
   printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

die() {
   printf 'ERROR: %s\n' "$*" >&2
   exit 1
}

require_command() {
   command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

post_json() {
   local base_url="$1"
   local path="$2"
   local payload="$3"

   curl --fail --silent --show-error \
      --connect-timeout "$CURL_CONNECT_TIMEOUT_SECONDS" \
      --max-time "$CURL_TIMEOUT_SECONDS" \
      -H 'Content-Type: application/json' \
      --data "$payload" \
      "${base_url%/}${path}"
}

split_urls() {
   printf '%s\n' "$1" | tr ', ' '\n\n' | sed '/^$/d'
}

get_info() {
   post_json "$1" /v1/chain/get_info '{}'
}

get_supported_features() {
   post_json "$1" /v1/producer/get_supported_protocol_features \
      '{"exclude_disabled":false,"exclude_unactivatable":false}'
}

get_activated_features() {
   post_json "$1" /v1/chain/get_activated_protocol_features \
      '{"limit":1000,"search_by_block_num":false,"reverse":true}'
}

feature_record_by_codename() {
   local codename="$1"
   jq -c --arg codename "$codename" \
      '.[] | select(any(.specification[]?; .name == "builtin_feature_codename" and .value == $codename))' \
      | head -n 1
}

activation_record_by_digest() {
   local feature_digest="$1"
   jq -c --arg feature_digest "$feature_digest" \
      '.activated_protocol_features[]? | select(.feature_digest == $feature_digest)' \
      | head -n 1
}

assert_chain_id() {
   local label="$1"
   local info_json="$2"
   local chain_id

   chain_id="$(jq -er '.chain_id' <<<"$info_json")"
   [[ "$chain_id" == "$CHAIN_ID" ]] || \
      die "$label is on chain $chain_id, expected $CHAIN_ID"
}

check_system_contract() {
   local abi_json

   abi_json="$(post_json "$NODE_URL" /v1/chain/get_abi \
      "{\"account_name\":\"${SYSTEM_ACCOUNT}\"}")"
   jq -e --arg action activate \
      '(.abi.actions // []) | any(.name == $action)' \
      >/dev/null <<<"$abi_json" || \
      die "${SYSTEM_ACCOUNT} ABI does not expose the activate action"

   log "system contract ${SYSTEM_ACCOUNT} exposes activate"
}

check_preactivate_feature() {
   local activated_json="$1"

   jq -e --arg codename PREACTIVATE_FEATURE \
      '.activated_protocol_features[]? |
       select(any(.specification[]?; .name == "builtin_feature_codename" and .value == $codename))' \
      >/dev/null <<<"$activated_json" || \
      die "PREACTIVATE_FEATURE is not active on chain"

   log "PREACTIVATE_FEATURE is active"
}

check_producer_support() {
   local producer_url info_json supported_json record digest description enabled dependencies
   local common_digest=""
   local producer_count=0

   while IFS= read -r producer_url; do
      producer_count=$((producer_count + 1))
      info_json="$(get_info "$producer_url")"
      assert_chain_id "producer endpoint $producer_url" "$info_json"

      supported_json="$(get_supported_features "$producer_url")" || \
         die "cannot query producer protocol features from $producer_url"
      record="$(feature_record_by_codename "$FEATURE_CODENAME" <<<"$supported_json")"
      [[ -n "$record" ]] || \
         die "$FEATURE_CODENAME is not recognized by producer endpoint $producer_url"

      digest="$(jq -er '.feature_digest' <<<"$record")"
      description="$(jq -er '.description_digest' <<<"$record")"
      enabled="$(jq -er '.subjective_restrictions.enabled' <<<"$record")"
      dependencies="$(jq -er '.dependencies | length' <<<"$record")"

      [[ "$description" == "$FEATURE_DESCRIPTION_DIGEST" ]] || \
         die "$producer_url has an unexpected $FEATURE_CODENAME description digest: $description"
      [[ "$enabled" == "true" ]] || \
         die "$FEATURE_CODENAME is disabled on producer endpoint $producer_url"
      [[ "$dependencies" == "0" ]] || \
         die "$producer_url reports unexpected dependencies for $FEATURE_CODENAME"

      if [[ -z "$common_digest" ]]; then
         common_digest="$digest"
      elif [[ "$digest" != "$common_digest" ]]; then
         die "producer feature digest mismatch: $producer_url has $digest, expected $common_digest"
      fi

      log "producer ready: $producer_url, feature digest $digest"
   done < <(split_urls "$PRODUCER_URLS")

   [[ "$producer_count" -gt 0 ]] || die "PRODUCER_URLS is empty"
   [[ -n "$common_digest" ]] || die "could not resolve $FEATURE_CODENAME digest"
   FEATURE_DIGEST="$common_digest"
}

activation_state() {
   local node_url="$1"
   local info_json activated_json record activation_block lib

   info_json="$(get_info "$node_url")"
   assert_chain_id "verification endpoint $node_url" "$info_json"
   activated_json="$(get_activated_features "$node_url")"
   record="$(activation_record_by_digest "$FEATURE_DIGEST" <<<"$activated_json")"
   if [[ -z "$record" ]]; then
      printf 'missing'
      return 0
   fi

   activation_block="$(jq -er '.activation_block_num' <<<"$record")"
   lib="$(jq -er '.last_irreversible_block_num' <<<"$info_json")"
   if (( lib >= activation_block )); then
      printf 'irreversible:%s:%s' "$activation_block" "$lib"
   else
      printf 'reversible:%s:%s' "$activation_block" "$lib"
   fi
}

wait_for_irreversible_activation() {
   local node_url="$1"
   local deadline state state_data activation_block lib

   deadline=$(( $(date +%s) + ACTIVATION_TIMEOUT_SECONDS ))
   while (( $(date +%s) <= deadline )); do
      state="$(activation_state "$node_url")"
      case "$state" in
         irreversible:*)
            state_data="${state#irreversible:}"
            activation_block="${state_data%%:*}"
            lib="${state_data##*:}"
            log "$node_url activation is irreversible (activation_block=$activation_block lib=$lib)"
            return 0
            ;;
         reversible:*)
            state_data="${state#reversible:}"
            activation_block="${state_data%%:*}"
            lib="${state_data##*:}"
            log "$node_url activation is visible but not irreversible yet (activation_block=$activation_block lib=$lib)"
            ;;
         missing)
            log "$node_url is waiting for $FEATURE_CODENAME activation"
            ;;
         *)
            die "unexpected activation state from $node_url: $state"
            ;;
      esac
      sleep "$POLL_INTERVAL_SECONDS"
   done

   die "timed out after ${ACTIVATION_TIMEOUT_SECONDS}s waiting for irreversible activation on $node_url"
}

verify_all_nodes() {
   local verify_url verify_count=0

   while IFS= read -r verify_url; do
      verify_count=$((verify_count + 1))
      wait_for_irreversible_activation "$verify_url"
   done < <(split_urls "$VERIFY_NODE_URLS")

   [[ "$verify_count" -gt 0 ]] || die "VERIFY_NODE_URLS is empty"
}

submit_activation() {
   local wallet_list

   require_command "$FUCLI_BIN"
   wallet_list="$("$FUCLI_BIN" --wallet-url "$WALLET_URL" --no-auto-wallet wallet list 2>&1)" || \
      die "cannot query fuwal at $WALLET_URL; unlock the intended wallet first"
   grep -Fq '*' <<<"$wallet_list" || \
      die "no unlocked wallet reported by fuwal at $WALLET_URL"

   log "submitting ${SYSTEM_ACCOUNT}::activate for $FEATURE_CODENAME"
   "$FUCLI_BIN" -u "$NODE_URL" --wallet-url "$WALLET_URL" --no-auto-wallet \
      push action "$SYSTEM_ACCOUNT" activate "[\"$FEATURE_DIGEST\"]" \
      -p "${SYSTEM_ACCOUNT}@${SYSTEM_PERMISSION}"
}

case "$MODE" in
   check|activate|verify)
      ;;
   -h|--help|help)
      usage
      exit 0
      ;;
   *)
      usage >&2
      die "unsupported mode: $MODE"
      ;;
esac

require_command curl
require_command jq
[[ -n "$NODE_URL" ]] || die "NODE_URL is required"

PRIMARY_INFO="$(get_info "$NODE_URL")"
CHAIN_ID="$(jq -er '.chain_id' <<<"$PRIMARY_INFO")"
SERVER_VERSION="$(jq -r '.server_version_string // .server_version // "unknown"' <<<"$PRIMARY_INFO")"
HEAD_BLOCK_NUM="$(jq -er '.head_block_num' <<<"$PRIMARY_INFO")"
LIB_BLOCK_NUM="$(jq -er '.last_irreversible_block_num' <<<"$PRIMARY_INFO")"

if [[ -n "$EXPECTED_CHAIN_ID" && "$CHAIN_ID" != "$EXPECTED_CHAIN_ID" ]]; then
   die "NODE_URL is on chain $CHAIN_ID, expected $EXPECTED_CHAIN_ID"
fi

log "chain_id=$CHAIN_ID server_version=$SERVER_VERSION head=$HEAD_BLOCK_NUM lib=$LIB_BLOCK_NUM"

if [[ "$MODE" == "verify" ]]; then
   # Verification does not require producer_api_plugin. Resolve the digest from
   # the activated feature record by codename instead.
   PRIMARY_ACTIVATED="$(get_activated_features "$NODE_URL")"
   ACTIVE_RECORD="$(jq -c --arg codename "$FEATURE_CODENAME" \
      '.activated_protocol_features[]? |
       select(any(.specification[]?; .name == "builtin_feature_codename" and .value == $codename))' \
      <<<"$PRIMARY_ACTIVATED" | head -n 1)"
   [[ -n "$ACTIVE_RECORD" ]] || die "$FEATURE_CODENAME is not activated"
   [[ "$(jq -er '.description_digest' <<<"$ACTIVE_RECORD")" == "$FEATURE_DESCRIPTION_DIGEST" ]] || \
      die "$FEATURE_CODENAME has an unexpected description digest"
   FEATURE_DIGEST="$(jq -er '.feature_digest' <<<"$ACTIVE_RECORD")"
   verify_all_nodes
   log "$FEATURE_CODENAME is active and irreversible on every verification endpoint"
   exit 0
fi

check_producer_support
PRIMARY_ACTIVATED="$(get_activated_features "$NODE_URL")"
check_preactivate_feature "$PRIMARY_ACTIVATED"
check_system_contract

CURRENT_STATE="$(activation_state "$NODE_URL")"
case "$CURRENT_STATE" in
   irreversible:*)
      CURRENT_STATE_DATA="${CURRENT_STATE#irreversible:}"
      log "$FEATURE_CODENAME is already irreversible (activation_block=${CURRENT_STATE_DATA%%:*} lib=${CURRENT_STATE_DATA##*:})"
      if [[ "$MODE" == "activate" ]]; then
         verify_all_nodes
      fi
      exit 0
      ;;
   reversible:*)
      CURRENT_STATE_DATA="${CURRENT_STATE#reversible:}"
      log "$FEATURE_CODENAME is already activated but still reversible (activation_block=${CURRENT_STATE_DATA%%:*} lib=${CURRENT_STATE_DATA##*:})"
      if [[ "$MODE" == "activate" ]]; then
         verify_all_nodes
      fi
      exit 0
      ;;
   missing)
      log "$FEATURE_CODENAME is supported but not activated"
      ;;
esac

if [[ "$MODE" == "check" ]]; then
   log "read-only checks passed; no transaction was submitted"
   exit 0
fi

[[ -n "$EXPECTED_CHAIN_ID" ]] || \
   die "EXPECTED_CHAIN_ID must be set for activate mode (current chain: $CHAIN_ID)"
[[ "$CONFIRM_ACTIVATION" == "$FEATURE_CODENAME" ]] || \
   die "set CONFIRM_ACTIVATION=$FEATURE_CODENAME to authorize irreversible activation"

submit_activation
verify_all_nodes
log "$FEATURE_CODENAME activation completed and is irreversible"
