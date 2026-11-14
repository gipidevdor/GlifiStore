# GlifiStore benchmark report

Generated at `2026-08-26T16:20:14+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

No retained baseline is available; throughput deltas are not shown.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 10.40 M | — | — | 96.19 | — | — | — | — | 41.05 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 11.46 M | — | — | 87.22 | — | — | — | — | 42.63 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 11.02 M | — | — | 90.76 | — | — | — | — | 42.63 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 13.85 M | — | — | 72.18 | — | — | — | — | 48.75 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.52 M | — | — | 60.55 | — | — | — | — | 66.56 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 13.17 M | — | — | 75.93 | — | — | — | — | 66.58 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.73 M | — | — | 578.71 | — | — | — | — | 601.16 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 4.26 M | — | — | 234.96 | — | — | — | — | 215.61 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 2.73 M | — | — | 366.49 | — | — | — | — | 312.98 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 17.23 k | — | — | 58,028.20 | — | — | — | — | 509.78 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 789.44 k | — | — | 1,266.72 | — | — | — | — | 1,777.41 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 4.05 M | — | — | 246.75 | — | — | — | — | 238.19 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 66.12 k | — | — | 15,124.60 | — | — | — | — | 511.44 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 557.82 k | — | — | 1,792.68 | — | — | — | — | 602.27 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 690.40 k | — | — | 1,448.44 | — | — | — | — | 211.36 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 632.36 k | — | — | 1,581.39 | — | — | — | — | 312.67 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 16.76 k | — | — | 59,648.50 | — | — | — | — | 509.16 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 389.66 k | — | — | 2,566.31 | — | — | — | — | 1,778.27 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 692.14 k | — | — | 1,444.80 | — | — | — | — | 234.03 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 64.73 k | — | — | 15,449.40 | — | — | — | — | 511.75 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 703.68 k | — | — | 1,421.11 | — | — | — | — | 601.52 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 879.99 k | — | — | 1,136.38 | — | — | — | — | 219.86 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 830.40 k | — | — | 1,204.24 | — | — | — | — | 302.91 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.97 k | — | — | 58,941.70 | — | — | — | — | 509.58 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 434.06 k | — | — | 2,303.82 | — | — | — | — | 1,776.41 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 882.64 k | — | — | 1,132.97 | — | — | — | — | 240.72 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 60.56 k | — | — | 16,511.60 | — | — | — | — | 511.36 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 446.82 k | — | — | 2,238.03 | — | — | — | — | 602.53 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 512.38 k | — | — | 1,951.70 | — | — | — | — | 213.34 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 501.43 k | — | — | 1,994.30 | — | — | — | — | 305.95 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 15.65 k | — | — | 63,886.00 | — | — | — | — | 509.23 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 299.43 k | — | — | 3,339.65 | — | — | — | — | 1,779.55 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 500.96 k | — | — | 1,996.16 | — | — | — | — | 231.69 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 61.16 k | — | — | 16,350.10 | — | — | — | — | 511.58 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 710.53 k | — | — | 1,407.40 | — | — | — | — | 604.16 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 929.95 k | — | — | 1,075.33 | — | — | — | — | 213.45 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 900.63 k | — | — | 1,110.33 | — | — | — | — | 309.09 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 15.40 k | — | — | 64,926.40 | — | — | — | — | 509.56 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 437.19 k | — | — | 2,287.31 | — | — | — | — | 1,776.97 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 907.05 k | — | — | 1,102.48 | — | — | — | — | 237.44 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 61.78 k | — | — | 16,187.60 | — | — | — | — | 511.91 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.75 k | — | — | 53,333.10 | 52.67 µs | 91.96 µs | 111.71 µs | 156.88 µs | 5.92 MiB | 3.00 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 8.93 k | — | — | 111,925.00 | 52.00 µs | 80.29 µs | 100.62 µs | 130.25 µs | 5.95 MiB | 1.43 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 336.72 | — | — | 2,969,810.00 | 5.24 ms | 10.60 ms | 18.44 ms | 54.11 ms | 5.73 MiB | 53.88 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 42.10 k | — | — | 23,750.30 | 419.58 µs | 833.46 µs | 1.28 ms | 1.65 ms | 6.22 MiB | 6.74 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 12.55 k | — | — | 79,654.70 | 488.54 µs | 6.87 ms | 11.36 ms | 11.57 ms | 6.00 MiB | 2.01 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 324.20 | — | — | 3,084,550.00 | 90.02 ms | 196.02 ms | 290.75 ms | 861.68 ms | 6.20 MiB | 51.87 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 27.23 k | — | — | 36,719.70 | 173.42 µs | 1.57 ms | 4.85 ms | 6.61 ms | 5.92 MiB | 4.36 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 11.93 k | — | — | 83,822.60 | 143.88 µs | 5.06 ms | 6.65 ms | 7.28 ms | 5.91 MiB | 1.91 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 357.13 | — | — | 2,800,110.00 | 24.80 ms | 46.57 ms | 56.27 ms | 78.93 ms | 5.97 MiB | 57.14 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 6.66 k | — | — | 150,055.00 | 17.21 ms | 35.15 ms | 41.22 ms | 46.94 ms | 6.52 MiB | 1.07 M |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.54 k | — | — | 647,442.00 | 83.59 ms | 162.94 ms | 177.96 ms | 193.02 ms | 6.63 MiB | 247.13 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.43 k | — | — | 697,504.00 | 93.41 ms | 205.08 ms | 416.93 ms | 486.19 ms | 6.63 MiB | 229.39 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.45 k | — | — | 688,992.00 | 87.07 ms | 178.41 ms | 248.68 ms | 717.55 ms | 6.53 MiB | 232.22 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 20.66 k | — | — | 48,407.70 | 6.68 ms | 22.21 ms | 71.45 ms | 89.30 ms | 6.52 MiB | 3.31 M |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 88.24 k | — | — | 11,332.90 | 399.46 µs | 834.17 µs | 1.03 ms | 1.19 ms | 8.50 MiB | 14.12 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 372.41 | — | — | 2,685,240.00 | 156.00 ms | 384.48 ms | 618.64 ms | 820.52 ms | 6.45 MiB | 59.58 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 99.64 k | — | — | 10,036.20 | 617.08 µs | 1.27 ms | 2.13 ms | 2.53 ms | 9.02 MiB | 15.94 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 505.26 | — | — | 1,979,190.00 | 247.00 ms | 472.96 ms | 536.04 ms | 582.01 ms | 9.11 MiB | 80.84 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 20.41 k | — | — | 49,001.80 | 49.46 µs | 73.54 µs | 91.42 µs | 132.38 µs | 5.84 MiB | 3.27 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 12.80 k | — | — | 78,100.60 | 53.62 µs | 73.54 µs | 103.88 µs | 140.58 µs | 5.87 MiB | 2.05 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 12.18 k | — | — | 82,111.50 | 67.83 µs | 222.62 µs | 4.34 ms | 5.80 ms | 5.91 MiB | 1.95 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 47.67 k | — | — | 20,976.70 | 328.75 µs | 660.62 µs | 810.79 µs | 929.67 µs | 6.17 MiB | 7.63 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 27.21 k | — | — | 36,746.50 | 357.25 µs | 4.62 ms | 8.04 ms | 8.30 ms | 5.97 MiB | 4.35 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 17.13 k | — | — | 58,382.20 | 772.12 µs | 6.55 ms | 8.64 ms | 9.69 ms | 6.16 MiB | 2.74 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 45.54 k | — | — | 21,958.30 | 113.71 µs | 223.00 µs | 313.58 µs | 374.25 µs | 5.91 MiB | 7.29 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 15.71 k | — | — | 63,666.50 | 132.08 µs | 355.67 µs | 4.42 ms | 19.22 ms | 5.92 MiB | 2.51 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 13.64 k | — | — | 73,314.20 | 233.12 µs | 4.72 ms | 6.52 ms | 21.42 ms | 4.92 MiB | 2.18 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 74.63 k | — | — | 13,399.80 | 430.96 µs | 864.58 µs | 1.07 ms | 1.43 ms | 8.31 MiB | 11.94 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 11.87 k | — | — | 84,234.50 | 5.44 ms | 13.12 ms | 21.46 ms | 24.22 ms | 8.42 MiB | 1.90 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 98.15 k | — | — | 10,188.60 | 679.46 µs | 1.57 ms | 1.89 ms | 2.19 ms | 12.80 MiB | 15.70 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 23.18 k | — | — | 43,143.50 | 1.47 ms | 14.59 ms | 26.18 ms | 34.84 ms | 12.78 MiB | 3.71 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 19.70 k | — | — | 50,758.50 | 49.79 µs | 199.54 µs | 703.92 µs | 2.10 ms | 5.84 MiB | 3.15 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 9.79 k | — | — | 102,105.00 | 46.71 µs | 85.29 µs | 111.67 µs | 153.08 µs | 5.80 MiB | 1.57 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 433.99 | — | — | 2,304,230.00 | 4.12 ms | 5.23 ms | 7.54 ms | 11.60 ms | 5.87 MiB | 69.44 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 38.80 k | — | — | 25,771.30 | 420.71 µs | 925.29 µs | 1.35 ms | 1.52 ms | 5.09 MiB | 6.21 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 9.24 k | — | — | 108,185.00 | 645.00 µs | 14.66 ms | 27.76 ms | 28.11 ms | 5.66 MiB | 1.48 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 402.08 | — | — | 2,487,070.00 | 80.07 ms | 154.11 ms | 165.06 ms | 178.37 ms | 6.14 MiB | 64.33 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 34.44 k | — | — | 29,038.90 | 133.50 µs | 297.92 µs | 393.71 µs | 490.08 µs | 5.80 MiB | 5.51 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 14.36 k | — | — | 69,659.40 | 128.96 µs | 3.06 ms | 5.58 ms | 5.90 ms | 5.87 MiB | 2.30 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 430.01 | — | — | 2,325,550.00 | 20.01 ms | 38.95 ms | 41.65 ms | 169.55 ms | 5.95 MiB | 68.80 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 67.92 k | — | — | 14,723.20 | 439.38 µs | 1.12 ms | 1.52 ms | 1.80 ms | 6.41 MiB | 10.87 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 361.39 | — | — | 2,767,070.00 | 170.69 ms | 390.59 ms | 1028.43 ms | 1167.73 ms | 6.50 MiB | 57.82 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 93.78 k | — | — | 10,662.80 | 703.50 µs | 1.63 ms | 1.91 ms | 2.04 ms | 12.83 MiB | 15.01 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 475.91 | — | — | 2,101,240.00 | 231.05 ms | 657.77 ms | 967.78 ms | 1042.66 ms | 8.95 MiB | 76.15 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 182.22 | — | — | 5,487,900.00 | — | — | — | — | 4.28 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 403.54 k | — | — | 2,478.08 | — | — | — | — | 4.64 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 382.31 | — | — | 2,615,710.00 | — | — | — | — | 4.64 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 361.74 | — | — | 2,764,450.00 | — | — | — | — | 4.70 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 365.40 | — | — | 2,736,710.00 | 10.04 ms | 22.09 ms | 30.91 ms | 251.18 ms | 4.70 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.66 k | — | — | 273,217.00 | — | — | — | — | 3.14 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 1.09 M | — | — | 921.17 | — | — | — | — | 3.41 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 6.39 k | — | — | 156,533.00 | — | — | — | — | 3.42 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 5.58 k | — | — | 179,371.00 | — | — | — | — | 3.52 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 176.64 | — | — | 5,661,270.00 | — | — | — | — | 3.03 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 342.77 k | — | — | 2,917.42 | — | — | — | — | 3.27 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 394.24 | — | — | 2,536,550.00 | — | — | — | — | 3.33 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 411.02 | — | — | 2,432,970.00 | — | — | — | — | 3.41 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 175.94 k | — | — | 5,683.58 | — | — | — | — | 3.61 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 271.56 | — | — | 3,682,470.00 | 13.01 ms | 19.02 ms | 24.97 ms | 88.06 ms | 3.53 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 3.04 M | — | — | 329.05 | — | — | — | — | 239.98 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 3.65 M | — | — | 274.30 | — | — | — | — | 233.69 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 3.88 M | — | — | 258.00 | — | — | — | — | 231.14 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 8.67 M | — | — | 115.37 | — | — | — | — | 166.42 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 5.84 M | — | — | 171.18 | — | — | — | — | 166.39 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.02 M | — | — | 166.13 | — | — | — | — | 171.20 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 17.36 M | — | — | 57.60 | — | — | — | — | 130.06 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 6.81 M | — | — | 146.91 | — | — | — | — | 138.97 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 8.37 M | — | — | 119.42 | — | — | — | — | 128.58 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 21.28 M | — | — | 46.99 | — | — | — | — | 134.56 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 11.95 M | — | — | 83.69 | — | — | — | — | 126.11 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.62 M | — | — | 94.21 | — | — | — | — | 133.14 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 578.25 k | — | — | 1,729.37 | — | — | — | — | 85.28 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 574.67 k | — | — | 1,740.13 | — | — | — | — | 81.98 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 583.75 k | — | — | 1,713.06 | — | — | — | — | 87.58 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 742.94 k | — | — | 1,346.01 | — | — | — | — | 72.95 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 499.12 k | — | — | 2,003.54 | — | — | — | — | 69.09 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 432.26 k | — | — | 2,313.44 | — | — | — | — | 73.75 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.14 M | — | — | 880.68 | — | — | — | — | 83.47 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 348.32 k | — | — | 2,870.96 | — | — | — | — | 72.53 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 480.76 k | — | — | 2,080.05 | — | — | — | — | 68.48 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 1.99 M | — | — | 501.80 | — | — | — | — | 64.33 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 505.09 k | — | — | 1,979.85 | — | — | — | — | 73.33 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 413.14 k | — | — | 2,420.48 | — | — | — | — | 68.52 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 1.06 M | — | — | 945.91 | — | — | — | — | 81.95 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.19 M | — | — | 841.10 | — | — | — | — | 68.33 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 2.24 M | — | — | 446.70 | — | — | — | — | 62.61 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.27 M | — | — | 305.95 | — | — | — | — | 66.92 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 225.36 k | — | — | 4,437.40 | 254.71 µs | 887.75 µs | 1.86 ms | 7.26 ms | 34.50 MiB | 36.06 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 46.08 k | — | — | 21,701.60 | 21.79 µs | 24.71 µs | 37.00 µs | 91.12 µs | 31.56 MiB | 7.37 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.62 k | — | — | 22,409.00 | 22.00 µs | 38.04 µs | 67.04 µs | 193.54 µs | 32.31 MiB | 7.14 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 46.72 k | — | — | 21,403.00 | 40.67 µs | 57.79 µs | 99.42 µs | 258.67 µs | 36.58 MiB | 7.48 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.82 M | — | — | 549.95 | 65.25 µs | 76.33 µs | 81.92 µs | 94.54 µs | 101.88 MiB | 290.93 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.19 M | — | — | 839.41 | 89.12 µs | 154.17 µs | 227.75 µs | 1.50 ms | 107.38 MiB | 190.61 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 202.90 k | — | — | 4,928.62 | 485.79 µs | 2.14 ms | 4.38 ms | 9.14 ms | 128.66 MiB | 32.46 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 377.01 k | — | — | 2,652.44 | 66.21 µs | 127.00 µs | 233.62 µs | 1.00 ms | 152.36 MiB | 422.25 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 887.06 k | — | — | 1,127.32 | 34.67 µs | 38.33 µs | 42.54 µs | 66.33 µs | 100.34 MiB | 141.93 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 11.95 k | — | — | 83,704.40 | 1.82 ms | 3.17 ms | 5.42 ms | 38.58 ms | 420.58 MiB | 784.09 M |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 737.77 k | — | — | 1,355.44 | 36.46 µs | 58.54 µs | 78.21 µs | 157.42 µs | 106.84 MiB | 118.04 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 331.91 k | — | — | 3,012.87 | 117.75 µs | 307.25 µs | 867.29 µs | 2.96 ms | 126.17 MiB | 53.11 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 3.24 k | — | — | 308,312.00 | 1.50 ms | 2.29 ms | 2.35 ms | 2.47 ms | 483.55 MiB | 850.57 M |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 286.34 k | — | — | 3,492.40 | 25.88 µs | 39.29 µs | 64.83 µs | 156.62 µs | 99.33 MiB | 45.81 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 273.69 k | — | — | 3,653.83 | 26.25 µs | 49.67 µs | 91.42 µs | 306.50 µs | 102.28 MiB | 43.79 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 168.37 k | — | — | 5,939.35 | 70.42 µs | 152.12 µs | 268.33 µs | 906.38 µs | 128.47 MiB | 26.94 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 75.97 k | — | — | 13,163.70 | 25.04 µs | 47.00 µs | 62.21 µs | 89.79 µs | 32.08 MiB | 12.15 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 79.87 k | — | — | 12,520.90 | 24.79 µs | 52.42 µs | 89.54 µs | 192.25 µs | 29.48 MiB | 12.78 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 83.95 k | — | — | 11,911.30 | 46.67 µs | 86.21 µs | 150.96 µs | 657.21 µs | 40.75 MiB | 13.43 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.47 M | — | — | 288.44 | 69.42 µs | 177.62 µs | 284.54 µs | 797.88 µs | 81.48 MiB | 554.71 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 1.67 M | — | — | 600.30 | 89.38 µs | 174.62 µs | 233.83 µs | 2.62 ms | 85.48 MiB | 266.53 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 304.62 k | — | — | 3,282.77 | 771.00 µs | 2.19 ms | 4.38 ms | 10.58 ms | 113.36 MiB | 48.74 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.61 M | — | — | 622.61 | 38.33 µs | 70.17 µs | 96.58 µs | 162.62 µs | 80.05 MiB | 256.99 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.28 M | — | — | 779.76 | 40.04 µs | 79.54 µs | 126.58 µs | 279.79 µs | 85.95 MiB | 205.19 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 353.99 k | — | — | 2,824.97 | 213.08 µs | 527.38 µs | 890.71 µs | 2.63 ms | 113.97 MiB | 56.64 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 592.68 k | — | — | 1,687.25 | 27.17 µs | 62.25 µs | 108.25 µs | 320.71 µs | 77.66 MiB | 94.83 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 531.03 k | — | — | 1,883.12 | 26.79 µs | 42.46 µs | 57.17 µs | 82.42 µs | 82.78 MiB | 84.97 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 200.23 k | — | — | 4,994.27 | 98.67 µs | 243.04 µs | 426.62 µs | 1.30 ms | 116.94 MiB | 32.04 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 102.45 k | — | — | 9,760.79 | 36.25 µs | 73.75 µs | 169.21 µs | 700.79 µs | 33.20 MiB | 16.39 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 113.53 k | — | — | 8,808.41 | 33.62 µs | 62.21 µs | 128.42 µs | 631.46 µs | 33.92 MiB | 18.16 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 102.88 k | — | — | 9,719.87 | 75.46 µs | 112.71 µs | 200.33 µs | 494.08 µs | 42.66 MiB | 16.46 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.61 M | — | — | 178.25 | 84.79 µs | 132.42 µs | 189.96 µs | 257.08 µs | 80.09 MiB | 897.61 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 2.77 M | — | — | 360.69 | 104.42 µs | 222.92 µs | 323.62 µs | 738.71 µs | 84.80 MiB | 443.60 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 337.14 k | — | — | 2,966.14 | 1.46 ms | 3.76 ms | 6.96 ms | 36.72 ms | 115.50 MiB | 53.94 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.62 M | — | — | 381.94 | 46.17 µs | 79.67 µs | 98.46 µs | 171.38 µs | 78.25 MiB | 418.91 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.72 M | — | — | 582.77 | 53.58 µs | 119.46 µs | 182.83 µs | 289.96 µs | 80.45 MiB | 274.55 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 289.52 k | — | — | 3,453.97 | 402.71 µs | 1.00 ms | 2.35 ms | 6.44 ms | 114.06 MiB | 46.32 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 673.65 k | — | — | 1,484.44 | 45.38 µs | 127.42 µs | 360.83 µs | 1.71 ms | 79.23 MiB | 107.78 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 721.53 k | — | — | 1,385.94 | 38.92 µs | 101.62 µs | 164.92 µs | 378.62 µs | 73.91 MiB | 115.44 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 226.18 k | — | — | 4,421.21 | 163.83 µs | 326.12 µs | 692.71 µs | 1.40 ms | 119.59 MiB | 36.19 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 133.66 k | — | — | 7,481.81 | 59.96 µs | 80.42 µs | 96.54 µs | 168.12 µs | 43.34 MiB | 21.39 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 130.20 k | — | — | 7,680.30 | 60.04 µs | 82.79 µs | 156.46 µs | 262.96 µs | 42.27 MiB | 20.83 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 160.05 k | — | — | 6,247.90 | 92.54 µs | 158.33 µs | 513.38 µs | 2.76 ms | 49.14 MiB | 25.61 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 9.24 M | — | — | 108.20 | 102.75 µs | 155.79 µs | 628.88 µs | 900.12 µs | 85.78 MiB | 1.48 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.89 M | — | — | 204.47 | 109.58 µs | 201.42 µs | 283.25 µs | 564.46 µs | 92.33 MiB | 782.50 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 402.17 k | — | — | 2,486.50 | 2.04 ms | 7.20 ms | 19.80 ms | 98.20 ms | 124.03 MiB | 64.35 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.66 M | — | — | 273.10 | 67.42 µs | 88.71 µs | 103.04 µs | 138.75 µs | 87.03 MiB | 585.87 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.57 M | — | — | 389.70 | 76.38 µs | 372.50 µs | 782.00 µs | 2.20 ms | 90.64 MiB | 410.57 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 371.82 k | — | — | 2,689.46 | 542.08 µs | 1.96 ms | 4.83 ms | 27.99 ms | 117.42 MiB | 59.49 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 1.00 M | — | — | 996.78 | 63.29 µs | 84.75 µs | 248.54 µs | 1.95 ms | 86.91 MiB | 160.52 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 925.62 k | — | — | 1,080.36 | 64.67 µs | 123.46 µs | 217.42 µs | 351.33 µs | 86.20 MiB | 148.10 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 322.57 k | — | — | 3,100.08 | 223.54 µs | 519.17 µs | 1.20 ms | 4.66 ms | 119.28 MiB | 51.61 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| index-all-k16-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | f999cac-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-get-only | 54.83 µs / 6.17 ms | 29 rec / 6656 B | 1.39 ms / 13.65 ms | 1.59 ms / 13.51 ms | 30.00 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-99-write-1 | 42.23 µs / 6.99 ms | 31 rec / 6656 B | 1.49 ms / 12.60 ms | 3.03 ms / 12.51 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 674.57 µs / 14.29 ms | 1 rec / 208 B | 5.23 ms / 32.68 ms | 5.18 ms / 32.62 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-get-only | 211.38 µs / 8.66 ms | 26 rec / 6656 B | 2.65 ms / 22.47 ms | 2.98 ms / 22.35 ms | 26.32 / 32.00 | 0 rec / 0 B | 12/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 54.94 µs / 917.79 µs | 32 rec / 6656 B | 1.40 ms / 12.33 ms | 2.20 ms / 12.21 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 303.54 µs / 3.45 ms | 1 rec / 208 B | 5.75 ms / 439.33 ms | 5.71 ms / 439.24 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-get-only | 51.08 µs / 4.44 ms | 32 rec / 6656 B | 1.30 ms / 15.71 ms | 1.62 ms / 15.64 ms | 30.00 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 54.70 µs / 7.22 ms | 21 rec / 6656 B | 1.28 ms / 16.41 ms | 2.78 ms / 16.26 ms | 23.31 / 32.00 | 0 rec / 0 B | 9/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 317.52 µs / 4.31 ms | 1 rec / 208 B | 5.15 ms / 16.51 ms | 5.09 ms / 16.45 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 786.63 µs / 8.73 ms | 3 rec / 624 B | 298.14 µs / 8.26 ms | 288.94 µs / 8.25 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 276.53 µs / 387.04 µs | 4 rec / 832 B | 4.77 ms / 22.61 ms | 4.67 ms / 22.52 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 281.13 µs / 790.38 µs | 4 rec / 832 B | 5.19 ms / 170.77 ms | 5.12 ms / 170.62 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 283.13 µs / 1.33 ms | 4 rec / 832 B | 5.10 ms / 434.68 ms | 5.02 ms / 434.61 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 17.01 µs / 106.75 µs | 4 rec / 832 B | 335.15 µs / 20.46 ms | 296.28 µs / 20.43 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-get-only | 64.48 µs / 10.11 ms | 32 rec / 6656 B | 1.95 ms / 580.80 ms | 1.79 ms / 580.44 ms | 31.25 / 32.00 | 0 rec / 0 B | 14/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 319.89 µs / 1.66 ms | 1 rec / 208 B | 10.35 ms / 388.50 ms | 10.32 ms / 388.46 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-get-only | 82.82 µs / 12.08 ms | 32 rec / 6656 B | 2.59 ms / 11.74 ms | 2.37 ms / 11.25 ms | 31.25 / 32.00 | 0 rec / 0 B | 12/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 312.53 µs / 783.12 µs | 1 rec / 208 B | 15.39 ms / 43.22 ms | 15.36 ms / 43.19 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-get-only | 781.41 µs / 20.84 ms | 32 rec / 6656 B | 1.07 ms / 20.82 ms | 766.69 µs / 4.37 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 523.02 µs / 7.94 ms | 30 rec / 6656 B | 1.03 ms / 8.45 ms | 1.13 ms / 7.12 ms | 30.20 / 32.00 | 1 rec / 136 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 4.80 µs / 20.96 µs | 1 rec / 208 B | 110.25 µs / 7.49 ms | 804.14 µs / 7.47 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-get-only | 995.48 µs / 14.59 ms | 27 rec / 6656 B | 1.64 ms / 14.58 ms | 599.19 µs / 5.42 ms | 32.00 / 32.00 | 20 rec / 2720 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 274.89 µs / 8.34 ms | 28 rec / 6656 B | 827.77 µs / 8.89 ms | 693.60 µs / 1.37 ms | 32.00 / 32.00 | 15 rec / 2040 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 5.42 µs / 32.38 µs | 1 rec / 208 B | 87.28 µs / 8.24 ms | 658.11 µs / 6.88 ms | 32.00 / 32.00 | 20 rec / 2720 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-get-only | 771.05 µs / 8.70 ms | 32 rec / 6656 B | 954.77 µs / 8.67 ms | 648.25 µs / 6.91 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 170.92 µs / 1.47 ms | 32 rec / 6656 B | 452.46 µs / 18.89 ms | 449.03 µs / 677.04 µs | 30.20 / 32.00 | 15 rec / 2040 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 5.84 µs / 110.25 µs | 1 rec / 208 B | 98.14 µs / 20.97 ms | 731.34 µs / 1.87 ms | 32.00 / 32.00 | 12 rec / 1632 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w2-c2-p32-get-only | 983.53 µs / 29.42 ms | 27 rec / 6656 B | 1.53 ms / 29.40 ms | 1.41 ms / 28.64 ms | 31.60 / 32.00 | 52 rec / 7072 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 6.94 µs / 40.58 µs | 1 rec / 208 B | 285.22 µs / 14.27 ms | 4.34 ms / 13.20 ms | 32.00 / 32.00 | 52 rec / 7072 B | 14/0/0/0 | 0/0/0 |
| durable-periodic-w4-c4-p32-get-only | 958.50 µs / 9.87 ms | 32 rec / 6656 B | 2.00 ms / 21.91 ms | 2.74 ms / 21.80 ms | 31.40 / 32.00 | 116 rec / 15776 B | 12/0/0/3 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 9.97 µs / 59.38 µs | 1 rec / 208 B | 216.99 µs / 31.93 ms | 4.92 ms / 31.07 ms | 32.00 / 32.00 | 117 rec / 15912 B | 12/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-get-only | 33.64 ms / 164.00 ms | 32 rec / 6656 B | 98.23 ms / 163.95 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 41.57 ms / 130.01 ms | 32 rec / 6656 B | 85.53 ms / 136.22 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 9.32 µs / 589.04 µs | 1 rec / 208 B | 4.45 ms / 11.44 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-get-only | 46.06 ms / 146.82 ms | 27 rec / 6656 B | 90.29 ms / 146.81 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 54.03 ms / 269.54 ms | 32 rec / 6656 B | 160.31 ms / 314.14 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 10.76 µs / 28.42 µs | 1 rec / 208 B | 4.90 ms / 18.10 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-get-only | 37.52 ms / 130.03 ms | 31 rec / 6656 B | 77.61 ms / 129.98 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 40.33 ms / 129.66 ms | 32 rec / 6656 B | 85.25 ms / 185.78 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 11.08 µs / 4.16 ms | 1 rec / 208 B | 4.53 ms / 41.54 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-get-only | 47.21 ms / 215.30 ms | 32 rec / 6656 B | 96.07 ms / 215.27 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 11.35 µs / 163.21 µs | 1 rec / 208 B | 10.97 ms / 456.07 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-get-only | 45.32 ms / 568.68 ms | 31 rec / 6656 B | 97.23 ms / 568.64 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 13.53 µs / 200.79 µs | 1 rec / 208 B | 16.62 ms / 452.60 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
