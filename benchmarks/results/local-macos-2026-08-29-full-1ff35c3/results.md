# GlifiStore benchmark report

Generated at `2026-08-29T13:29:17+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

No retained baseline is available; throughput deltas are not shown.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 12.95 M | — | — | 77.25 | — | — | — | — | 40.77 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 9.40 M | — | — | 106.37 | — | — | — | — | 40.83 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 10.68 M | — | — | 93.67 | — | — | — | — | 40.83 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 13.73 M | — | — | 72.84 | — | — | — | — | 46.98 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.75 M | — | — | 59.70 | — | — | — | — | 63.27 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 11.68 M | — | — | 85.62 | — | — | — | — | 63.27 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.91 M | — | — | 524.23 | — | — | — | — | 480.88 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 3.61 M | — | — | 277.02 | — | — | — | — | 91.05 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 2.75 M | — | — | 364.21 | — | — | — | — | 183.03 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.63 k | — | — | 63,973.20 | — | — | — | — | 504.64 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 744.95 k | — | — | 1,342.38 | — | — | — | — | 1,649.03 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 3.26 M | — | — | 306.48 | — | — | — | — | 108.98 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 63.62 k | — | — | 15,717.50 | — | — | — | — | 506.25 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 566.28 k | — | — | 1,765.92 | — | — | — | — | 478.88 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 655.38 k | — | — | 1,525.83 | — | — | — | — | 94.80 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 633.37 k | — | — | 1,578.85 | — | — | — | — | 183.98 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 15.21 k | — | — | 65,747.00 | — | — | — | — | 504.31 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 369.37 k | — | — | 2,707.34 | — | — | — | — | 1,652.08 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 606.78 k | — | — | 1,648.05 | — | — | — | — | 110.88 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 53.72 k | — | — | 18,616.50 | — | — | — | — | 505.98 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 607.47 k | — | — | 1,646.17 | — | — | — | — | 476.42 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 702.88 k | — | — | 1,422.71 | — | — | — | — | 91.03 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 672.99 k | — | — | 1,485.91 | — | — | — | — | 185.98 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 14.71 k | — | — | 67,962.50 | — | — | — | — | 504.61 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 391.04 k | — | — | 2,557.30 | — | — | — | — | 1,652.10 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 713.79 k | — | — | 1,400.97 | — | — | — | — | 109.48 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 54.09 k | — | — | 18,487.40 | — | — | — | — | 505.98 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 377.89 k | — | — | 2,646.30 | — | — | — | — | 476.39 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 399.20 k | — | — | 2,504.99 | — | — | — | — | 91.03 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 394.80 k | — | — | 2,532.91 | — | — | — | — | 187.19 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 13.81 k | — | — | 72,392.10 | — | — | — | — | 504.23 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 264.75 k | — | — | 3,777.22 | — | — | — | — | 1,650.83 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 402.62 k | — | — | 2,483.73 | — | — | — | — | 112.14 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 47.45 k | — | — | 21,073.90 | — | — | — | — | 505.94 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 619.88 k | — | — | 1,613.21 | — | — | — | — | 478.45 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 771.11 k | — | — | 1,296.83 | — | — | — | — | 92.58 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 757.51 k | — | — | 1,320.11 | — | — | — | — | 184.05 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 14.75 k | — | — | 67,816.90 | — | — | — | — | 504.61 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 394.14 k | — | — | 2,537.18 | — | — | — | — | 1,652.11 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 771.77 k | — | — | 1,295.73 | — | — | — | — | 109.42 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 55.37 k | — | — | 18,059.80 | — | — | — | — | 506.06 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.16 k | — | — | 65,964.60 | 61.00 µs | 87.67 µs | 101.58 µs | 220.58 µs | 4.98 MiB | 2.43 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 8.59 k | — | — | 116,464.00 | 57.25 µs | 84.29 µs | 145.17 µs | 479.17 µs | 4.95 MiB | 1.37 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 391.40 | — | — | 2,554,930.00 | 5.00 ms | 6.19 ms | 7.61 ms | 12.00 ms | 5.00 MiB | 62.62 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 42.39 k | — | — | 23,590.60 | 389.04 µs | 852.33 µs | 1.13 ms | 1.39 ms | 6.25 MiB | 6.78 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 13.88 k | — | — | 72,034.70 | 457.08 µs | 6.23 ms | 8.21 ms | 8.50 ms | 5.11 MiB | 2.22 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 389.27 | — | — | 2,568,910.00 | 85.64 ms | 176.41 ms | 225.65 ms | 651.20 ms | 6.30 MiB | 62.28 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 45.91 k | — | — | 21,783.20 | 115.12 µs | 181.79 µs | 237.71 µs | 341.71 µs | 4.95 MiB | 7.35 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.44 k | — | — | 80,362.50 | 120.96 µs | 4.92 ms | 6.35 ms | 6.90 ms | 5.09 MiB | 1.99 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 410.66 | — | — | 2,435,090.00 | 21.24 ms | 40.87 ms | 43.96 ms | 48.98 ms | 5.06 MiB | 65.71 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 375.82 | — | — | 2,660,870.00 | 324.16 ms | 833.81 ms | 1173.20 ms | 1337.62 ms | 6.70 MiB | 60.13 k |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.49 k | — | — | 672,980.00 | 85.09 ms | 169.36 ms | 206.92 ms | 257.35 ms | 5.80 MiB | 237.75 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.41 k | — | — | 706,988.00 | 100.06 ms | 262.81 ms | 494.97 ms | 544.37 ms | 5.73 MiB | 226.31 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.48 k | — | — | 674,249.00 | 87.88 ms | 175.03 ms | 204.81 ms | 229.12 ms | 5.69 MiB | 237.30 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.61 k | — | — | 620,659.00 | 84.01 ms | 186.58 ms | 260.48 ms | 622.48 ms | 6.69 MiB | 257.79 k |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 75.08 k | — | — | 13,319.30 | 478.04 µs | 1.50 ms | 2.23 ms | 2.58 ms | 8.48 MiB | 12.01 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 300.41 | — | — | 3,328,780.00 | 184.91 ms | 431.75 ms | 1103.42 ms | 1241.13 ms | 8.61 MiB | 48.07 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 95.69 k | — | — | 10,450.60 | 669.79 µs | 1.31 ms | 1.57 ms | 1.93 ms | 12.97 MiB | 15.31 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 349.28 | — | — | 2,863,040.00 | 292.81 ms | 825.44 ms | 1304.35 ms | 1436.10 ms | 9.22 MiB | 55.88 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.44 k | — | — | 54,223.10 | 51.67 µs | 78.38 µs | 126.50 µs | 256.67 µs | 4.97 MiB | 2.95 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 16.91 k | — | — | 59,131.80 | 52.46 µs | 75.42 µs | 132.88 µs | 186.92 µs | 4.95 MiB | 2.71 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.75 k | — | — | 93,012.60 | 72.12 µs | 329.08 µs | 4.81 ms | 8.02 ms | 4.95 MiB | 1.72 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 49.65 k | — | — | 20,142.90 | 342.50 µs | 679.62 µs | 944.21 µs | 1.18 ms | 6.28 MiB | 7.94 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 31.41 k | — | — | 31,832.80 | 384.08 µs | 3.72 ms | 9.32 ms | 9.62 ms | 5.02 MiB | 5.03 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 12.92 k | — | — | 77,414.20 | 833.71 µs | 13.15 ms | 20.32 ms | 36.30 ms | 6.25 MiB | 2.07 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 39.37 k | — | — | 25,398.60 | 126.88 µs | 232.71 µs | 372.21 µs | 565.00 µs | 5.00 MiB | 6.30 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 29.16 k | — | — | 34,289.00 | 124.96 µs | 246.33 µs | 2.72 ms | 4.86 ms | 5.20 MiB | 4.67 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 15.22 k | — | — | 65,705.50 | 228.25 µs | 1.79 ms | 7.38 ms | 13.77 ms | 6.02 MiB | 2.44 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 82.35 k | — | — | 12,143.70 | 441.67 µs | 879.71 µs | 1.12 ms | 1.39 ms | 6.53 MiB | 13.18 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 10.98 k | — | — | 91,063.40 | 5.25 ms | 15.40 ms | 23.66 ms | 29.34 ms | 6.66 MiB | 1.76 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 86.07 k | — | — | 11,619.00 | 774.96 µs | 1.49 ms | 1.81 ms | 2.00 ms | 9.11 MiB | 13.77 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 31.94 k | — | — | 31,307.00 | 1.51 ms | 13.03 ms | 18.39 ms | 24.21 ms | 9.06 MiB | 5.11 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 13.65 k | — | — | 73,253.30 | 66.04 µs | 99.00 µs | 152.04 µs | 274.21 µs | 4.95 MiB | 2.18 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.17 k | — | — | 98,325.40 | 52.46 µs | 93.92 µs | 140.83 µs | 201.83 µs | 5.02 MiB | 1.63 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 435.75 | — | — | 2,294,920.00 | 4.81 ms | 5.82 ms | 6.46 ms | 10.73 ms | 4.95 MiB | 69.72 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 44.31 k | — | — | 22,569.40 | 372.67 µs | 818.08 µs | 1.03 ms | 1.34 ms | 5.28 MiB | 7.09 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 15.30 k | — | — | 65,366.40 | 482.92 µs | 5.35 ms | 5.74 ms | 5.86 ms | 5.00 MiB | 2.45 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 419.13 | — | — | 2,385,910.00 | 82.97 ms | 173.19 ms | 233.59 ms | 318.10 ms | 6.34 MiB | 67.06 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 38.13 k | — | — | 26,224.90 | 122.08 µs | 242.58 µs | 308.29 µs | 365.33 µs | 4.97 MiB | 6.10 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 13.49 k | — | — | 74,117.10 | 137.33 µs | 3.70 ms | 5.00 ms | 5.79 ms | 4.94 MiB | 2.16 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 420.69 | — | — | 2,377,070.00 | 20.94 ms | 39.35 ms | 45.05 ms | 51.36 ms | 4.94 MiB | 67.31 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 69.34 k | — | — | 14,421.40 | 462.71 µs | 949.17 µs | 1.19 ms | 1.37 ms | 6.47 MiB | 11.09 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 409.74 | — | — | 2,440,580.00 | 166.58 ms | 394.47 ms | 781.14 ms | 882.03 ms | 8.50 MiB | 65.56 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 106.30 k | — | — | 9,407.25 | 632.29 µs | 1.50 ms | 2.03 ms | 2.36 ms | 9.08 MiB | 17.01 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 599.70 | — | — | 1,667,510.00 | 218.99 ms | 432.76 ms | 496.02 ms | 547.10 ms | 13.00 MiB | 95.95 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 164.31 | — | — | 6,085,910.00 | — | — | — | — | 4.39 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 679.31 k | — | — | 1,472.08 | — | — | — | — | 4.84 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 390.18 | — | — | 2,562,940.00 | — | — | — | — | 4.94 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 393.30 | — | — | 2,542,600.00 | — | — | — | — | 4.98 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 388.53 | — | — | 2,573,790.00 | 9.98 ms | 16.14 ms | 23.94 ms | 34.93 ms | 4.80 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.21 k | — | — | 311,873.00 | — | — | — | — | 3.16 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 921.87 k | — | — | 1,084.75 | — | — | — | — | 3.42 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 6.36 k | — | — | 157,128.00 | — | — | — | — | 3.56 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 6.62 k | — | — | 150,982.00 | — | — | — | — | 3.67 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 181.39 | — | — | 5,513,100.00 | — | — | — | — | 3.14 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 619.36 k | — | — | 1,614.58 | — | — | — | — | 3.37 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 381.97 | — | — | 2,617,990.00 | — | — | — | — | 3.47 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 344.98 | — | — | 2,898,680.00 | — | — | — | — | 3.53 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 317.70 k | — | — | 3,147.58 | — | — | — | — | 3.75 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 269.73 | — | — | 3,707,420.00 | 13.88 ms | 20.86 ms | 29.83 ms | 120.08 ms | 3.61 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 4.17 M | — | — | 240.03 | — | — | — | — | 111.58 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 4.01 M | — | — | 249.21 | — | — | — | — | 111.02 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 4.15 M | — | — | 241.22 | — | — | — | — | 112.23 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 9.30 M | — | — | 107.50 | — | — | — | — | 112.80 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 5.90 M | — | — | 169.36 | — | — | — | — | 112.80 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.31 M | — | — | 158.36 | — | — | — | — | 112.52 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 16.31 M | — | — | 61.32 | — | — | — | — | 115.98 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 9.96 M | — | — | 100.42 | — | — | — | — | 116.03 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 7.50 M | — | — | 133.35 | — | — | — | — | 100.44 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 23.17 M | — | — | 43.17 | — | — | — | — | 122.48 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 11.72 M | — | — | 85.32 | — | — | — | — | 118.44 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.44 M | — | — | 95.78 | — | — | — | — | 104.75 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 490.04 k | — | — | 2,040.65 | — | — | — | — | 57.37 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 484.05 k | — | — | 2,065.91 | — | — | — | — | 57.34 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 476.78 k | — | — | 2,097.42 | — | — | — | — | 57.34 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 680.53 k | — | — | 1,469.43 | — | — | — | — | 60.98 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 473.61 k | — | — | 2,111.46 | — | — | — | — | 61.06 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 405.52 k | — | — | 2,465.98 | — | — | — | — | 59.91 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.22 M | — | — | 819.65 | — | — | — | — | 71.42 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 478.36 k | — | — | 2,090.47 | — | — | — | — | 60.12 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 391.73 k | — | — | 2,552.81 | — | — | — | — | 53.84 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 1.93 M | — | — | 518.65 | — | — | — | — | 59.58 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 535.71 k | — | — | 1,866.67 | — | — | — | — | 67.37 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 519.52 k | — | — | 1,924.87 | — | — | — | — | 69.84 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 905.98 k | — | — | 1,103.77 | — | — | — | — | 57.37 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.31 M | — | — | 763.07 | — | — | — | — | 65.27 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 2.16 M | — | — | 463.95 | — | — | — | — | 66.55 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.56 M | — | — | 281.01 | — | — | — | — | 60.98 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 91.70 k | — | — | 10,904.90 | 641.92 µs | 1.06 ms | 1.28 ms | 2.74 ms | 34.14 MiB | 14.67 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 45.57 k | — | — | 21,946.10 | 21.38 µs | 26.17 µs | 42.75 µs | 87.92 µs | 31.97 MiB | 7.29 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.80 k | — | — | 22,319.20 | 21.46 µs | 26.54 µs | 44.88 µs | 85.79 µs | 32.02 MiB | 7.17 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 51.23 k | — | — | 19,519.50 | 36.38 µs | 60.71 µs | 108.17 µs | 200.21 µs | 37.31 MiB | 8.20 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.73 M | — | — | 578.86 | 66.62 µs | 83.00 µs | 122.58 µs | 191.42 µs | 78.55 MiB | 276.40 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.31 M | — | — | 760.51 | 81.71 µs | 106.58 µs | 151.92 µs | 201.71 µs | 85.12 MiB | 210.39 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 130.61 k | — | — | 7,656.22 | 895.46 µs | 2.46 ms | 3.63 ms | 7.24 ms | 108.70 MiB | 20.90 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 423.51 k | — | — | 2,361.21 | 62.17 µs | 77.58 µs | 93.17 µs | 166.83 µs | 147.22 MiB | 474.33 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 849.93 k | — | — | 1,176.56 | 35.17 µs | 46.29 µs | 77.08 µs | 125.38 µs | 79.00 MiB | 135.99 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 16.30 k | — | — | 61,362.70 | 1.02 ms | 1.87 ms | 2.09 ms | 2.48 ms | 402.08 MiB | 1.07 G |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 721.91 k | — | — | 1,385.21 | 36.71 µs | 60.62 µs | 93.58 µs | 134.83 µs | 81.03 MiB | 115.51 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 163.50 k | — | — | 6,116.38 | 199.46 µs | 405.21 µs | 690.75 µs | 3.14 ms | 108.63 MiB | 26.16 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 4.21 k | — | — | 237,261.00 | 982.38 µs | 1.70 ms | 1.85 ms | 2.07 ms | 428.44 MiB | 1.11 G |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 281.04 k | — | — | 3,558.17 | 25.88 µs | 41.00 µs | 65.96 µs | 112.92 µs | 79.28 MiB | 44.97 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 265.34 k | — | — | 3,768.77 | 26.54 µs | 43.75 µs | 65.04 µs | 119.04 µs | 79.19 MiB | 42.45 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 133.93 k | — | — | 7,466.86 | 75.88 µs | 136.67 µs | 229.00 µs | 858.00 µs | 107.58 MiB | 21.43 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 79.70 k | — | — | 12,546.50 | 23.96 µs | 38.33 µs | 58.62 µs | 82.75 µs | 30.77 MiB | 12.75 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 78.84 k | — | — | 12,683.40 | 23.75 µs | 33.25 µs | 55.58 µs | 81.83 µs | 29.98 MiB | 12.61 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 82.22 k | — | — | 12,162.20 | 43.96 µs | 81.42 µs | 104.38 µs | 136.46 µs | 39.05 MiB | 13.16 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.38 M | — | — | 295.73 | 69.17 µs | 110.92 µs | 139.42 µs | 188.96 µs | 75.34 MiB | 541.03 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 1.85 M | — | — | 540.81 | 81.71 µs | 151.25 µs | 191.71 µs | 267.67 µs | 78.09 MiB | 295.85 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 171.81 k | — | — | 5,820.45 | 1.44 ms | 3.02 ms | 3.77 ms | 5.29 ms | 112.56 MiB | 27.49 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.60 M | — | — | 625.93 | 37.38 µs | 46.67 µs | 78.62 µs | 112.38 µs | 74.94 MiB | 255.62 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.21 M | — | — | 823.91 | 39.00 µs | 70.38 µs | 97.75 µs | 140.92 µs | 77.28 MiB | 194.19 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 188.16 k | — | — | 5,314.50 | 348.67 µs | 734.17 µs | 923.54 µs | 2.70 ms | 105.52 MiB | 30.11 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 572.99 k | — | — | 1,745.23 | 26.12 µs | 37.38 µs | 64.21 µs | 92.88 µs | 76.70 MiB | 91.68 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 516.90 k | — | — | 1,934.61 | 26.58 µs | 43.25 µs | 64.96 µs | 96.88 µs | 78.52 MiB | 82.70 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 165.50 k | — | — | 6,042.25 | 113.46 µs | 232.50 µs | 294.92 µs | 816.62 µs | 107.50 MiB | 26.48 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 112.81 k | — | — | 8,864.35 | 32.75 µs | 54.50 µs | 65.79 µs | 83.04 µs | 33.03 MiB | 18.05 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 108.61 k | — | — | 9,207.45 | 33.38 µs | 56.79 µs | 75.17 µs | 190.25 µs | 32.95 MiB | 17.38 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 111.07 k | — | — | 9,003.45 | 69.58 µs | 97.71 µs | 113.75 µs | 144.25 µs | 41.73 MiB | 17.77 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.62 M | — | — | 177.94 | 79.71 µs | 125.88 µs | 152.83 µs | 188.17 µs | 75.53 MiB | 899.15 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 2.74 M | — | — | 364.79 | 92.04 µs | 170.42 µs | 239.88 µs | 303.92 µs | 79.75 MiB | 438.61 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 259.18 k | — | — | 3,858.36 | 1.98 ms | 3.90 ms | 4.72 ms | 6.25 ms | 115.86 MiB | 41.47 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.55 M | — | — | 392.01 | 43.75 µs | 75.62 µs | 91.79 µs | 113.50 µs | 76.94 MiB | 408.15 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.69 M | — | — | 591.36 | 48.50 µs | 85.54 µs | 107.12 µs | 134.00 µs | 77.77 MiB | 270.56 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 279.10 k | — | — | 3,583.00 | 492.79 µs | 926.46 µs | 1.09 ms | 2.55 ms | 116.70 MiB | 44.66 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 810.58 k | — | — | 1,233.69 | 35.71 µs | 60.62 µs | 73.58 µs | 91.42 µs | 75.27 MiB | 129.69 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 688.47 k | — | — | 1,452.50 | 37.79 µs | 64.83 µs | 81.21 µs | 108.21 µs | 76.19 MiB | 110.16 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 236.42 k | — | — | 4,229.84 | 168.67 µs | 296.67 µs | 347.62 µs | 874.21 µs | 118.28 MiB | 37.83 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 129.33 k | — | — | 7,732.08 | 59.96 µs | 78.54 µs | 89.88 µs | 116.00 µs | 39.92 MiB | 20.69 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 130.93 k | — | — | 7,637.65 | 59.54 µs | 78.25 µs | 89.67 µs | 119.29 µs | 40.08 MiB | 20.95 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 177.19 k | — | — | 5,643.79 | 87.96 µs | 116.50 µs | 144.12 µs | 237.42 µs | 50.14 MiB | 28.35 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 9.28 M | — | — | 107.71 | 100.71 µs | 131.96 µs | 155.79 µs | 204.71 µs | 81.09 MiB | 1.49 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.39 M | — | — | 227.55 | 106.62 µs | 209.96 µs | 276.92 µs | 500.08 µs | 82.12 MiB | 703.14 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 342.95 k | — | — | 2,915.86 | 3.00 ms | 5.96 ms | 6.78 ms | 8.32 ms | 113.78 MiB | 54.87 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.63 M | — | — | 275.11 | 67.17 µs | 88.33 µs | 101.79 µs | 137.67 µs | 80.62 MiB | 581.58 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.61 M | — | — | 383.44 | 70.58 µs | 105.54 µs | 129.92 µs | 162.00 µs | 82.45 MiB | 417.28 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 322.00 k | — | — | 3,105.56 | 812.88 µs | 1.59 ms | 1.86 ms | 2.72 ms | 114.06 MiB | 51.52 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 1.00 M | — | — | 998.08 | 61.96 µs | 81.04 µs | 93.12 µs | 125.29 µs | 81.19 MiB | 160.31 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 905.97 k | — | — | 1,103.79 | 63.50 µs | 87.83 µs | 107.71 µs | 137.79 µs | 81.08 MiB | 144.96 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 275.31 k | — | — | 3,632.32 | 276.21 µs | 488.33 µs | 599.25 µs | 886.42 µs | 118.36 MiB | 44.05 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| copy-heavy | 1ff35c3 | unknown | unknown | unknown |
| high-reclaim | 1ff35c3 | unknown | unknown | unknown |
| low-reclaim | 1ff35c3 | unknown | unknown | unknown |
| medium-reclaim | 1ff35c3 | unknown | unknown | unknown |
| no-gain | 1ff35c3 | unknown | unknown | unknown |
| ttl-50 | 1ff35c3 | unknown | unknown | unknown |
| index-all-k16-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| generation-publication-adopt | unknown | unknown | unknown | unknown |
| generation-publication-get | unknown | unknown | unknown | unknown |
| generation-shell | unknown | unknown | unknown | unknown |
| churn-background | 1ff35c3 | unknown | unknown | unknown |
| churn-cooperative | 1ff35c3 | unknown | unknown | unknown |
| churn-disabled | 1ff35c3 | unknown | unknown | unknown |
| forced-rotation-background | 1ff35c3 | unknown | unknown | unknown |
| forced-rotation-cooperative | 1ff35c3 | unknown | unknown | unknown |
| forced-rotation-disabled | 1ff35c3 | unknown | unknown | unknown |
| idle-background | 1ff35c3 | unknown | unknown | unknown |
| idle-cooperative | 1ff35c3 | unknown | unknown | unknown |
| idle-disabled | 1ff35c3 | unknown | unknown | unknown |
| mixed-background | 1ff35c3 | unknown | unknown | unknown |
| mixed-cooperative | 1ff35c3 | unknown | unknown | unknown |
| mixed-disabled | 1ff35c3 | unknown | unknown | unknown |
| paired-reactor-c1-p1-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p1-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p1-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p1-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p128-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p128-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p128-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p128-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p32-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p32-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p32-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p32-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p8-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p8-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p8-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c1-p8-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p1-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p1-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p1-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p1-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p128-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p128-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p128-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p128-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p32-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p32-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p32-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p32-put5-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p8-put0-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p8-put1-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p8-put10-v64 | unknown | unknown | unknown | unknown |
| paired-reactor-c4-p8-put5-v64 | unknown | unknown | unknown | unknown |
| paired-shard-v1024 | unknown | unknown | unknown | unknown |
| paired-shard-v262144 | unknown | unknown | unknown | unknown |
| paired-shard-v64 | unknown | unknown | unknown | unknown |
| paired-shard-v65536 | unknown | unknown | unknown | unknown |
| get-w1-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | 1ff35c3 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-read-99-write-1 | 266.92 µs / 7.16 ms | 32 rec / 6656 B | 5.11 ms / 11.38 ms | 5.06 ms / 11.31 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 296.12 µs / 7.86 ms | 1 rec / 208 B | 4.63 ms / 12.28 ms | 4.57 ms / 12.19 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 270.12 µs / 862.38 µs | 31 rec / 6656 B | 5.06 ms / 7.92 ms | 5.04 ms / 7.58 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 283.29 µs / 2.98 ms | 1 rec / 208 B | 4.76 ms / 278.98 ms | 4.68 ms / 278.85 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 262.43 µs / 338.33 µs | 32 rec / 6656 B | 5.55 ms / 7.72 ms | 5.53 ms / 7.60 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 287.00 µs / 4.09 ms | 1 rec / 208 B | 4.49 ms / 12.62 ms | 4.44 ms / 12.53 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 15.84 ms / 425.22 ms | 3 rec / 624 B | 5.31 ms / 389.20 ms | 5.26 ms / 389.10 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 274.50 µs / 5.29 ms | 4 rec / 832 B | 5.02 ms / 22.63 ms | 4.95 ms / 22.57 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 274.73 µs / 1.13 ms | 4 rec / 832 B | 5.27 ms / 290.11 ms | 5.19 ms / 290.00 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 273.77 µs / 4.74 ms | 4 rec / 832 B | 5.03 ms / 22.52 ms | 4.95 ms / 22.46 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 13.30 µs / 256.00 µs | 4 rec / 832 B | 4.86 ms / 310.46 ms | 4.78 ms / 310.26 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 302.21 µs / 4.44 ms | 1 rec / 208 B | 12.87 ms / 642.11 ms | 12.78 ms / 641.99 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 309.36 µs / 5.21 ms | 1 rec / 208 B | 22.33 ms / 520.57 ms | 22.24 ms / 520.52 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 5.24 µs / 8.84 ms | 30 rec / 6656 B | 774.99 µs / 8.83 ms | 637.21 µs / 1.80 ms | 15.00 / 32.00 | 15 rec / 2040 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 5.63 µs / 123.21 µs | 1 rec / 208 B | 113.34 µs / 8.06 ms | 825.64 µs / 8.04 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 5.75 µs / 17.75 ms | 29 rec / 6656 B | 1.07 ms / 17.74 ms | — / 2.72 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 5.56 µs / 755.58 µs | 1 rec / 208 B | 126.52 µs / 30.99 ms | 504.03 µs / 1.07 ms | 32.00 / 32.00 | 20 rec / 2720 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 5.33 µs / 18.80 ms | 32 rec / 6656 B | 875.64 µs / 18.79 ms | — / 3.17 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 5.43 µs / 388.96 µs | 1 rec / 208 B | 89.38 µs / 13.44 ms | 726.68 µs / 1.55 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 7.34 µs / 124.71 µs | 1 rec / 208 B | 311.86 µs / 21.03 ms | 5.14 ms / 19.97 ms | 31.71 / 32.00 | 56 rec / 7616 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 13.24 µs / 982.29 µs | 1 rec / 208 B | 131.09 µs / 20.11 ms | 3.01 ms / 18.70 ms | 32.00 / 32.00 | 117 rec / 15912 B | 12/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 9.62 µs / 138.15 ms | 31 rec / 6656 B | 4.41 ms / 165.52 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 15.60 µs / 1.89 ms | 1 rec / 208 B | 4.43 ms / 11.21 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 13.33 µs / 168.92 ms | 32 rec / 6656 B | 4.19 ms / 168.92 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 10.53 µs / 5.89 ms | 1 rec / 208 B | 4.69 ms / 416.81 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 10.49 µs / 155.69 ms | 32 rec / 6656 B | 3.98 ms / 155.67 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 16.15 µs / 3.99 ms | 1 rec / 208 B | 4.61 ms / 11.60 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 19.15 µs / 9.88 ms | 1 rec / 208 B | 9.64 ms / 448.50 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 37.46 µs / 1.74 ms | 1 rec / 208 B | 13.13 ms / 35.81 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
