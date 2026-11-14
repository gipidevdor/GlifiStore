# GlifiStore benchmark report

Generated at `2026-08-01T21:43:35+00:00` from 38 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| core | index_insert | k=16, v=64, w=1, t=1, uniform | 12.17 M | — | 82.19 | — | — | — | — | 40.83 MiB | 0.00 |
| core | index_replace | k=16, v=64, w=1, t=1, uniform | 6.73 M | — | 148.54 | — | — | — | — | 41.14 MiB | 0.00 |
| core | index_find_hit | k=16, v=64, w=1, t=1, uniform | 11.59 M | — | 86.26 | — | — | — | — | 41.14 MiB | 0.00 |
| core | index_find_miss | k=16, v=64, w=1, t=1, uniform | 13.52 M | — | 73.98 | — | — | — | — | 47.27 MiB | 0.00 |
| core | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.45 M | — | 60.81 | — | — | — | — | 64.56 MiB | 0.00 |
| core | index_erase | k=16, v=64, w=1, t=1, uniform | 11.41 M | — | 87.67 | — | — | — | — | 64.56 MiB | 0.00 |
| core | store_put | k=16, v=64, w=1, t=1, uniform | 377.84 k | — | 2,646.61 | — | — | — | — | 185.72 MiB | 0.00 |
| core | store_get_copy | k=16, v=64, w=1, t=1, uniform | 2.20 M | — | 455.19 | — | — | — | — | 167.42 MiB | 0.00 |
| core | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 627.71 k | — | 1,593.10 | — | — | — | — | 165.22 MiB | 0.00 |
| core | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 532.43 k | — | 1,878.18 | — | — | — | — | 158.31 MiB | 0.00 |
| durable-group | store_durable_group_put | k=16, v=64, w=1, t=32, single-worker | 4.03 k | — | 248,070.00 | 7.04 ms | 10.92 ms | 14.11 ms | 14.14 ms | 4.25 MiB | 0.00 |
| durable-periodic | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 80.55 k | — | 12,413.90 | — | — | — | — | 14.05 MiB | 0.00 |
| durable-sync | store_durable_put | k=16, v=64, w=1, t=1, uniform | 204.63 | — | 4,886,760.00 | — | — | — | — | 2.77 MiB | 0.00 |
| parallel-single-worker | store_parallel_put | k=16, v=64, w=2, t=2, single-worker | 656.24 k | — | 1,523.85 | — | — | — | — | 40.52 MiB | 0.00 |
| parallel-single-worker | store_parallel_get_copy | k=16, v=64, w=2, t=2, single-worker | 3.66 M | — | 273.07 | — | — | — | — | 50.33 MiB | 0.00 |
| parallel-single-worker | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, single-worker | 1.14 M | — | 879.77 | — | — | — | — | 48.55 MiB | 0.00 |
| parallel-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 139.67 k | — | 7,159.82 | — | — | — | — | 139.53 MiB | 0.00 |
| parallel-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 2.74 M | — | 364.58 | — | — | — | — | 145.08 MiB | 0.00 |
| parallel-uniform | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, uniform | 659.24 k | — | 1,516.90 | — | — | — | — | 149.05 MiB | 0.00 |
| parallel-worker-affine | store_parallel_put | k=16, v=64, w=2, t=2, worker-affine | 506.05 k | — | 1,976.10 | — | — | — | — | 150.86 MiB | 0.00 |
| parallel-worker-affine | store_parallel_get_copy | k=16, v=64, w=2, t=2, worker-affine | 5.07 M | — | 197.42 | — | — | — | — | 127.70 MiB | 0.00 |
| parallel-worker-affine | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, worker-affine | 824.21 k | — | 1,213.28 | — | — | — | — | 127.48 MiB | 0.00 |
| parallel-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 230.56 k | — | 4,337.22 | — | — | — | — | 144.52 MiB | 0.00 |
| parallel-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 2.86 M | — | 349.14 | — | — | — | — | 140.53 MiB | 0.00 |
| parallel-zipf | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, zipf | 406.56 k | — | 2,459.66 | — | — | — | — | 148.44 MiB | 0.00 |
| server-latency-w2-p32 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 137.94 k | — | 7,249.64 | 489.21 µs | 1.12 ms | 1.88 ms | 6.93 ms | 119.25 MiB | 22.07 M |
| server-tcp-w1-p1 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 47.63 k | — | 20,994.00 | — | — | — | — | 103.16 MiB | 7.62 M |
| server-tcp-w1-p128 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 224.26 k | — | 4,459.10 | — | — | — | — | 106.98 MiB | 35.88 M |
| server-tcp-w1-p32 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 183.86 k | — | 5,438.99 | — | — | — | — | 103.45 MiB | 29.42 M |
| server-tcp-w1-p8 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 164.22 k | — | 6,089.23 | — | — | — | — | 106.31 MiB | 26.28 M |
| server-tcp-w2-p1 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 75.47 k | — | 13,250.90 | — | — | — | — | 101.63 MiB | 12.07 M |
| server-tcp-w2-p128 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 244.06 k | — | 4,097.44 | — | — | — | — | 104.20 MiB | 39.05 M |
| server-tcp-w2-p32 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 216.04 k | — | 4,628.80 | — | — | — | — | 105.45 MiB | 34.57 M |
| server-tcp-w2-p8 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 141.99 k | — | 7,042.86 | — | — | — | — | 99.36 MiB | 22.72 M |
| server-tcp-w4-p1 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 98.61 k | — | 10,141.50 | — | — | — | — | 107.97 MiB | 15.78 M |
| server-tcp-w4-p128 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 206.38 k | — | 4,845.36 | — | — | — | — | 118.42 MiB | 33.02 M |
| server-tcp-w4-p32 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 281.60 k | — | 3,551.13 | — | — | — | — | 105.89 MiB | 45.06 M |
| server-tcp-w4-p8 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 206.63 k | — | 4,839.65 | — | — | — | — | 103.72 MiB | 33.06 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| core | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| parallel-single-worker | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| parallel-uniform | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| parallel-worker-affine | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| parallel-zipf | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-latency-w2-p32 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w1-p1 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w1-p128 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w1-p32 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w1-p8 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w2-p1 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w2-p128 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w2-p32 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w2-p8 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w4-p1 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w4-p128 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w4-p32 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| server-tcp-w4-p8 | 629bc68 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| server-latency-w2-p32 | 8.46 µs / 11.11 ms | 1 rec / 208 B | 4.74 µs / 3.22 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w1-p1 | 6.21 µs / 3.21 ms | 1 rec / 208 B | 1.91 µs / 4.48 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w1-p128 | 1.86 µs / 2.70 ms | 1 rec / 208 B | 1.64 µs / 4.74 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w1-p32 | 2.47 µs / 8.62 ms | 1 rec / 208 B | 1.61 µs / 7.38 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w1-p8 | 2.53 µs / 3.74 ms | 1 rec / 208 B | 1.50 µs / 5.00 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w2-p1 | 7.74 µs / 9.44 ms | 1 rec / 208 B | 2.62 µs / 13.61 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w2-p128 | 4.44 µs / 5.98 ms | 1 rec / 208 B | 2.85 µs / 2.28 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w2-p32 | 5.03 µs / 1.82 ms | 1 rec / 208 B | 3.02 µs / 2.00 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w2-p8 | 7.02 µs / 11.42 ms | 1 rec / 208 B | 3.92 µs / 4.57 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w4-p1 | 10.93 µs / 16.79 ms | 1 rec / 208 B | 6.02 µs / 1.91 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w4-p128 | 13.54 µs / 17.41 ms | 1 rec / 208 B | 4.71 µs / 6.48 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w4-p32 | 8.15 µs / 4.55 ms | 1 rec / 208 B | 4.20 µs / 1.41 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| server-tcp-w4-p8 | 10.88 µs / 10.07 ms | 1 rec / 208 B | 4.65 µs / 3.89 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
