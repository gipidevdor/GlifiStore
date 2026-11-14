#pragma once

#include <cstddef>
#include <ostream>

namespace glifistore::bench {

inline void print_common_metadata(std::ostream& out, const std::size_t warmup, const std::size_t repeats) {
#ifdef GLIFISTORE_GIT_SHA
    out << "# git_sha=" << GLIFISTORE_GIT_SHA << '\n';
#else
    out << "# git_sha=unknown\n";
#endif
#if defined(__aarch64__)
    out << "# arch=arm64\n";
#elif defined(__x86_64__)
    out << "# arch=x86_64\n";
#else
    out << "# arch=unknown\n";
#endif
#if defined(__APPLE__)
    out << "# platform=macos\n";
#elif defined(__linux__)
    out << "# platform=linux\n";
#elif defined(__FreeBSD__)
    out << "# platform=freebsd\n";
#elif defined(__OpenBSD__)
    out << "# platform=openbsd\n";
#else
    out << "# platform=unknown\n";
#endif
    out << "# compiler=" << __VERSION__ << '\n';
    out << "# benchmark_warmup=" << warmup << '\n';
    out << "# benchmark_repeats=" << repeats << '\n';
    out << "# note=use plugged-in power; thermal throttling affects spread\n";
}

} // namespace glifistore::bench
