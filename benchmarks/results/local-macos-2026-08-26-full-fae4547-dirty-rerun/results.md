# GlifiStore benchmark report

Generated at `2026-08-26T20:37:02+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

Baseline report: `2026-08-26T16:20:14+00:00`.

Environment identity: **incompatible** (`identity-fields-missing`); throughput deltas are suppressed.

Missing current identity fields: `none`.
Missing baseline identity fields: `runner_image, runner_image_version, benchmark_contract_sha256`.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 13.15 M | — | — | 76.06 | — | — | — | — | 40.77 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 12.92 M | — | — | 77.40 | — | — | — | — | 42.34 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 13.95 M | — | — | 71.69 | — | — | — | — | 42.34 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 14.37 M | — | — | 69.60 | — | — | — | — | 48.47 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 17.19 M | — | — | 58.16 | — | — | — | — | 64.75 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 12.70 M | — | — | 78.77 | — | — | — | — | 64.77 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.83 M | — | — | 547.41 | — | — | — | — | 605.16 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 4.57 M | — | — | 218.82 | — | — | — | — | 211.30 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 3.00 M | — | — | 333.46 | — | — | — | — | 311.58 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.74 k | — | — | 59,753.20 | — | — | — | — | 509.61 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 795.01 k | — | — | 1,257.84 | — | — | — | — | 1,768.75 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 3.55 M | — | — | 281.31 | — | — | — | — | 229.66 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 68.35 k | — | — | 14,630.70 | — | — | — | — | 511.33 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 573.13 k | — | — | 1,744.80 | — | — | — | — | 603.23 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 679.58 k | — | — | 1,471.51 | — | — | — | — | 213.53 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 661.53 k | — | — | 1,511.65 | — | — | — | — | 308.83 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 14.81 k | — | — | 67,505.90 | — | — | — | — | 509.19 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 393.75 k | — | — | 2,539.66 | — | — | — | — | 1,782.00 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 697.60 k | — | — | 1,433.48 | — | — | — | — | 234.05 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 61.87 k | — | — | 16,162.40 | — | — | — | — | 511.52 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 691.78 k | — | — | 1,445.55 | — | — | — | — | 602.00 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 829.38 k | — | — | 1,205.71 | — | — | — | — | 215.58 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 815.70 k | — | — | 1,225.94 | — | — | — | — | 303.02 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.02 k | — | — | 66,597.90 | — | — | — | — | 509.33 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 437.60 k | — | — | 2,285.19 | — | — | — | — | 1,772.91 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 899.54 k | — | — | 1,111.67 | — | — | — | — | 233.94 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 60.45 k | — | — | 16,541.30 | — | — | — | — | 511.42 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 429.84 k | — | — | 2,326.46 | — | — | — | — | 605.19 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 487.81 k | — | — | 2,050.00 | — | — | — | — | 211.30 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 489.57 k | — | — | 2,042.60 | — | — | — | — | 307.28 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 14.67 k | — | — | 68,144.80 | — | — | — | — | 509.12 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 297.95 k | — | — | 3,356.28 | — | — | — | — | 1,780.09 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 478.14 k | — | — | 2,091.43 | — | — | — | — | 232.69 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 58.18 k | — | — | 17,186.60 | — | — | — | — | 511.86 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 709.36 k | — | — | 1,409.73 | — | — | — | — | 602.30 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 911.71 k | — | — | 1,096.84 | — | — | — | — | 215.56 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 889.17 k | — | — | 1,124.65 | — | — | — | — | 307.31 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 15.85 k | — | — | 63,101.60 | — | — | — | — | 509.75 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 434.60 k | — | — | 2,300.99 | — | — | — | — | 1,776.85 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 914.06 k | — | — | 1,094.02 | — | — | — | — | 233.97 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 61.05 k | — | — | 16,381.10 | — | — | — | — | 511.69 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.76 k | — | — | 53,314.30 | 50.38 µs | 71.12 µs | 77.79 µs | 94.46 µs | 5.02 MiB | 3.00 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 9.08 k | — | — | 110,143.00 | 59.67 µs | 68.25 µs | 77.08 µs | 104.79 µs | 5.00 MiB | 1.45 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 396.91 | — | — | 2,519,470.00 | 4.99 ms | 6.04 ms | 7.00 ms | 12.93 ms | 5.98 MiB | 63.51 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 45.42 k | — | — | 22,017.80 | 389.08 µs | 1.28 ms | 8.77 ms | 9.37 ms | 6.30 MiB | 7.27 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 14.47 k | — | — | 69,109.30 | 411.21 µs | 5.68 ms | 5.96 ms | 6.16 ms | 5.05 MiB | 2.32 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 394.74 | — | — | 2,533,350.00 | 82.09 ms | 157.97 ms | 170.99 ms | 189.85 ms | 5.39 MiB | 63.16 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 50.07 k | — | — | 19,972.20 | 111.25 µs | 174.25 µs | 212.58 µs | 252.71 µs | 5.00 MiB | 8.01 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 14.07 k | — | — | 71,096.50 | 123.42 µs | 4.80 ms | 5.10 ms | 5.39 ms | 6.03 MiB | 2.25 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 414.14 | — | — | 2,414,620.00 | 22.07 ms | 41.00 ms | 47.07 ms | 48.11 ms | 6.03 MiB | 66.26 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 6.32 k | — | — | 158,297.00 | 23.09 ms | 94.28 ms | 154.40 ms | 166.57 ms | 6.56 MiB | 1.01 M |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.11 k | — | — | 897,489.00 | 100.74 ms | 526.42 ms | 829.16 ms | 875.65 ms | 6.66 MiB | 178.28 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.48 k | — | — | 677,084.00 | 89.05 ms | 180.73 ms | 278.21 ms | 390.02 ms | 6.58 MiB | 236.31 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.53 k | — | — | 653,777.00 | 85.98 ms | 182.77 ms | 293.27 ms | 750.72 ms | 6.59 MiB | 244.73 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 12.38 k | — | — | 80,797.30 | 9.91 ms | 22.26 ms | 27.96 ms | 32.17 ms | 6.48 MiB | 1.98 M |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 68.96 k | — | — | 14,500.50 | 446.62 µs | 896.67 µs | 1.13 ms | 1.30 ms | 8.55 MiB | 11.03 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 417.60 | — | — | 2,394,660.00 | 152.10 ms | 306.02 ms | 399.44 ms | 516.19 ms | 6.52 MiB | 66.82 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 94.99 k | — | — | 10,527.60 | 686.88 µs | 1.30 ms | 1.75 ms | 1.95 ms | 12.91 MiB | 15.20 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 524.53 | — | — | 1,906,470.00 | 248.07 ms | 488.26 ms | 567.12 ms | 604.60 ms | 9.22 MiB | 83.92 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 17.03 k | — | — | 58,736.10 | 51.00 µs | 113.96 µs | 293.25 µs | 1.23 ms | 5.89 MiB | 2.72 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 14.94 k | — | — | 66,918.30 | 52.08 µs | 64.58 µs | 76.38 µs | 87.04 µs | 5.00 MiB | 2.39 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.89 k | — | — | 91,804.70 | 75.29 µs | 359.08 µs | 4.45 ms | 4.97 ms | 5.95 MiB | 1.74 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 44.68 k | — | — | 22,379.40 | 363.04 µs | 774.08 µs | 985.96 µs | 1.16 ms | 6.22 MiB | 7.15 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 31.31 k | — | — | 31,941.50 | 361.79 µs | 4.63 ms | 9.91 ms | 10.13 ms | 5.00 MiB | 5.01 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 11.85 k | — | — | 84,378.80 | 1.23 ms | 7.95 ms | 21.62 ms | 31.35 ms | 6.27 MiB | 1.90 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 35.69 k | — | — | 28,015.70 | 130.12 µs | 255.38 µs | 352.62 µs | 498.46 µs | 5.94 MiB | 5.71 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 32.82 k | — | — | 30,472.60 | 123.08 µs | 246.25 µs | 2.75 ms | 6.36 ms | 5.97 MiB | 5.25 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 15.20 k | — | — | 65,777.00 | 286.38 µs | 4.49 ms | 6.84 ms | 8.15 ms | 5.95 MiB | 2.43 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 53.93 k | — | — | 18,541.70 | 552.50 µs | 4.94 ms | 10.15 ms | 34.21 ms | 8.41 MiB | 8.63 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 13.04 k | — | — | 76,685.00 | 2.69 ms | 12.67 ms | 17.57 ms | 21.55 ms | 6.50 MiB | 2.09 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 102.25 k | — | — | 9,780.08 | 686.83 µs | 1.86 ms | 2.28 ms | 2.58 ms | 12.91 MiB | 16.36 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 25.32 k | — | — | 39,490.40 | 1.58 ms | 12.97 ms | 27.14 ms | 29.17 ms | 12.89 MiB | 4.05 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.78 k | — | — | 63,359.00 | 50.00 µs | 97.88 µs | 446.67 µs | 3.03 ms | 5.89 MiB | 2.53 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 11.91 k | — | — | 83,937.90 | 46.54 µs | 83.54 µs | 130.58 µs | 238.21 µs | 5.87 MiB | 1.91 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 453.33 | — | — | 2,205,900.00 | 4.04 ms | 5.04 ms | 6.00 ms | 11.74 ms | 5.87 MiB | 72.53 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 35.16 k | — | — | 28,445.20 | 473.54 µs | 1.03 ms | 1.56 ms | 1.94 ms | 5.25 MiB | 5.62 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 14.58 k | — | — | 68,600.30 | 508.29 µs | 5.89 ms | 6.84 ms | 7.06 ms | 5.92 MiB | 2.33 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 385.46 | — | — | 2,594,280.00 | 80.96 ms | 162.16 ms | 191.52 ms | 228.73 ms | 6.23 MiB | 61.67 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 37.15 k | — | — | 26,921.20 | 127.71 µs | 239.08 µs | 370.88 µs | 448.71 µs | 5.89 MiB | 5.94 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 14.02 k | — | — | 71,318.20 | 145.46 µs | 3.59 ms | 4.63 ms | 5.06 ms | 5.91 MiB | 2.24 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 427.68 | — | — | 2,338,230.00 | 20.02 ms | 39.98 ms | 42.96 ms | 47.15 ms | 6.00 MiB | 68.43 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 71.00 k | — | — | 14,085.30 | 439.71 µs | 906.62 µs | 1.43 ms | 1.55 ms | 8.34 MiB | 11.36 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 412.38 | — | — | 2,424,970.00 | 152.98 ms | 299.94 ms | 329.00 ms | 347.25 ms | 6.42 MiB | 65.98 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 91.28 k | — | — | 10,955.70 | 760.33 µs | 1.96 ms | 2.47 ms | 2.86 ms | 12.81 MiB | 14.60 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 632.19 | — | — | 1,581,790.00 | 198.02 ms | 403.01 ms | 453.15 ms | 504.94 ms | 9.02 MiB | 101.15 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 158.46 | — | — | 6,310,610.00 | — | — | — | — | 4.17 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 414.39 k | — | — | 2,413.17 | — | — | — | — | 4.62 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 324.27 | — | — | 3,083,810.00 | — | — | — | — | 4.64 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 377.60 | — | — | 2,648,330.00 | — | — | — | — | 4.70 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 353.33 | — | — | 2,830,180.00 | 10.04 ms | 16.94 ms | 26.33 ms | 304.46 ms | 4.84 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.27 k | — | — | 305,385.00 | — | — | — | — | 3.09 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 699.14 k | — | — | 1,430.33 | — | — | — | — | 3.30 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 5.58 k | — | — | 179,372.00 | — | — | — | — | 3.39 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 7.12 k | — | — | 140,382.00 | — | — | — | — | 3.44 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 192.69 | — | — | 5,189,580.00 | — | — | — | — | 3.12 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 366.33 k | — | — | 2,729.75 | — | — | — | — | 3.31 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 389.51 | — | — | 2,567,310.00 | — | — | — | — | 3.41 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 371.34 | — | — | 2,692,950.00 | — | — | — | — | 3.47 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 180.35 k | — | — | 5,544.67 | — | — | — | — | 3.72 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 323.73 | — | — | 3,088,970.00 | 12.96 ms | 22.33 ms | 34.85 ms | 448.35 ms | 3.56 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 3.75 M | — | — | 266.91 | — | — | — | — | 240.05 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 3.70 M | — | — | 270.42 | — | — | — | — | 234.80 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 3.81 M | — | — | 262.73 | — | — | — | — | 240.69 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 9.19 M | — | — | 108.81 | — | — | — | — | 156.86 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 5.84 M | — | — | 171.17 | — | — | — | — | 158.83 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.49 M | — | — | 154.11 | — | — | — | — | 177.87 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 14.39 M | — | — | 69.48 | — | — | — | — | 139.83 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 10.88 M | — | — | 91.87 | — | — | — | — | 144.56 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 10.50 M | — | — | 95.27 | — | — | — | — | 131.03 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 24.50 M | — | — | 40.81 | — | — | — | — | 142.64 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 12.26 M | — | — | 81.58 | — | — | — | — | 131.61 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.87 M | — | — | 91.98 | — | — | — | — | 137.17 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 582.45 k | — | — | 1,716.89 | — | — | — | — | 82.00 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 584.63 k | — | — | 1,710.48 | — | — | — | — | 87.22 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 588.70 k | — | — | 1,698.65 | — | — | — | — | 82.03 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 724.57 k | — | — | 1,380.13 | — | — | — | — | 77.81 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 493.04 k | — | — | 2,028.23 | — | — | — | — | 75.92 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 415.68 k | — | — | 2,405.70 | — | — | — | — | 71.88 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.19 M | — | — | 839.78 | — | — | — | — | 64.22 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 513.75 k | — | — | 1,946.45 | — | — | — | — | 67.17 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 544.82 k | — | — | 1,835.47 | — | — | — | — | 69.02 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 2.19 M | — | — | 457.52 | — | — | — | — | 69.83 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 549.44 k | — | — | 1,820.03 | — | — | — | — | 83.20 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 502.07 k | — | — | 1,991.73 | — | — | — | — | 84.17 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 1.05 M | — | — | 949.20 | — | — | — | — | 83.91 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.26 M | — | — | 796.23 | — | — | — | — | 65.25 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 2.14 M | — | — | 467.21 | — | — | — | — | 63.14 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.32 M | — | — | 300.90 | — | — | — | — | 66.89 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 332.96 k | — | — | 3,003.37 | 181.00 µs | 379.50 µs | 918.04 µs | 2.01 ms | 32.48 MiB | 53.27 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 45.58 k | — | — | 21,939.30 | 21.88 µs | 26.38 µs | 50.21 µs | 167.83 µs | 32.89 MiB | 7.29 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 45.11 k | — | — | 22,169.70 | 21.96 µs | 28.88 µs | 48.79 µs | 237.71 µs | 32.70 MiB | 7.22 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 48.93 k | — | — | 20,436.30 | 40.29 µs | 49.46 µs | 86.00 µs | 184.67 µs | 37.73 MiB | 7.83 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.82 M | — | — | 548.21 | 65.88 µs | 104.08 µs | 171.79 µs | 267.92 µs | 101.86 MiB | 291.86 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.35 M | — | — | 738.72 | 82.75 µs | 97.71 µs | 107.62 µs | 124.71 µs | 105.06 MiB | 216.59 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 287.13 k | — | — | 3,482.74 | 392.00 µs | 1.26 ms | 3.74 ms | 9.28 ms | 127.89 MiB | 45.94 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 439.08 k | — | — | 2,277.50 | 61.25 µs | 87.75 µs | 156.04 µs | 287.92 µs | 155.73 MiB | 491.77 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 885.92 k | — | — | 1,128.77 | 34.75 µs | 38.17 µs | 41.62 µs | 51.12 µs | 97.28 MiB | 141.75 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 12.27 k | — | — | 81,471.90 | 1.69 ms | 2.54 ms | 2.75 ms | 3.15 ms | 412.23 MiB | 805.58 M |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 747.53 k | — | — | 1,337.74 | 36.21 µs | 58.62 µs | 87.12 µs | 174.58 µs | 101.59 MiB | 119.61 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 349.40 k | — | — | 2,862.02 | 112.50 µs | 228.21 µs | 628.58 µs | 2.58 ms | 128.42 MiB | 55.90 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 3.28 k | — | — | 305,251.00 | 1.54 ms | 2.31 ms | 2.64 ms | 3.25 ms | 483.30 MiB | 859.10 M |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 294.59 k | — | — | 3,394.54 | 25.96 µs | 34.67 µs | 60.46 µs | 155.25 µs | 97.14 MiB | 47.13 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 285.58 k | — | — | 3,501.69 | 26.00 µs | 45.88 µs | 65.29 µs | 184.83 µs | 100.89 MiB | 45.69 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 172.18 k | — | — | 5,808.00 | 67.08 µs | 122.79 µs | 223.75 µs | 742.42 µs | 124.17 MiB | 27.55 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 75.10 k | — | — | 13,315.80 | 25.46 µs | 46.71 µs | 77.62 µs | 413.92 µs | 31.17 MiB | 12.02 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 80.19 k | — | — | 12,470.40 | 23.83 µs | 33.25 µs | 65.33 µs | 275.92 µs | 33.25 MiB | 12.83 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 84.99 k | — | — | 11,766.70 | 46.46 µs | 76.67 µs | 113.71 µs | 233.46 µs | 38.72 MiB | 13.60 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.47 M | — | — | 288.09 | 68.92 µs | 125.21 µs | 189.96 µs | 318.83 µs | 82.91 MiB | 555.39 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 2.15 M | — | — | 464.31 | 75.50 µs | 151.83 µs | 233.92 µs | 369.00 µs | 83.30 MiB | 344.60 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 341.04 k | — | — | 2,932.17 | 704.33 µs | 2.03 ms | 4.16 ms | 12.63 ms | 114.98 MiB | 54.57 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.64 M | — | — | 607.94 | 37.25 µs | 42.71 µs | 45.83 µs | 56.08 µs | 79.39 MiB | 263.18 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.28 M | — | — | 783.86 | 38.21 µs | 58.58 µs | 70.67 µs | 94.79 µs | 84.67 MiB | 204.12 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 375.82 k | — | — | 2,660.82 | 216.83 µs | 561.17 µs | 1.15 ms | 2.64 ms | 113.91 MiB | 60.13 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 595.44 k | — | — | 1,679.44 | 26.25 µs | 38.12 µs | 66.83 µs | 132.58 µs | 84.47 MiB | 95.27 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 535.60 k | — | — | 1,867.06 | 26.54 µs | 38.25 µs | 51.50 µs | 78.58 µs | 84.58 MiB | 85.70 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 229.34 k | — | — | 4,360.28 | 93.58 µs | 218.88 µs | 372.96 µs | 860.92 µs | 114.41 MiB | 36.69 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 97.13 k | — | — | 10,295.30 | 39.92 µs | 62.75 µs | 107.21 µs | 831.25 µs | 33.48 MiB | 15.54 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 107.30 k | — | — | 9,319.88 | 33.88 µs | 57.04 µs | 72.08 µs | 286.58 µs | 33.11 MiB | 17.17 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 110.79 k | — | — | 9,026.33 | 71.83 µs | 115.88 µs | 252.58 µs | 860.67 µs | 42.13 MiB | 17.73 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.52 M | — | — | 181.28 | 89.96 µs | 209.96 µs | 367.75 µs | 920.75 µs | 77.16 MiB | 882.63 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 2.98 M | — | — | 336.00 | 92.42 µs | 224.25 µs | 323.29 µs | 795.75 µs | 81.64 MiB | 476.20 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 467.19 k | — | — | 2,140.46 | 1.11 ms | 2.56 ms | 3.80 ms | 23.44 ms | 115.63 MiB | 74.75 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.59 M | — | — | 385.71 | 45.46 µs | 94.75 µs | 221.75 µs | 680.50 µs | 75.37 MiB | 414.82 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.73 M | — | — | 578.83 | 49.92 µs | 101.79 µs | 170.38 µs | 299.38 µs | 77.75 MiB | 276.42 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 395.15 k | — | — | 2,530.68 | 366.12 µs | 829.38 µs | 1.44 ms | 3.58 ms | 112.05 MiB | 63.22 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 829.59 k | — | — | 1,205.41 | 35.29 µs | 58.29 µs | 69.96 µs | 85.33 µs | 76.36 MiB | 132.74 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 722.34 k | — | — | 1,384.39 | 39.50 µs | 94.08 µs | 169.17 µs | 423.79 µs | 78.73 MiB | 115.57 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 264.25 k | — | — | 3,784.24 | 156.12 µs | 297.75 µs | 496.25 µs | 1.56 ms | 115.34 MiB | 42.28 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 132.27 k | — | — | 7,560.17 | 59.83 µs | 82.62 µs | 186.62 µs | 340.83 µs | 49.41 MiB | 21.16 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 112.53 k | — | — | 8,886.78 | 65.67 µs | 142.62 µs | 274.12 µs | 1.31 ms | 40.98 MiB | 18.00 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 132.54 k | — | — | 7,544.85 | 102.21 µs | 166.67 µs | 356.83 µs | 2.29 ms | 50.25 MiB | 21.21 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 9.32 M | — | — | 107.32 | 100.79 µs | 130.00 µs | 152.54 µs | 217.88 µs | 88.50 MiB | 1.49 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.88 M | — | — | 204.95 | 109.04 µs | 197.25 µs | 258.17 µs | 359.21 µs | 90.33 MiB | 780.68 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 349.10 k | — | — | 2,864.52 | 2.13 ms | 7.01 ms | 19.27 ms | 135.61 ms | 119.31 MiB | 55.86 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.67 M | — | — | 272.58 | 66.42 µs | 86.42 µs | 96.58 µs | 114.88 µs | 92.03 MiB | 586.99 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.65 M | — | — | 376.96 | 72.54 µs | 225.42 µs | 376.38 µs | 636.54 µs | 94.16 MiB | 424.45 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 347.83 k | — | — | 2,874.92 | 651.58 µs | 1.79 ms | 5.01 ms | 17.20 ms | 125.78 MiB | 55.65 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 1.00 M | — | — | 999.27 | 65.12 µs | 203.92 µs | 455.50 µs | 1.64 ms | 93.97 MiB | 160.12 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 905.11 k | — | — | 1,104.84 | 63.62 µs | 87.21 µs | 114.92 µs | 339.12 µs | 89.53 MiB | 144.82 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 269.89 k | — | — | 3,705.26 | 234.25 µs | 530.58 µs | 1.34 ms | 4.08 ms | 120.23 MiB | 43.18 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| index-all-k16-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | fae4547-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-get-only | 44.14 µs / 305.29 µs | 32 rec / 6656 B | 944.63 µs / 6.47 ms | 1.04 ms / 6.41 ms | 30.00 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-99-write-1 | 42.15 µs / 333.88 µs | 23 rec / 6656 B | 1.02 ms / 11.18 ms | 1.95 ms / 11.16 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 296.44 µs / 7.63 ms | 1 rec / 208 B | 4.62 ms / 12.47 ms | 4.57 ms / 12.40 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-get-only | 42.69 µs / 13.33 ms | 32 rec / 6656 B | 1.52 ms / 13.63 ms | 1.36 ms / 13.53 ms | 31.25 / 32.00 | 0 rec / 0 B | 15/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 41.74 µs / 379.96 µs | 32 rec / 6656 B | 1.02 ms / 8.22 ms | 1.87 ms / 8.12 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 309.11 µs / 3.19 ms | 1 rec / 208 B | 4.69 ms / 27.32 ms | 4.65 ms / 27.27 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-get-only | 38.73 µs / 293.38 µs | 20 rec / 6656 B | 1.13 ms / 8.75 ms | 1.25 ms / 8.65 ms | 30.00 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 47.24 µs / 338.29 µs | 32 rec / 6656 B | 1.04 ms / 5.33 ms | 1.76 ms / 5.26 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 306.97 µs / 1.15 ms | 1 rec / 208 B | 4.41 ms / 11.26 ms | 4.34 ms / 11.15 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 856.21 µs / 13.59 ms | 4 rec / 832 B | 312.11 µs / 10.36 ms | 298.68 µs / 10.35 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 286.02 µs / 754.62 µs | 4 rec / 832 B | 6.76 ms / 430.44 ms | 6.67 ms / 430.15 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 287.08 µs / 2.72 ms | 4 rec / 832 B | 5.01 ms / 26.35 ms | 4.93 ms / 26.18 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 288.34 µs / 2.89 ms | 4 rec / 832 B | 4.82 ms / 435.44 ms | 4.73 ms / 435.32 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 25.68 µs / 1.61 ms | 4 rec / 832 B | 446.80 µs / 4.91 ms | 387.13 µs / 4.55 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-get-only | 62.64 µs / 6.44 ms | 32 rec / 6656 B | 2.18 ms / 11.90 ms | 1.77 ms / 11.62 ms | 31.25 / 32.00 | 0 rec / 0 B | 14/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 313.16 µs / 2.12 ms | 1 rec / 208 B | 9.16 ms / 34.52 ms | 9.12 ms / 34.44 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-get-only | 115.71 µs / 8.72 ms | 32 rec / 6656 B | 2.80 ms / 12.19 ms | 2.68 ms / 12.05 ms | 29.41 / 32.00 | 0 rec / 0 B | 11/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 314.24 µs / 4.38 ms | 1 rec / 208 B | 14.83 ms / 36.85 ms | 14.79 ms / 36.81 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-get-only | 201.14 µs / 5.79 ms | 32 rec / 6656 B | 421.50 µs / 5.80 ms | 533.05 µs / 5.67 ms | 30.00 / 32.00 | 12 rec / 1632 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 281.04 µs / 6.04 ms | 24 rec / 5616 B | 425.18 µs / 6.03 ms | 718.45 µs / 4.88 ms | 30.30 / 32.00 | 1 rec / 136 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 4.60 µs / 70.17 µs | 1 rec / 208 B | 109.27 µs / 5.11 ms | 895.20 µs / 1.88 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-get-only | 1.56 ms / 19.13 ms | 32 rec / 6656 B | 2.20 ms / 19.14 ms | 1.01 ms / 8.95 ms | 30.69 / 32.00 | 20 rec / 2720 B | 15/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 789.98 µs / 9.81 ms | 29 rec / 6656 B | 963.65 µs / 9.78 ms | 796.44 µs / 2.63 ms | 32.00 / 32.00 | 15 rec / 2040 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 6.32 µs / 38.83 µs | 1 rec / 208 B | 119.83 µs / 30.37 ms | 761.22 µs / 2.64 ms | 32.00 / 32.00 | 29 rec / 3944 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-get-only | 657.87 µs / 15.54 ms | 30 rec / 6656 B | 1.44 ms / 15.51 ms | 808.49 µs / 7.58 ms | 32.00 / 32.00 | 29 rec / 3944 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 490.72 µs / 15.43 ms | 32 rec / 6656 B | 1.27 ms / 15.47 ms | 778.64 µs / 7.07 ms | 30.30 / 32.00 | 15 rec / 2040 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 7.27 µs / 91.25 µs | 1 rec / 208 B | 81.22 µs / 7.65 ms | 558.70 µs / 6.85 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w2-c2-p32-get-only | 986.63 µs / 27.26 ms | 32 rec / 6656 B | 2.39 ms / 27.24 ms | 1.54 ms / 12.82 ms | 31.60 / 32.00 | 26 rec / 3536 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 8.45 µs / 630.92 µs | 1 rec / 208 B | 245.50 µs / 10.94 ms | 3.29 ms / 8.87 ms | 30.69 / 32.00 | 52 rec / 7072 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w4-c4-p32-get-only | 1.18 ms / 29.97 ms | 32 rec / 6656 B | 1.47 ms / 29.95 ms | 2.34 ms / 29.88 ms | 31.40 / 32.00 | 29 rec / 3944 B | 12/0/0/3 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 10.93 µs / 502.12 µs | 1 rec / 208 B | 169.96 µs / 28.27 ms | 3.72 ms / 28.27 ms | 31.69 / 32.00 | 116 rec / 15776 B | 12/0/0/1 | 0/0/0 |
| durable-sync-w1-c1-p1-get-only | 46.26 ms / 125.01 ms | 26 rec / 6656 B | 78.05 ms / 124.97 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 40.23 ms / 123.95 ms | 32 rec / 6656 B | 75.83 ms / 123.98 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 8.74 µs / 44.00 µs | 1 rec / 208 B | 4.28 ms / 11.78 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-get-only | 45.04 ms / 156.32 ms | 32 rec / 6656 B | 93.18 ms / 165.08 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 42.97 ms / 149.79 ms | 32 rec / 6656 B | 91.43 ms / 149.74 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 12.50 µs / 1.79 ms | 1 rec / 208 B | 5.04 ms / 16.68 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-get-only | 44.16 ms / 129.93 ms | 28 rec / 6656 B | 76.97 ms / 129.90 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 38.92 ms / 121.19 ms | 25 rec / 6448 B | 75.33 ms / 121.17 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 9.73 µs / 1.15 ms | 1 rec / 208 B | 4.54 ms / 12.85 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-get-only | 47.03 ms / 141.91 ms | 26 rec / 6448 B | 92.70 ms / 141.86 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 12.06 µs / 151.58 µs | 1 rec / 208 B | 9.61 ms / 22.89 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-get-only | 43.43 ms / 172.00 ms | 32 rec / 6656 B | 85.96 ms / 171.95 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 12.51 µs / 899.62 µs | 1 rec / 208 B | 12.48 ms / 28.80 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
