# GlifiStore benchmark report

Generated at `2026-09-15T11:02:19+00:00` from 180 canonical result(s) and 108 specialized diagnostic(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

Baseline report: `2026-08-30T11:18:14+00:00`.

Environment identity: **incompatible** (`identity-fields-missing`); benchmark deltas are suppressed.

Missing current identity fields: `none`.
Missing baseline identity fields: `cmake_identity, ninja_identity`.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 12.40 M | — | — | 80.62 | — | — | — | — | 40.98 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 8.36 M | — | — | 119.62 | — | — | — | — | 41.03 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 12.61 M | — | — | 79.32 | — | — | — | — | 41.03 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 13.83 M | — | — | 72.29 | — | — | — | — | 47.17 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.81 M | — | — | 59.50 | — | — | — | — | 63.44 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 12.16 M | — | — | 82.23 | — | — | — | — | 63.73 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.65 M | — | — | 605.03 | — | — | — | — | 478.20 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 3.71 M | — | — | 269.25 | — | — | — | — | 91.70 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 2.83 M | — | — | 353.09 | — | — | — | — | 186.36 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.19 k | — | — | 61,783.70 | — | — | — | — | 504.91 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 735.00 k | — | — | 1,360.55 | — | — | — | — | 1,649.12 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 3.86 M | — | — | 259.13 | — | — | — | — | 111.02 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 62.81 k | — | — | 15,921.90 | — | — | — | — | 506.34 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 566.74 k | — | — | 1,764.47 | — | — | — | — | 477.95 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 601.29 k | — | — | 1,663.10 | — | — | — | — | 92.63 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 669.23 k | — | — | 1,494.26 | — | — | — | — | 184.34 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 14.80 k | — | — | 67,580.50 | — | — | — | — | 504.47 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 362.92 k | — | — | 2,755.46 | — | — | — | — | 1,652.36 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 697.54 k | — | — | 1,433.61 | — | — | — | — | 111.19 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 58.67 k | — | — | 17,044.20 | — | — | — | — | 506.36 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 580.09 k | — | — | 1,723.87 | — | — | — | — | 477.09 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 718.61 k | — | — | 1,391.57 | — | — | — | — | 91.50 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 719.74 k | — | — | 1,389.39 | — | — | — | — | 184.41 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.32 k | — | — | 65,261.30 | — | — | — | — | 504.88 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 393.70 k | — | — | 2,539.98 | — | — | — | — | 1,651.19 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 766.31 k | — | — | 1,304.95 | — | — | — | — | 109.73 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 60.61 k | — | — | 16,497.60 | — | — | — | — | 506.83 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 332.65 k | — | — | 3,006.14 | — | — | — | — | 478.06 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 400.16 k | — | — | 2,498.98 | — | — | — | — | 91.63 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 418.36 k | — | — | 2,390.27 | — | — | — | — | 183.09 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 13.88 k | — | — | 72,047.90 | — | — | — | — | 504.34 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 264.13 k | — | — | 3,785.96 | — | — | — | — | 1,653.06 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 414.91 k | — | — | 2,410.14 | — | — | — | — | 109.73 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 51.14 k | — | — | 19,554.20 | — | — | — | — | 506.22 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 639.14 k | — | — | 1,564.60 | — | — | — | — | 476.75 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 787.61 k | — | — | 1,269.67 | — | — | — | — | 92.70 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 783.91 k | — | — | 1,275.66 | — | — | — | — | 183.09 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 14.97 k | — | — | 66,802.90 | — | — | — | — | 504.92 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 394.96 k | — | — | 2,531.90 | — | — | — | — | 1,652.45 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 833.98 k | — | — | 1,199.08 | — | — | — | — | 110.06 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 57.61 k | — | — | 17,358.40 | — | — | — | — | 506.30 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.56 k | — | — | 64,278.60 | 62.25 µs | 85.83 µs | 120.21 µs | 169.17 µs | 5.19 MiB | 2.49 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 8.92 k | — | — | 112,063.00 | 60.12 µs | 98.46 µs | 159.79 µs | 296.25 µs | 6.06 MiB | 1.43 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 398.53 | — | — | 2,509,200.00 | 4.97 ms | 6.11 ms | 11.75 ms | 26.27 ms | 6.23 MiB | 63.77 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 42.86 k | — | — | 23,329.40 | 367.79 µs | 787.46 µs | 1.55 ms | 3.48 ms | 6.27 MiB | 6.86 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 11.70 k | — | — | 85,482.60 | 570.12 µs | 10.01 ms | 20.86 ms | 22.69 ms | 5.27 MiB | 1.87 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 231.97 | — | — | 4,310,990.00 | 96.58 ms | 344.14 ms | 1138.57 ms | 1645.30 ms | 6.08 MiB | 37.11 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 41.21 k | — | — | 24,264.20 | 121.42 µs | 234.50 µs | 342.21 µs | 499.50 µs | 6.27 MiB | 6.59 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.68 k | — | — | 78,842.20 | 125.58 µs | 5.04 ms | 6.32 ms | 8.31 ms | 6.25 MiB | 2.03 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 352.53 | — | — | 2,836,640.00 | 24.87 ms | 48.36 ms | 66.99 ms | 98.40 ms | 6.20 MiB | 56.40 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 438.93 | — | — | 2,278,260.00 | 267.53 ms | 605.01 ms | 1250.74 ms | 1478.79 ms | 5.83 MiB | 70.23 k |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.35 k | — | — | 738,228.00 | 89.41 ms | 282.81 ms | 948.49 ms | 1006.66 ms | 6.95 MiB | 216.74 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.35 k | — | — | 741,534.00 | 99.10 ms | 232.53 ms | 305.32 ms | 857.58 ms | 6.75 MiB | 215.77 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.20 k | — | — | 833,780.00 | 94.19 ms | 306.82 ms | 452.15 ms | 571.21 ms | 6.67 MiB | 191.90 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.37 k | — | — | 730,770.00 | 89.01 ms | 683.46 ms | 879.16 ms | 914.64 ms | 6.77 MiB | 218.95 k |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 70.54 k | — | — | 14,175.90 | 457.75 µs | 975.25 µs | 1.28 ms | 1.44 ms | 6.80 MiB | 11.29 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 289.18 | — | — | 3,458,070.00 | 169.79 ms | 530.97 ms | 1168.35 ms | 1385.44 ms | 8.34 MiB | 46.27 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 124.53 k | — | — | 8,030.00 | 616.58 µs | 1.50 ms | 2.22 ms | 2.59 ms | 13.25 MiB | 19.93 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 407.03 | — | — | 2,456,800.00 | 298.19 ms | 803.84 ms | 1393.11 ms | 1514.16 ms | 9.52 MiB | 65.13 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.32 k | — | — | 65,289.40 | 56.33 µs | 125.88 µs | 249.33 µs | 614.67 µs | 5.23 MiB | 2.45 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.20 k | — | — | 65,796.50 | 56.67 µs | 88.83 µs | 142.08 µs | 333.83 µs | 5.20 MiB | 2.43 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.92 k | — | — | 91,568.60 | 71.88 µs | 347.67 µs | 4.17 ms | 5.20 ms | 5.16 MiB | 1.75 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 36.73 k | — | — | 27,227.60 | 449.88 µs | 912.21 µs | 1.05 ms | 1.29 ms | 6.48 MiB | 5.88 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 33.70 k | — | — | 29,671.00 | 392.62 µs | 3.31 ms | 5.06 ms | 5.29 ms | 6.23 MiB | 5.39 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 6.43 k | — | — | 155,501.00 | 1.38 ms | 18.57 ms | 28.43 ms | 28.99 ms | 6.48 MiB | 1.03 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 37.35 k | — | — | 26,771.90 | 124.92 µs | 259.08 µs | 433.38 µs | 786.88 µs | 6.20 MiB | 5.98 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 25.78 k | — | — | 38,795.10 | 129.75 µs | 273.62 µs | 2.83 ms | 4.61 ms | 6.25 MiB | 4.12 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.14 k | — | — | 82,352.20 | 264.21 µs | 5.25 ms | 7.97 ms | 9.07 ms | 5.27 MiB | 1.94 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 60.52 k | — | — | 16,522.20 | 489.25 µs | 1.51 ms | 55.88 ms | 59.29 ms | 6.77 MiB | 9.68 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 15.85 k | — | — | 63,102.70 | 4.46 ms | 11.17 ms | 15.80 ms | 21.98 ms | 7.73 MiB | 2.54 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 107.56 k | — | — | 9,296.67 | 585.96 µs | 1.35 ms | 2.16 ms | 2.35 ms | 9.39 MiB | 17.21 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 21.43 k | — | — | 46,654.80 | 1.80 ms | 16.59 ms | 25.18 ms | 49.16 ms | 13.22 MiB | 3.43 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 16.44 k | — | — | 60,844.20 | 57.33 µs | 89.33 µs | 122.33 µs | 169.42 µs | 5.20 MiB | 2.63 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.11 k | — | — | 98,880.70 | 46.71 µs | 85.50 µs | 136.42 µs | 290.21 µs | 5.16 MiB | 1.62 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 444.69 | — | — | 2,248,770.00 | 4.71 ms | 6.04 ms | 7.42 ms | 12.51 ms | 5.19 MiB | 71.15 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 50.97 k | — | — | 19,621.10 | 343.46 µs | 698.21 µs | 880.92 µs | 1.04 ms | 6.31 MiB | 8.15 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 13.92 k | — | — | 71,823.50 | 549.38 µs | 5.64 ms | 6.25 ms | 6.63 ms | 5.22 MiB | 2.23 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 414.84 | — | — | 2,410,560.00 | 77.20 ms | 151.02 ms | 168.44 ms | 184.17 ms | 5.52 MiB | 66.37 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 33.63 k | — | — | 29,733.20 | 140.21 µs | 282.00 µs | 366.25 µs | 479.54 µs | 5.16 MiB | 5.38 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 14.39 k | — | — | 69,507.60 | 132.42 µs | 3.53 ms | 4.87 ms | 4.98 ms | 5.20 MiB | 2.30 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 464.47 | — | — | 2,152,980.00 | 19.04 ms | 36.59 ms | 49.96 ms | 109.41 ms | 5.28 MiB | 74.32 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 83.21 k | — | — | 12,017.20 | 408.08 µs | 869.58 µs | 1.10 ms | 1.24 ms | 7.77 MiB | 13.31 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 398.70 | — | — | 2,508,160.00 | 160.95 ms | 308.23 ms | 352.40 ms | 426.68 ms | 8.52 MiB | 63.79 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 81.31 k | — | — | 12,299.30 | 721.08 µs | 2.04 ms | 2.42 ms | 2.83 ms | 9.08 MiB | 13.01 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 552.39 | — | — | 1,810,310.00 | 226.15 ms | 474.03 ms | 1122.51 ms | 1235.03 ms | 9.34 MiB | 88.38 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 190.98 | — | — | 5,236,030.00 | — | — | — | — | 4.23 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 818.78 k | — | — | 1,221.33 | — | — | — | — | 4.28 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 384.91 | — | — | 2,598,020.00 | — | — | — | — | 4.31 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 370.10 | — | — | 2,701,980.00 | — | — | — | — | 4.45 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 459.00 | — | — | 2,178,630.00 | 8.82 ms | 13.00 ms | 23.99 ms | 57.50 ms | 5.03 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.67 k | — | — | 272,609.00 | — | — | — | — | 3.50 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 777.81 k | — | — | 1,285.67 | — | — | — | — | 3.78 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 7.31 k | — | — | 136,757.00 | — | — | — | — | 3.91 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 7.15 k | — | — | 139,838.00 | — | — | — | — | 4.00 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 158.67 | — | — | 6,302,300.00 | — | — | — | — | 3.33 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 750.05 k | — | — | 1,333.25 | — | — | — | — | 3.61 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 330.86 | — | — | 3,022,470.00 | — | — | — | — | 3.73 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 392.57 | — | — | 2,547,300.00 | — | — | — | — | 3.80 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 260.55 k | — | — | 3,838.08 | — | — | — | — | 4.03 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 330.84 | — | — | 3,022,570.00 | 12.08 ms | 18.05 ms | 22.45 ms | 32.99 ms | 4.03 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 4.36 M | — | — | 229.13 | — | — | — | — | 111.33 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 4.31 M | — | — | 231.88 | — | — | — | — | 111.33 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 4.38 M | — | — | 228.56 | — | — | — | — | 111.31 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 9.55 M | — | — | 104.76 | — | — | — | — | 113.11 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 6.08 M | — | — | 164.46 | — | — | — | — | 113.12 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.55 M | — | — | 152.77 | — | — | — | — | 113.67 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 17.65 M | — | — | 56.66 | — | — | — | — | 116.20 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 9.91 M | — | — | 100.94 | — | — | — | — | 116.17 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 10.24 M | — | — | 97.61 | — | — | — | — | 100.20 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 24.63 M | — | — | 40.59 | — | — | — | — | 118.50 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 12.88 M | — | — | 77.63 | — | — | — | — | 119.38 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.60 M | — | — | 94.31 | — | — | — | — | 105.48 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 493.23 k | — | — | 2,027.47 | — | — | — | — | 57.72 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 480.88 k | — | — | 2,079.53 | — | — | — | — | 58.95 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 479.25 k | — | — | 2,086.57 | — | — | — | — | 57.69 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 683.44 k | — | — | 1,463.19 | — | — | — | — | 61.30 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 462.38 k | — | — | 2,162.70 | — | — | — | — | 61.52 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 401.88 k | — | — | 2,488.33 | — | — | — | — | 60.23 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.10 M | — | — | 905.46 | — | — | — | — | 67.22 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 481.39 k | — | — | 2,077.31 | — | — | — | — | 60.77 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 430.55 k | — | — | 2,322.59 | — | — | — | — | 54.64 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 1.80 M | — | — | 555.32 | — | — | — | — | 60.73 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 509.25 k | — | — | 1,963.69 | — | — | — | — | 68.50 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 529.73 k | — | — | 1,887.75 | — | — | — | — | 73.77 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 936.82 k | — | — | 1,067.44 | — | — | — | — | 57.75 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.30 M | — | — | 767.67 | — | — | — | — | 61.08 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 1.84 M | — | — | 544.02 | — | — | — | — | 76.88 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.33 M | — | — | 300.23 | — | — | — | — | 68.39 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 148.33 k | — | — | 6,741.61 | 389.29 µs | 835.25 µs | 1.10 ms | 2.19 ms | 30.89 MiB | 23.73 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 41.76 k | — | — | 23,946.30 | 22.33 µs | 34.25 µs | 71.62 µs | 133.17 µs | 29.91 MiB | 6.68 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 41.50 k | — | — | 24,094.20 | 22.33 µs | 33.67 µs | 70.50 µs | 141.29 µs | 30.80 MiB | 6.64 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 48.44 k | — | — | 20,644.30 | 36.75 µs | 71.00 µs | 146.25 µs | 244.12 µs | 35.94 MiB | 7.75 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.28 M | — | — | 778.79 | 83.83 µs | 163.21 µs | 255.33 µs | 354.17 µs | 72.12 MiB | 205.45 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.32 M | — | — | 758.85 | 82.46 µs | 106.08 µs | 157.54 µs | 236.71 µs | 77.06 MiB | 210.85 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 157.55 k | — | — | 6,347.14 | 794.04 µs | 1.80 ms | 3.45 ms | 6.71 ms | 99.61 MiB | 25.21 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 431.63 k | — | — | 2,316.80 | 63.33 µs | 104.04 µs | 170.00 µs | 400.25 µs | 132.62 MiB | 483.42 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 829.78 k | — | — | 1,205.14 | 35.29 µs | 51.58 µs | 95.17 µs | 158.67 µs | 71.61 MiB | 132.76 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 21.98 k | — | — | 45,505.70 | 807.21 µs | 1.61 ms | 4.18 ms | 18.15 ms | 388.36 MiB | 1.44 G |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 700.76 k | — | — | 1,427.02 | 37.50 µs | 61.75 µs | 106.50 µs | 179.54 µs | 74.44 MiB | 112.12 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 83.42 k | — | — | 11,987.70 | 334.83 µs | 911.38 µs | 1.24 ms | 4.25 ms | 100.69 MiB | 13.35 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 6.00 k | — | — | 166,782.00 | 714.46 µs | 1.19 ms | 1.45 ms | 1.72 ms | 412.44 MiB | 1.57 G |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 225.12 k | — | — | 4,442.09 | 28.25 µs | 74.12 µs | 118.12 µs | 190.21 µs | 71.86 MiB | 36.02 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 270.19 k | — | — | 3,701.14 | 25.75 µs | 44.21 µs | 88.00 µs | 180.92 µs | 74.02 MiB | 43.23 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 72.76 k | — | — | 13,743.20 | 113.38 µs | 302.38 µs | 418.25 µs | 1.23 ms | 100.09 MiB | 11.64 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 79.50 k | — | — | 12,578.50 | 24.12 µs | 34.46 µs | 64.83 µs | 104.67 µs | 27.78 MiB | 12.72 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 77.43 k | — | — | 12,915.40 | 24.88 µs | 52.79 µs | 69.04 µs | 102.08 µs | 32.50 MiB | 12.39 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 75.63 k | — | — | 13,222.90 | 44.62 µs | 100.88 µs | 124.12 µs | 429.92 µs | 36.11 MiB | 12.10 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 2.34 M | — | — | 426.80 | 98.62 µs | 188.92 µs | 368.96 µs | 814.33 µs | 69.69 MiB | 374.89 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 1.27 M | — | — | 788.23 | 124.54 µs | 227.46 µs | 308.04 µs | 654.12 µs | 72.45 MiB | 202.99 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 147.28 k | — | — | 6,789.58 | 1.61 ms | 3.41 ms | 4.09 ms | 5.71 ms | 99.22 MiB | 23.57 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 831.15 k | — | — | 1,203.15 | 71.25 µs | 115.00 µs | 203.96 µs | 783.88 µs | 70.03 MiB | 132.99 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 977.21 k | — | — | 1,023.32 | 42.88 µs | 91.33 µs | 125.83 µs | 303.25 µs | 71.41 MiB | 156.35 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 124.33 k | — | — | 8,042.76 | 507.58 µs | 1.09 ms | 2.08 ms | 9.50 ms | 102.42 MiB | 19.89 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 545.24 k | — | — | 1,834.05 | 25.79 µs | 55.46 µs | 77.04 µs | 137.38 µs | 69.72 MiB | 87.24 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 461.90 k | — | — | 2,164.97 | 27.92 µs | 70.83 µs | 98.50 µs | 298.42 µs | 70.64 MiB | 73.90 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 125.86 k | — | — | 7,945.56 | 153.54 µs | 330.29 µs | 454.67 µs | 1.40 ms | 102.62 MiB | 20.14 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 83.73 k | — | — | 11,942.70 | 43.04 µs | 69.00 µs | 106.58 µs | 464.12 µs | 32.97 MiB | 13.40 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 85.88 k | — | — | 11,644.30 | 43.50 µs | 69.12 µs | 87.83 µs | 165.88 µs | 31.94 MiB | 13.74 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 93.36 k | — | — | 10,710.70 | 76.00 µs | 131.04 µs | 294.54 µs | 2.38 ms | 39.95 MiB | 14.94 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 4.23 M | — | — | 236.65 | 109.25 µs | 162.71 µs | 218.33 µs | 463.17 µs | 68.56 MiB | 676.12 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 2.26 M | — | — | 442.11 | 108.12 µs | 200.96 µs | 281.96 µs | 459.71 µs | 71.45 MiB | 361.90 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 192.56 k | — | — | 5,193.21 | 2.63 ms | 6.57 ms | 13.22 ms | 107.36 ms | 105.72 MiB | 30.81 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.50 M | — | — | 664.97 | 69.17 µs | 181.25 µs | 804.54 µs | 3.83 ms | 73.42 MiB | 240.61 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.17 M | — | — | 855.60 | 65.21 µs | 113.04 µs | 156.33 µs | 345.58 µs | 72.25 MiB | 187.00 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 222.71 k | — | — | 4,490.11 | 586.83 µs | 1.22 ms | 1.89 ms | 4.42 ms | 105.66 MiB | 35.63 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 570.05 k | — | — | 1,754.24 | 52.21 µs | 82.83 µs | 154.46 µs | 2.01 ms | 70.61 MiB | 91.21 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 560.52 k | — | — | 1,784.04 | 50.38 µs | 78.33 µs | 106.12 µs | 212.92 µs | 70.91 MiB | 89.68 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 201.63 k | — | — | 4,959.66 | 184.75 µs | 346.21 µs | 521.83 µs | 1.41 ms | 110.14 MiB | 32.26 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 107.64 k | — | — | 9,290.44 | 69.46 µs | 99.12 µs | 155.79 µs | 351.00 µs | 40.56 MiB | 17.22 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 107.56 k | — | — | 9,297.27 | 66.79 µs | 91.96 µs | 171.62 µs | 837.29 µs | 46.34 MiB | 17.21 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 149.04 k | — | — | 6,709.69 | 99.79 µs | 161.25 µs | 239.67 µs | 847.79 µs | 47.38 MiB | 23.85 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 8.71 M | — | — | 114.88 | 107.00 µs | 158.25 µs | 213.58 µs | 384.58 µs | 73.69 MiB | 1.39 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.41 M | — | — | 226.79 | 107.38 µs | 200.83 µs | 272.58 µs | 508.75 µs | 78.09 MiB | 705.49 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 244.28 k | — | — | 4,093.59 | 3.98 ms | 8.22 ms | 10.65 ms | 29.56 ms | 110.67 MiB | 39.09 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.59 M | — | — | 278.94 | 67.92 µs | 91.38 µs | 114.67 µs | 173.00 µs | 73.77 MiB | 573.61 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.52 M | — | — | 396.65 | 71.54 µs | 108.29 µs | 145.00 µs | 302.08 µs | 75.78 MiB | 403.38 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 284.25 k | — | — | 3,518.09 | 940.00 µs | 1.76 ms | 2.10 ms | 2.90 ms | 107.14 MiB | 45.48 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 901.75 k | — | — | 1,108.96 | 68.46 µs | 96.33 µs | 154.12 µs | 512.54 µs | 73.80 MiB | 144.28 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 745.37 k | — | — | 1,341.62 | 69.54 µs | 99.96 µs | 175.25 µs | 2.02 ms | 77.02 MiB | 119.26 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 264.59 k | — | — | 3,779.45 | 284.75 µs | 498.54 µs | 668.08 µs | 1.51 ms | 107.23 MiB | 42.33 M |

## Specialized diagnostics

Direction is metric-specific: throughput is higher-is-better; latency, CPU duty, and memory are lower-is-better. Candidates still require focused reproduction.

| Suite | Diagnostic | Metric | Direction | Median | Observed min–max | Δ metric | Interpretation |
| --- | --- | --- | --- | ---: | ---: | ---: | --- |
| copy-heavy | copy-heavy | compact_ms | lower | 744.68 ms | 706.78 ms–1,953.90 ms | — | — |
| high-reclaim | high-reclaim | compact_ms | lower | 176.00 ms | 162.81 ms–199.42 ms | — | — |
| low-reclaim | low-reclaim | compact_ms | lower | 604.82 ms | 537.09 ms–1,344.91 ms | — | — |
| medium-reclaim | medium-reclaim | compact_ms | lower | 394.11 ms | 367.02 ms–1,539.64 ms | — | — |
| no-gain | no-gain | compact_ms | lower | 121.68 ms | 114.31 ms–124.96 ms | — | — |
| ttl-50 | ttl-50 | compact_ms | lower | 438.92 ms | 396.18 ms–1,848.07 ms | — | — |
| generation-publication-adopt | direct_slot_protocol | publications_per_second | higher | 1.06 M | 1.00 M–1.16 M | — | — |
| generation-publication-adopt | shared_slot_protocol | publications_per_second | higher | 1.08 M | 1.03 M–1.15 M | — | — |
| generation-publication-get | direct_slot_protocol | publications_per_second | higher | 914.08 k | 853.10 k–970.24 k | — | — |
| generation-publication-get | shared_slot_protocol | publications_per_second | higher | 833.96 k | 798.47 k–881.15 k | — | — |
| generation-shell | direct_ring | ops_per_second | higher | 2.00 M | 655.21 k–2.35 M | — | — |
| generation-shell | direct_slot_pool | ops_per_second | higher | 1.80 M | 139.06 k–1.92 M | — | — |
| generation-shell | fixed_shell | ops_per_second | higher | 1.99 M | 1.61 M–2.44 M | — | — |
| generation-shell | inline_pool | ops_per_second | higher | 2.13 M | 913.85 k–2.46 M | — | — |
| generation-shell | make_shared | ops_per_second | higher | 1.97 M | 566.64 k–2.41 M | — | — |
| generation-shell | owning_pool | ops_per_second | higher | 1.88 M | 978.46 k–2.03 M | — | — |
| churn-background | background | foreground_ops_s | higher | 4.46 k | 960.18–6.10 k | — | — |
| churn-cooperative | cooperative | foreground_ops_s | higher | 4.21 k | 3.91 k–4.80 k | — | — |
| churn-disabled | disabled | foreground_ops_s | higher | 7.17 k | 3.99 k–7.92 k | — | — |
| forced-rotation-background | background | rotation_ms | lower | 164.42 ms | 58.59 ms–219.67 ms | — | — |
| forced-rotation-cooperative | cooperative | rotation_ms | lower | 143.58 ms | 135.26 ms–1,175.79 ms | — | — |
| forced-rotation-disabled | disabled | rotation_ms | lower | 112.28 ms | 104.37 ms–179.69 ms | — | — |
| idle-background | background | process_cpu_duty_pct | lower | 0.01 % | 0.01 %–0.01 % | — | — |
| idle-cooperative | cooperative | process_cpu_duty_pct | lower | 0.00 % | 0.00 %–0.00 % | — | — |
| idle-disabled | disabled | process_cpu_duty_pct | lower | 0.00 % | 0.00 %–0.00 % | — | — |
| mixed-background | background | foreground_ops_s | higher | 56.85 k | 45.72 k–93.43 k | — | — |
| mixed-cooperative | cooperative | foreground_ops_s | higher | 49.07 k | 25.49 k–85.47 k | — | — |
| mixed-disabled | disabled | foreground_ops_s | higher | 70.95 k | 55.37 k–83.71 k | — | — |
| paired-reactor-c1-p1-put0-v64 | current | ops_per_second | higher | 37.72 k | 37.58 k–37.96 k | — | — |
| paired-reactor-c1-p1-put0-v64 | paired | ops_per_second | higher | 38.17 k | 35.34 k–39.19 k | — | — |
| paired-reactor-c1-p1-put1-v64 | current | ops_per_second | higher | 34.15 k | 19.48 k–36.54 k | — | — |
| paired-reactor-c1-p1-put1-v64 | paired | ops_per_second | higher | 27.49 k | 21.39 k–33.88 k | — | — |
| paired-reactor-c1-p1-put10-v64 | current | ops_per_second | higher | 35.35 k | 34.54 k–35.54 k | — | — |
| paired-reactor-c1-p1-put10-v64 | paired | ops_per_second | higher | 34.85 k | 32.20 k–35.16 k | — | — |
| paired-reactor-c1-p1-put5-v64 | current | ops_per_second | higher | 35.15 k | 29.24 k–36.24 k | — | — |
| paired-reactor-c1-p1-put5-v64 | paired | ops_per_second | higher | 32.89 k | 31.42 k–35.55 k | — | — |
| paired-reactor-c1-p128-put0-v64 | current | ops_per_second | higher | 1.46 M | 1.38 M–1.46 M | — | — |
| paired-reactor-c1-p128-put0-v64 | paired | ops_per_second | higher | 1.63 M | 1.61 M–1.63 M | — | — |
| paired-reactor-c1-p128-put1-v64 | current | ops_per_second | higher | 1.18 M | 1.16 M–1.26 M | — | — |
| paired-reactor-c1-p128-put1-v64 | paired | ops_per_second | higher | 1.33 M | 1.19 M–1.34 M | — | — |
| paired-reactor-c1-p128-put10-v64 | current | ops_per_second | higher | 436.85 k | 385.51 k–457.47 k | — | — |
| paired-reactor-c1-p128-put10-v64 | paired | ops_per_second | higher | 346.31 k | 300.45 k–353.07 k | — | — |
| paired-reactor-c1-p128-put5-v64 | current | ops_per_second | higher | 657.86 k | 596.08 k–690.54 k | — | — |
| paired-reactor-c1-p128-put5-v64 | paired | ops_per_second | higher | 607.64 k | 534.23 k–621.35 k | — | — |
| paired-reactor-c1-p32-put0-v64 | current | ops_per_second | higher | 699.82 k | 643.50 k–702.76 k | — | — |
| paired-reactor-c1-p32-put0-v64 | paired | ops_per_second | higher | 723.71 k | 697.54 k–739.77 k | — | — |
| paired-reactor-c1-p32-put1-v64 | current | ops_per_second | higher | 572.23 k | 520.87 k–624.77 k | — | — |
| paired-reactor-c1-p32-put1-v64 | paired | ops_per_second | higher | 622.91 k | 85.99 k–632.89 k | — | — |
| paired-reactor-c1-p32-put10-v64 | current | ops_per_second | higher | 323.08 k | 277.55 k–338.24 k | — | — |
| paired-reactor-c1-p32-put10-v64 | paired | ops_per_second | higher | 278.75 k | 247.65 k–281.06 k | — | — |
| paired-reactor-c1-p32-put5-v64 | current | ops_per_second | higher | 420.76 k | 402.58 k–439.16 k | — | — |
| paired-reactor-c1-p32-put5-v64 | paired | ops_per_second | higher | 415.44 k | 391.50 k–423.92 k | — | — |
| paired-reactor-c1-p8-put0-v64 | current | ops_per_second | higher | 247.01 k | 242.19 k–247.91 k | — | — |
| paired-reactor-c1-p8-put0-v64 | paired | ops_per_second | higher | 252.35 k | 245.06 k–254.54 k | — | — |
| paired-reactor-c1-p8-put1-v64 | current | ops_per_second | higher | 233.26 k | 165.61 k–235.20 k | — | — |
| paired-reactor-c1-p8-put1-v64 | paired | ops_per_second | higher | 230.26 k | 165.56 k–237.74 k | — | — |
| paired-reactor-c1-p8-put10-v64 | current | ops_per_second | higher | 118.83 k | 110.75 k–120.67 k | — | — |
| paired-reactor-c1-p8-put10-v64 | paired | ops_per_second | higher | 118.35 k | 108.08 k–135.65 k | — | — |
| paired-reactor-c1-p8-put5-v64 | current | ops_per_second | higher | 195.34 k | 172.26 k–199.26 k | — | — |
| paired-reactor-c1-p8-put5-v64 | paired | ops_per_second | higher | 191.11 k | 177.67 k–192.98 k | — | — |
| paired-reactor-c4-p1-put0-v64 | current | ops_per_second | higher | 104.76 k | 92.23 k–107.43 k | — | — |
| paired-reactor-c4-p1-put0-v64 | paired | ops_per_second | higher | 103.20 k | 101.51 k–104.44 k | — | — |
| paired-reactor-c4-p1-put1-v64 | current | ops_per_second | higher | 105.92 k | 104.42 k–106.32 k | — | — |
| paired-reactor-c4-p1-put1-v64 | paired | ops_per_second | higher | 103.80 k | 101.09 k–104.46 k | — | — |
| paired-reactor-c4-p1-put10-v64 | current | ops_per_second | higher | 97.40 k | 97.16 k–98.48 k | — | — |
| paired-reactor-c4-p1-put10-v64 | paired | ops_per_second | higher | 96.33 k | 89.38 k–98.52 k | — | — |
| paired-reactor-c4-p1-put5-v64 | current | ops_per_second | higher | 83.25 k | 78.60 k–88.37 k | — | — |
| paired-reactor-c4-p1-put5-v64 | paired | ops_per_second | higher | 97.57 k | 81.75 k–99.77 k | — | — |
| paired-reactor-c4-p128-put0-v64 | current | ops_per_second | higher | 3.06 M | 2.95 M–3.13 M | — | — |
| paired-reactor-c4-p128-put0-v64 | paired | ops_per_second | higher | 3.56 M | 3.48 M–3.62 M | — | — |
| paired-reactor-c4-p128-put1-v64 | current | ops_per_second | higher | 2.55 M | 2.48 M–2.57 M | — | — |
| paired-reactor-c4-p128-put1-v64 | paired | ops_per_second | higher | 2.44 M | 2.38 M–2.45 M | — | — |
| paired-reactor-c4-p128-put10-v64 | current | ops_per_second | higher | 879.30 k | 858.54 k–958.13 k | — | — |
| paired-reactor-c4-p128-put10-v64 | paired | ops_per_second | higher | 560.55 k | 549.99 k–576.26 k | — | — |
| paired-reactor-c4-p128-put5-v64 | current | ops_per_second | higher | 1.43 M | 1.33 M–1.51 M | — | — |
| paired-reactor-c4-p128-put5-v64 | paired | ops_per_second | higher | 1.02 M | 965.08 k–1.09 M | — | — |
| paired-reactor-c4-p32-put0-v64 | current | ops_per_second | higher | 2.01 M | 1.92 M–2.03 M | — | — |
| paired-reactor-c4-p32-put0-v64 | paired | ops_per_second | higher | 2.09 M | 2.07 M–2.15 M | — | — |
| paired-reactor-c4-p32-put1-v64 | current | ops_per_second | higher | 1.64 M | 1.62 M–1.71 M | — | — |
| paired-reactor-c4-p32-put1-v64 | paired | ops_per_second | higher | 1.61 M | 1.55 M–1.63 M | — | — |
| paired-reactor-c4-p32-put10-v64 | current | ops_per_second | higher | 775.03 k | 744.71 k–800.44 k | — | — |
| paired-reactor-c4-p32-put10-v64 | paired | ops_per_second | higher | 550.41 k | 507.89 k–559.68 k | — | — |
| paired-reactor-c4-p32-put5-v64 | current | ops_per_second | higher | 1.08 M | 916.54 k–1.09 M | — | — |
| paired-reactor-c4-p32-put5-v64 | paired | ops_per_second | higher | 850.28 k | 695.65 k–908.95 k | — | — |
| paired-reactor-c4-p8-put0-v64 | current | ops_per_second | higher | 755.40 k | 707.48 k–761.98 k | — | — |
| paired-reactor-c4-p8-put0-v64 | paired | ops_per_second | higher | 739.88 k | 735.44 k–741.31 k | — | — |
| paired-reactor-c4-p8-put1-v64 | current | ops_per_second | higher | 689.80 k | 511.40 k–710.32 k | — | — |
| paired-reactor-c4-p8-put1-v64 | paired | ops_per_second | higher | 681.78 k | 554.11 k–689.90 k | — | — |
| paired-reactor-c4-p8-put10-v64 | current | ops_per_second | higher | 453.14 k | 448.48 k–456.74 k | — | — |
| paired-reactor-c4-p8-put10-v64 | paired | ops_per_second | higher | 381.64 k | 368.68 k–396.25 k | — | — |
| paired-reactor-c4-p8-put5-v64 | current | ops_per_second | higher | 551.69 k | 490.13 k–560.26 k | — | — |
| paired-reactor-c4-p8-put5-v64 | paired | ops_per_second | higher | 515.72 k | 512.45 k–522.14 k | — | — |
| paired-shard-v1024 | current-store-copy/get100 | ops_per_second | higher | 2.12 M | 2.05 M–2.13 M | — | — |
| paired-shard-v1024 | current-store-copy/get95-put5 | ops_per_second | higher | 1.73 M | 1.68 M–1.74 M | — | — |
| paired-shard-v1024 | paired-span/get100 | ops_per_second | higher | 13.22 M | 12.35 M–13.27 M | — | — |
| paired-shard-v1024 | paired-span-async/get95-put5 | ops_per_second | higher | 10.77 M | 10.71 M–10.82 M | — | — |
| paired-shard-v262144 | current-store-copy/get100 | ops_per_second | higher | 17.42 k | 17.35 k–17.44 k | — | — |
| paired-shard-v262144 | current-store-copy/get95-put5 | ops_per_second | higher | 17.00 k | 16.81 k–17.03 k | — | — |
| paired-shard-v262144 | paired-span/get100 | ops_per_second | higher | 14.08 M | 13.80 M–14.29 M | — | — |
| paired-shard-v262144 | paired-span-async/get95-put5 | ops_per_second | higher | 1.90 M | 1.08 M–2.00 M | — | — |
| paired-shard-v64 | current-store-copy/get100 | ops_per_second | higher | 5.88 M | 5.86 M–6.00 M | — | — |
| paired-shard-v64 | current-store-copy/get95-put5 | ops_per_second | higher | 3.72 M | 2.10 M–3.79 M | — | — |
| paired-shard-v64 | paired-span/get100 | ops_per_second | higher | 13.08 M | 12.96 M–13.23 M | — | — |
| paired-shard-v64 | paired-span-async/get95-put5 | ops_per_second | higher | 10.84 M | 9.85 M–10.93 M | — | — |
| paired-shard-v65536 | current-store-copy/get100 | ops_per_second | higher | 70.60 k | 70.28 k–70.68 k | — | — |
| paired-shard-v65536 | current-store-copy/get95-put5 | ops_per_second | higher | 67.26 k | 65.11 k–68.56 k | — | — |
| paired-shard-v65536 | paired-span/get100 | ops_per_second | higher | 14.40 M | 12.02 M–14.58 M | — | — |
| paired-shard-v65536 | paired-span-async/get95-put5 | ops_per_second | higher | 5.72 M | 3.75 M–6.46 M | — | — |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| copy-heavy | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| high-reclaim | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| low-reclaim | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| medium-reclaim | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| no-gain | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| ttl-50 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| index-all-k16-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v256 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v4096 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-get-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v256 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v4096 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-batch-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v256 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v4096 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-get-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v256 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v4096 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-put-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v256 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v4096 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| store-read-after-write-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p1-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p1-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p1-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p32-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p8-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p8-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c1-p8-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c4-p32-batch1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c4-p32-batch128 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c4-p32-batch16 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c4-p32-batch32 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w1-c4-p32-batch4 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w2-c2-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w2-c2-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w4-c4-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-group-w4-c4-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p1-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p1-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p1-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p32-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p8-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p8-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w1-c1-p8-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w2-c2-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w2-c2-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w4-c4-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-periodic-w4-c4-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p1-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p1-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p1-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p32-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p8-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p8-read-99-write-1 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w1-c1-p8-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w2-c2-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w2-c2-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w4-c4-p32-get-only | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| durable-sync-w4-c4-p32-read-after-write | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| embedded-durable-group-all | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| embedded-durable-group-parallel-put | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| embedded-durable-periodic-all | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| embedded-durable-sync-all | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| embedded-durable-sync-parallel-put | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| generation-publication-adopt | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| generation-publication-get | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| generation-shell | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| churn-background | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| churn-cooperative | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| churn-disabled | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| forced-rotation-background | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| forced-rotation-cooperative | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| forced-rotation-disabled | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| idle-background | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| idle-cooperative | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| idle-disabled | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| mixed-background | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| mixed-cooperative | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| mixed-disabled | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p1-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p1-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p1-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p1-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p128-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p128-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p128-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p128-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p32-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p32-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p32-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p32-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p8-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p8-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p8-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c1-p8-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p1-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p1-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p1-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p1-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p128-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p128-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p128-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p128-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p32-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p32-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p32-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p32-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p8-put0-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p8-put1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p8-put10-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-reactor-c4-p8-put5-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-shard-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-shard-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-shard-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| paired-shard-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w1-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w1-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w1-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w2-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w2-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w2-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w4-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w4-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w4-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w8-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w8-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| get-w8-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w1-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w1-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w1-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w2-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w2-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w2-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w4-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w4-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w4-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w8-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w8-uniform | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| put-w8-zipf | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| raw-w1-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| raw-w2-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| raw-w4-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| raw-w8-owner-bound | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| client-api-w1-c1-p32-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p1-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p1-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p1-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p128-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p128-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p128-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p32-get-only-v1024 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p32-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p32-get-only-v65536 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p32-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p32-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p8-get-only-v262144 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p8-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p8-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w1-c1-p8-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p1-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p1-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p1-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p128-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p128-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p128-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p32-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p32-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p32-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p8-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p8-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w2-c2-p8-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p1-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p1-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p1-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p128-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p128-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p128-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p32-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p32-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p32-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p8-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p8-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w4-c4-p8-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p1-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p1-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p1-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p128-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p128-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p128-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p32-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p32-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p32-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p8-get-only-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p8-read-99-write-1-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| volatile-w8-c8-p8-read-after-write-v64 | ebf0740 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.3.34.2) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-read-99-write-1 | 267.96 µs / 5.72 ms | 32 rec / 6656 B | 4.52 ms / 9.41 ms | 4.49 ms / 9.36 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 326.03 µs / 20.22 ms | 1 rec / 208 B | 4.50 ms / 11.66 ms | 4.40 ms / 11.50 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 285.93 µs / 8.40 ms | 32 rec / 6656 B | 4.74 ms / 12.21 ms | 4.66 ms / 11.47 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 674.62 µs / 26.55 ms | 1 rec / 208 B | 6.81 ms / 919.95 ms | 6.75 ms / 919.82 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 269.67 µs / 10.80 ms | 32 rec / 6656 B | 5.05 ms / 12.12 ms | 4.98 ms / 11.50 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 411.08 µs / 20.40 ms | 1 rec / 208 B | 5.20 ms / 24.23 ms | 5.10 ms / 23.93 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 13.53 ms / 627.18 ms | 3 rec / 624 B | 4.55 ms / 603.22 ms | 4.47 ms / 602.82 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 479.21 µs / 20.05 ms | 4 rec / 832 B | 4.75 ms / 729.09 ms | 4.66 ms / 728.34 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 422.87 µs / 20.04 ms | 4 rec / 832 B | 5.54 ms / 429.83 ms | 5.44 ms / 429.69 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 1.28 ms / 21.12 ms | 4 rec / 832 B | 5.09 ms / 21.72 ms | 4.99 ms / 21.61 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 50.27 µs / 9.96 ms | 4 rec / 832 B | 5.56 ms / 491.02 ms | 5.45 ms / 490.89 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 699.25 µs / 20.95 ms | 1 rec / 208 B | 12.67 ms / 634.48 ms | 12.60 ms / 634.33 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 326.30 µs / 2.68 ms | 1 rec / 208 B | 19.08 ms / 475.25 ms | 19.01 ms / 475.21 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 6.12 µs / 5.19 ms | 32 rec / 6656 B | 319.28 µs / 5.48 ms | 827.29 µs / 1.33 ms | 14.00 / 32.00 | 2 rec / 272 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 5.52 µs / 126.29 µs | 1 rec / 208 B | 114.87 µs / 6.42 ms | 934.12 µs / 6.39 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 5.18 µs / 10.09 ms | 32 rec / 6656 B | 944.86 µs / 22.85 ms | — / 6.31 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 8.92 µs / 1.80 ms | 1 rec / 208 B | 224.32 µs / 25.60 ms | 702.03 µs / 2.25 ms | 30.56 / 32.00 | 31 rec / 4216 B | 14/0/0/3 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 4.97 µs / 15.05 ms | 32 rec / 6656 B | 997.29 µs / 15.04 ms | — / 2.24 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 5.87 µs / 2.54 ms | 1 rec / 208 B | 120.86 µs / 8.60 ms | 887.28 µs / 6.84 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 13.76 µs / 2.30 ms | 1 rec / 208 B | 186.61 µs / 13.75 ms | 2.91 ms / 13.21 ms | 32.00 / 32.00 | 52 rec / 7072 B | 14/0/0/0 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 25.64 µs / 4.33 ms | 1 rec / 208 B | 238.60 µs / 45.91 ms | 5.52 ms / 42.21 ms | 30.38 / 32.00 | 116 rec / 15776 B | 12/0/0/1 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 11.28 µs / 156.00 ms | 32 rec / 6656 B | 4.61 ms / 155.98 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 23.78 µs / 9.85 ms | 1 rec / 208 B | 4.31 ms / 12.32 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 14.04 µs / 153.90 ms | 32 rec / 6656 B | 4.18 ms / 154.03 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 20.46 µs / 3.58 ms | 1 rec / 208 B | 4.71 ms / 17.36 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 9.54 µs / 131.09 ms | 32 rec / 6656 B | 4.00 ms / 140.19 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 17.01 µs / 9.97 ms | 1 rec / 208 B | 4.21 ms / 16.96 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 60.81 µs / 11.95 ms | 1 rec / 208 B | 9.72 ms / 29.75 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 63.69 µs / 11.67 ms | 1 rec / 208 B | 13.50 ms / 528.18 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
