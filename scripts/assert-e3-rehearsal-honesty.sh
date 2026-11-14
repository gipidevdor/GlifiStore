#!/usr/bin/env bash
# Fail closed if an E3 harness or campaign-prep artifact claims certification.
# Hosted CI and disposable-image rows must remain e3_certified=no.
set -euo pipefail

kind="auto"
artifact_dir=""

usage() {
  cat <<'EOF'
Usage:
  scripts/assert-e3-rehearsal-honesty.sh --dir DIR [--kind auto|harness|campaign]

Validates that DIR is labeled rehearsal / campaign-prep:
  - e3_certified=no and e4_certified=no in provenance
  - summary marks E3/E4 certified as no
  - never contains e3_certified=yes / E3 certified: `yes`
  - SHA-256 manifest verifies
  - harness PASS rows contain pause, worker-stop, reset, fsck, and recovery confirmation
  - campaign artifacts keep promotion_candidate != yes (scripts never promote)

Exit 0 on honesty pass; non-zero on missing files or certification claims.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      [[ $# -ge 2 ]] || { echo "error: --dir requires a path" >&2; exit 2; }
      artifact_dir="$2"
      shift 2
      ;;
    --kind)
      [[ $# -ge 2 ]] || { echo "error: --kind requires a value" >&2; exit 2; }
      kind="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -n "$artifact_dir" ]] || { echo "error: --dir is required" >&2; exit 2; }
[[ -d "$artifact_dir" ]] || { echo "error: not a directory: $artifact_dir" >&2; exit 2; }
[[ ! -L "$artifact_dir" ]] || { echo "error: artifact directory must not be a symlink: $artifact_dir" >&2; exit 2; }
case "$kind" in
  auto|harness|campaign) ;;
  *)
    echo "error: --kind accepts auto|harness|campaign" >&2
    exit 2
    ;;
esac

if [[ "$kind" == "auto" ]]; then
  if [[ -f "$artifact_dir/campaign-provenance.txt" ]]; then
    kind="campaign"
  elif [[ -f "$artifact_dir/provenance.txt" ]]; then
    kind="harness"
  else
    echo "error: cannot detect kind; missing provenance in $artifact_dir" >&2
    exit 1
  fi
fi

fail=0
note() { printf '%s\n' "$*"; }
err() { printf 'error: %s\n' "$*" >&2; fail=1; }

require_file() {
  local path="$1"
  [[ -f "$path" ]] || err "missing required file: $path"
}

require_grep() {
  local path="$1"
  local pattern="$2"
  local label="$3"
  if [[ ! -f "$path" ]]; then
    return
  fi
  if ! grep -Eq "$pattern" "$path"; then
    err "$label (pattern /$pattern/ not found in $path)"
  fi
}

forbid_grep() {
  local path="$1"
  local pattern="$2"
  local label="$3"
  if [[ ! -f "$path" ]]; then
    return
  fi
  if grep -Eq "$pattern" "$path"; then
    err "$label (forbidden pattern /$pattern/ in $path)"
  fi
}

verify_manifest() {
  local directory="$1"
  local manifest_path="$directory/manifest.sha256"
  local actual_inventory digest line listed_inventory listed_path
  require_file "$manifest_path"
  if [[ ! -s "$manifest_path" ]]; then
    err "SHA-256 manifest is empty: $manifest_path"
    return
  fi
  if grep -q '^unavailable  ' "$manifest_path"; then
    err "SHA-256 manifest reports checksum tooling unavailable"
    return
  fi
  if [[ -n "$(find "$directory" -path "$directory/work" -prune -o -type l -print -quit)" ]]; then
    err "artifact tree contains a symbolic link"
    return
  fi
  while IFS= read -r line; do
    digest="${line%%  *}"
    listed_path="${line#*  }"
    if [[ ! "$digest" =~ ^[[:xdigit:]]{64}$ || "$listed_path" == "$line" ||
          -z "$listed_path" || "$listed_path" == /* || "$listed_path" == ".." ||
          "$listed_path" == ../* || "$listed_path" == */../* || "$listed_path" == */.. ||
          "$listed_path" == *\\* ]]; then
      err "unsafe or malformed SHA-256 manifest entry: $manifest_path"
      return
    fi
  done <"$manifest_path"
  actual_inventory="$(
    cd "$directory" &&
      find . -path './work' -prune -o -type f ! -path './manifest.sha256' -print |
      sed 's#^\./##' | LC_ALL=C sort
  )"
  listed_inventory="$(sed -n 's/^[[:xdigit:]]\{64\}  //p' "$manifest_path" | LC_ALL=C sort)"
  if [[ "$actual_inventory" != "$listed_inventory" ]]; then
    err "SHA-256 manifest inventory is incomplete or contains unknown paths: $manifest_path"
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    if ! (cd "$directory" && sha256sum -c manifest.sha256 >/dev/null 2>&1); then
      err "SHA-256 manifest verification failed: $manifest_path"
    fi
  elif command -v shasum >/dev/null 2>&1; then
    if ! (cd "$directory" && shasum -a 256 -c manifest.sha256 >/dev/null 2>&1); then
      err "SHA-256 manifest verification failed: $manifest_path"
    fi
  else
    err "no SHA-256 verifier available"
  fi
}

assert_common_honesty() {
  local prov="$1"
  local sum="$2"

  require_file "$prov"
  require_file "$sum"

  require_grep "$prov" '^e3_certified=no$' "provenance must set e3_certified=no"
  require_grep "$prov" '^e4_certified=no$' "provenance must set e4_certified=no"
  forbid_grep "$prov" '^e3_certified=yes$' "provenance must not set e3_certified=yes"
  forbid_grep "$prov" '^e4_certified=yes$' "provenance must not set e4_certified=yes"

  require_grep "$sum" 'E3 certified: `no`' "summary must mark E3 certified no"
  require_grep "$sum" 'E4 certified: `no`' "summary must mark E4 certified no"
  forbid_grep "$sum" 'E3 certified: `yes`' "summary must not claim E3 certified yes"
  forbid_grep "$sum" 'E4 certified: `yes`' "summary must not claim E4 certified yes"
}

if [[ "$kind" == "harness" ]]; then
  note "Asserting E3 harness honesty in $artifact_dir"
  assert_common_honesty "$artifact_dir/provenance.txt" "$artifact_dir/summary.md"
  require_file "$artifact_dir/results.tsv"
  verify_manifest "$artifact_dir"
  require_grep "$artifact_dir/provenance.txt" '^schema=glifistore-durability-e3-harness-v2$' \
    "harness provenance must use the pause/reset-confirmation schema"
  require_grep "$artifact_dir/provenance.txt" '^checkpoint_action=pause$' \
    "harness provenance must record checkpoint_action=pause"
  require_grep "$artifact_dir/summary.md" 'Release-certification eligible: `no`' \
    "harness summary must keep release-certification eligible=no"
  forbid_grep "$artifact_dir/summary.md" 'Release-certification eligible: `yes`' \
    "harness summary must not claim release-certification eligible=yes"
  if [[ -f "$artifact_dir/results.tsv" ]]; then
    expected_header=$'iteration\tscenario\tboundary\tcheckpoint_action\tworker_stop_confirmed\treset_mechanism\tdm_fault_mode\treset_confirmed\tfsck_status\trecovery\toutcome'
    actual_header="$(head -n 1 "$artifact_dir/results.tsv")"
    [[ "$actual_header" == "$expected_header" ]] || err "unexpected harness results.tsv schema"
    if ! awk -F '\t' 'NR == 1 { next }
      NF != 11 { exit 1 }
      $11 !~ /^(PASS|FAIL|INCONCLUSIVE)$/ { exit 1 }
      $11 == "PASS" && ($4 != "pause" || $5 != "yes" || $8 != "yes" ||
                        $9 !~ /^(clean-or-ok|reported-issues)$/ || $10 != "passed") { exit 1 }
      END { if (NR < 2) exit 1 }' "$artifact_dir/results.tsv"; then
      err "harness result rows are malformed or a PASS lacks stop/reset/recovery confirmation"
    fi
    expected_mode="$(sed -n 's/^dm_fault_mode=//p' "$artifact_dir/provenance.txt" | head -n 1)"
    if [[ -n "$expected_mode" ]] &&
       ! awk -F '\t' -v mode="$expected_mode" 'NR == 1 { next } $7 != mode { exit 1 }' \
         "$artifact_dir/results.tsv"; then
      err "results.tsv dm_fault_mode disagrees with provenance"
    fi
    read -r observed_passed observed_failed observed_inconclusive < <(
      awk -F '\t' 'NR > 1 {
        passed += ($11 == "PASS"); failed += ($11 == "FAIL"); inconclusive += ($11 == "INCONCLUSIVE")
      } END { print passed + 0, failed + 0, inconclusive + 0 }' "$artifact_dir/results.tsv"
    )
    declared_passed="$(sed -n 's/^passed=//p' "$artifact_dir/provenance.txt" | tail -n 1)"
    declared_failed="$(sed -n 's/^failed=//p' "$artifact_dir/provenance.txt" | tail -n 1)"
    declared_inconclusive="$(sed -n 's/^inconclusive=//p' "$artifact_dir/provenance.txt" | tail -n 1)"
    if [[ "$observed_passed" != "$declared_passed" || "$observed_failed" != "$declared_failed" ||
          "$observed_inconclusive" != "$declared_inconclusive" ]]; then
      err "results.tsv outcome totals disagree with provenance"
    fi
    expected_result="passed"
    if [[ "$observed_failed" -gt 0 ]]; then
      expected_result="failed"
    elif [[ "$observed_inconclusive" -gt 0 ]]; then
      expected_result="inconclusive"
    fi
    require_grep "$artifact_dir/provenance.txt" "^harness_result=${expected_result}$" \
      "harness_result must match per-case outcomes"
  fi
  if grep -Eq '^harness_result=passed$' "$artifact_dir/provenance.txt" 2>/dev/null; then
    require_grep "$artifact_dir/provenance.txt" '^reset_confirmed_any=yes$' \
      "a passing harness must contain at least one confirmed reset"
    if [[ -f "$artifact_dir/results.tsv" ]] &&
       ! awk -F '\t' 'NR == 1 { next } $11 != "PASS" { exit 1 }' "$artifact_dir/results.tsv"; then
      err "harness_result=passed requires every case row to be PASS"
    fi
  fi
