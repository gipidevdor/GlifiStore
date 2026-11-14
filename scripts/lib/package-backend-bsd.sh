#!/usr/bin/env bash
# FreeBSD / OpenBSD backend module for scripts/package-ci.sh (Wave D).
#
# Probes the host, the in-repo reference port, the upstream service-account
# marker and the sealed candidate, then hands those facts to
# engineering/tools/bsd_package_lifecycle.py, which owns every status decision.
# The native package/service lifecycle stays in
# scripts/test-{freebsd,openbsd}-package-lifecycle.sh and is only invoked when
# the preflight admits it; on any other host the native rows are BLOCKED.
#
# This module reads packaging/<backend>/PORTS_ACCOUNT_REGISTERED and never
# creates it: the upstream UID/GID allocation is not ours to declare.
#
# Sourced by package-ci.sh; uses its root, tools, profile, stage, version,
# release_context and candidate_dir variables.

bsd_backend_sha256() {
  python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
}

# Admit the sealed candidate exactly as the native producers do: seal digest
# first, then the seal itself, then a single expected source archive.
bsd_backend_admit_candidate() {
  local candidate="$1"
  [[ -n "${CANDIDATE_SEAL_SHA256:-}" ]] || {
    echo "CANDIDATE_SEAL_SHA256 is required to admit a sealed candidate"
    return 1
  }
  local seal="$candidate/candidate-seal.json"
  [[ -f "$seal" ]] || { echo "candidate seal is missing: $seal"; return 1; }
  local actual
  actual="$(bsd_backend_sha256 "$seal")"
  echo "candidate_seal_sha256=$actual"
  [[ "$actual" == "$CANDIDATE_SEAL_SHA256" ]] || {
    echo "candidate seal digest mismatch: expected $CANDIDATE_SEAL_SHA256"
    return 1
  }
  python3 "$tools/release_bundle.py" verify-seal --directory "$candidate" \
    --seal candidate-seal.json || return 1
  local archives=()
  while IFS= read -r line; do
    [[ -n "$line" ]] && archives+=("$line")
  done < <(find "$candidate" -maxdepth 1 -type f -name "GlifiStore-$version.tar.xz" -print)
  (( ${#archives[@]} == 1 )) || {
    echo "expected exactly one sealed source archive GlifiStore-$version.tar.xz"
    return 1
  }
  echo "sealed_source=${archives[0]} sha256=$(bsd_backend_sha256 "${archives[0]}")"
  return 0
}

bsd_backend_run() {
  local backend="$1"
  local directory="$2"
  local plan="$3"
  local evidence="$4"

  local expected_host lifecycle ports_makefile
  case "$backend" in
    freebsd)
      expected_host="FreeBSD"
      lifecycle="$root/scripts/test-freebsd-package-lifecycle.sh"
      ports_makefile="Mk/bsd.port.mk"
      ;;
    openbsd)
      expected_host="OpenBSD"
      lifecycle="$root/scripts/test-openbsd-package-lifecycle.sh"
      ports_makefile="infrastructure/mk/bsd.port.mk"
      ;;
    *)
      echo "error: $backend is not a BSD backend" >&2
      return 2
      ;;
  esac

  local host ports_root
  host="$(uname -s)"
  ports_root="${PORTSDIR:-/usr/ports}"

  local port_structure="PASS"
  if ! python3 "$tools/validate_bsd_packaging.py" --root "$root" \
    >"$directory/reference-port-structure.log" 2>&1; then
    port_structure="FAIL"
  fi

  local marker="$root/packaging/$backend/PORTS_ACCOUNT_REGISTERED"
  local ports_account="absent"
  if [[ -f "$marker" ]]; then
    ports_account="present"
    {
      echo "marker=packaging/$backend/PORTS_ACCOUNT_REGISTERED"
      echo "sha256=$(bsd_backend_sha256 "$marker")"
      echo "read-only: package-ci never creates or updates this marker"
    } >"$directory/ports-account-registration.log"
  else
    {
      echo "marker=packaging/$backend/PORTS_ACCOUNT_REGISTERED"
      echo "state=absent"
      echo "the upstream $expected_host service-account allocation does not exist yet"
      echo "package-ci never creates this marker"
    } >"$directory/ports-account-registration.log"
  fi

  {
    echo "backend=$backend"
    echo "in_repo_reference_port=packaging/$backend/"
    echo "upstream_ports_acceptance=not granted"
    echo "authority=docs/distribution/bsd-packaging.md"
  } >"$directory/upstream-ports-acceptance.log"

  local sealed_source="absent"
  if [[ -n "$candidate_dir" ]]; then
    sealed_source="unverified"
    if bsd_backend_admit_candidate "$candidate_dir" \
      >"$directory/sealed-source-admission.log" 2>&1; then
      sealed_source="verified"
    fi
  fi

  local ports_tree="absent"
  [[ -f "$ports_root/$ports_makefile" ]] && ports_tree="present"
  local privileged="no"
  [[ "$(id -u)" == "0" ]] && privileged="yes"
  local lifecycle_script="absent"
  [[ -f "$lifecycle" ]] && lifecycle_script="present"

  local decision reason
  if ! IFS=$'\t' read -r decision reason < <(
    python3 "$tools/bsd_package_lifecycle.py" preflight --backend "$backend" --stage "$stage" \
      --host "$host" --ports-account "$ports_account" --sealed-source "$sealed_source" \
      --ports-tree "$ports_tree" --ports-root "$ports_root" --privileged "$privileged" \
      --lifecycle-script "$lifecycle_script"
  ); then
    echo "error: the $backend preflight refused to answer" >&2
    return 1
  fi
  [[ -n "$decision" ]] || { echo "error: empty $backend preflight decision" >&2; return 1; }

  {
    echo "backend=$backend expected_host=$expected_host host=$host"
    echo "stage=$stage profile=$profile"
    echo "ports_account=$ports_account sealed_source=$sealed_source"
    echo "ports_tree=$ports_tree ports_root=$ports_root privileged=$privileged"
    echo "lifecycle_script=$lifecycle_script"
    echo "native_lifecycle=$decision"
    if [[ -n "$reason" ]]; then
      echo "blockers:"
      printf '%s\n' "$reason" | tr ';' '\n' | sed -e 's/^ *//' -e '/^$/d' -e 's/^/  - /'
    fi
  } >"$directory/native-prerequisites.log"

  local native="skipped"
  if [[ "$decision" == "RUN" ]]; then
    reason=""
    # Forward the release context so a sealed N-1 upgrade walk can select the
    # SemVer predecessor without rebuilding it from HEAD.
    export GLIFISTORE_RELEASE_CONTEXT="$release_context"
    # The native producer writes its own per-step logs into this directory; the
    # status of each framework row comes from those retained logs, not from here.
    if bash "$lifecycle" "$candidate_dir" "$directory" \
      >"$directory/$backend-native-lifecycle.log" 2>&1; then
      native="passed"
    else
      native="failed"
    fi
  fi

  python3 "$tools/bsd_package_lifecycle.py" plan --backend "$backend" --profile "$profile" \
    --stage "$stage" --directory "$directory" --release-context "$release_context" \
    --port-structure "$port_structure" --ports-account "$ports_account" \
    --sealed-source "$sealed_source" --native-lifecycle "$native" --native-reason "$reason" \
    --check-plan "$plan" --emit-arguments "$directory/emit-arguments.txt" --replace >/dev/null

  local emit_arguments=()
  while IFS= read -r line; do
    emit_arguments+=("$line")
  done <"$directory/emit-arguments.txt"

  python3 "$tools/validate_package_evidence.py" emit --backend "$backend" --profile "$profile" \
    --stage "$stage" --release-context "$release_context" --check-plan "$plan" \
    --output "$evidence" "${emit_arguments[@]}" >/dev/null
  python3 "$tools/validate_package_evidence.py" validate "$evidence" \
    --release-context "$release_context" --backend "$backend" --profile "$profile" \
    --artifact-root "$directory" >/dev/null

  local result
  result="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["result"])' "$evidence")"
  echo "PACKAGE-CI $backend $profile $stage $result $evidence"
  [[ "$result" == "FAIL" ]] && return 1
  return 0
}
