#!/usr/bin/env bash
set -euo pipefail

# lcov expects a gcov-compatible executable. Clang coverage data must be
# decoded by the matching LLVM frontend rather than by the host GCC gcov.
exec "${LLVM_COV:-llvm-cov}" gcov "$@"