elif [[ "$kind" == "campaign" ]]; then
  note "Asserting E3 campaign honesty in $artifact_dir"
  assert_common_honesty "$artifact_dir/campaign-provenance.txt" "$artifact_dir/campaign-summary.md"
  verify_manifest "$artifact_dir"
  require_grep "$artifact_dir/campaign-provenance.txt" '^schema=glifistore-durability-e3-campaign-v2$' \
    "campaign provenance must use the fault-mode-aware schema"
  require_file "$artifact_dir/campaign-pin.txt"
  require_file "$artifact_dir/promotion-checklist.md"
  require_grep "$artifact_dir/campaign-provenance.txt" '^promotion_candidate=' \
    "campaign provenance must record promotion_candidate"
  forbid_grep "$artifact_dir/campaign-provenance.txt" '^promotion_candidate=yes$' \
    "scripts must never emit promotion_candidate=yes"
  if [[ -f "$artifact_dir/stages/e3/provenance.txt" ]]; then
    require_grep "$artifact_dir/stages/e3/provenance.txt" '^e3_certified=no$' \
      "nested E3 harness provenance must set e3_certified=no"
    forbid_grep "$artifact_dir/stages/e3/provenance.txt" '^e3_certified=yes$' \
      "nested E3 harness provenance must not set e3_certified=yes"
  fi
fi

if [[ "$fail" -ne 0 ]]; then
  echo "E3 honesty assert FAILED for kind=$kind dir=$artifact_dir" >&2
  exit 1
fi

echo "E3 honesty assert OK (kind=$kind)"
exit 0
