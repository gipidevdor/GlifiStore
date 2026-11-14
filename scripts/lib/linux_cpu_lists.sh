#!/usr/bin/env bash
# CPU list helpers for Linux hard-pinned A/B scaffolding (Wave 6).
# Pure bash 3.2+; sourced by benchmark_paired_linux_ab.sh. No hardware claims.
#
# Accepts list forms used by taskset: "0-3,8,10-11"

# Expand a CPU list into sorted unique integers, one per line on stdout.
# Returns non-zero if the list is empty or malformed.
expand_cpu_list() {
  local spec="${1:-}"
  local part start end i
  local out=""
  if [[ -z "$spec" ]]; then
    return 1
  fi
  # shellcheck disable=SC2086
  for part in ${spec//,/ }; do
    part="${part//[[:space:]]/}"
    if [[ -z "$part" ]]; then
      continue
    fi
    if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      start="${BASH_REMATCH[1]}"
      end="${BASH_REMATCH[2]}"
      if (( end < start )); then
        echo "expand_cpu_list: inverted range $part" >&2
        return 1
      fi
      for ((i = start; i <= end; i++)); do
        out="${out}${i}"$'\n'
      done
    elif [[ "$part" =~ ^[0-9]+$ ]]; then
      out="${out}${part}"$'\n'
    else
      echo "expand_cpu_list: malformed token '$part'" >&2
      return 1
    fi
  done
  if [[ -z "$out" ]]; then
    return 1
  fi
  printf '%s' "$out" | sort -n | uniq
}

# Count distinct CPUs in a list. Echoes count; returns 1 if malformed/empty.
count_cpu_list() {
  local expanded
  expanded="$(expand_cpu_list "$1")" || return 1
  printf '%s\n' "$expanded" | grep -c .
}

# Return 0 if two CPU lists are disjoint; 1 if they overlap or either is invalid.
cpu_lists_disjoint() {
  local a_list="$1"
  local b_list="$2"
  local a_expanded b_expanded
  local cpu
  a_expanded="$(expand_cpu_list "$a_list")" || return 1
  b_expanded="$(expand_cpu_list "$b_list")" || return 1
  while IFS= read -r cpu; do
    [[ -z "$cpu" ]] && continue
    if printf '%s\n' "$a_expanded" | grep -qx "$cpu"; then
      echo "cpu overlap at core $cpu" >&2
      return 1
    fi
  done <<< "$b_expanded"
  return 0
}

# Return 0 if needle CPUs are a subset of haystack; 1 otherwise.
cpu_list_subset() {
  local needle="$1"
  local haystack="$2"
  local needle_expanded hay_expanded
  local cpu
  needle_expanded="$(expand_cpu_list "$needle")" || return 1
  hay_expanded="$(expand_cpu_list "$haystack")" || return 1
  while IFS= read -r cpu; do
    [[ -z "$cpu" ]] && continue
    if ! printf '%s\n' "$hay_expanded" | grep -qx "$cpu"; then
      echo "cpu $cpu not in allowed set" >&2
      return 1
    fi
  done <<< "$needle_expanded"
  return 0
}
