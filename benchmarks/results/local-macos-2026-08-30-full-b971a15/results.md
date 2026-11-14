# GlifiStore benchmark report

Generated at `2026-08-30T11:18:14+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

Baseline report: `2026-08-29T13:29:17+00:00`.

Environment identity: **compatible**; throughput deltas are shown.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 11.94 M | -7.80% | regression candidate | 83.78 | — | — | — | — | 40.73 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 7.73 M | -17.76% | inconclusive (ranges overlap) | 129.34 | — | — | — | — | 41.06 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 12.77 M | +19.61% | inconclusive (ranges overlap) | 78.31 | — | — | — | — | 41.06 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 14.08 M | +2.53% | inconclusive (ranges overlap) | 71.04 | — | — | — | — | 47.22 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.50 M | -1.52% | inconclusive (ranges overlap) | 60.62 | — | — | — | — | 63.50 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 9.66 M | -17.26% | regression candidate | 103.48 | — | — | — | — | 63.52 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.95 M | +2.00% | inconclusive (ranges overlap) | 513.93 | — | — | — | — | 479.00 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 4.54 M | +25.81% | inconclusive (ranges overlap) | 220.19 | — | — | — | — | 92.39 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 3.13 M | +13.96% | inconclusive (ranges overlap) | 319.59 | — | — | — | — | 184.11 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.71 k | +6.88% | inconclusive (ranges overlap) | 59,857.80 | — | — | — | — | 504.36 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 811.14 k | +8.89% | improvement candidate | 1,232.84 | — | — | — | — | 1,651.74 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 4.25 M | +30.16% | improvement candidate | 235.47 | — | — | — | — | 110.70 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 67.54 k | +6.15% | inconclusive (ranges overlap) | 14,806.60 | — | — | — | — | 505.97 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 553.34 k | -2.28% | inconclusive (ranges overlap) | 1,807.21 | — | — | — | — | 479.92 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 681.90 k | +4.05% | inconclusive (ranges overlap) | 1,466.50 | — | — | — | — | 94.66 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 673.13 k | +6.28% | inconclusive (ranges overlap) | 1,485.61 | — | — | — | — | 186.52 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 14.75 k | -3.01% | inconclusive (ranges overlap) | 67,790.20 | — | — | — | — | 504.23 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 387.55 k | +4.92% | inconclusive (ranges overlap) | 2,580.31 | — | — | — | — | 1,654.69 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 673.51 k | +11.00% | improvement candidate | 1,484.76 | — | — | — | — | 111.95 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 65.43 k | +21.82% | inconclusive (ranges overlap) | 15,282.40 | — | — | — | — | 506.55 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 620.68 k | +2.17% | inconclusive (ranges overlap) | 1,611.14 | — | — | — | — | 478.98 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 783.63 k | +11.49% | improvement candidate | 1,276.12 | — | — | — | — | 94.06 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 753.68 k | +11.99% | improvement candidate | 1,326.82 | — | — | — | — | 184.70 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.22 k | +3.47% | inconclusive (ranges overlap) | 65,682.70 | — | — | — | — | 504.39 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 407.47 k | +4.20% | improvement candidate | 2,454.19 | — | — | — | — | 1,655.61 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 814.22 k | +14.07% | improvement candidate | 1,228.18 | — | — | — | — | 110.77 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 60.16 k | +11.22% | inconclusive (ranges overlap) | 16,621.70 | — | — | — | — | 506.02 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 373.77 k | -1.09% | inconclusive (ranges overlap) | 2,675.43 | — | — | — | — | 477.77 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 421.85 k | +5.67% | improvement candidate | 2,370.52 | — | — | — | — | 91.69 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 427.20 k | +8.21% | improvement candidate | 2,340.80 | — | — | — | — | 185.33 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 13.36 k | -3.28% | inconclusive (ranges overlap) | 74,847.80 | — | — | — | — | 504.08 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 271.32 k | +2.48% | inconclusive (ranges overlap) | 3,685.66 | — | — | — | — | 1,651.56 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 427.84 k | +6.26% | inconclusive (ranges overlap) | 2,337.31 | — | — | — | — | 110.70 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 54.30 k | +14.43% | inconclusive (ranges overlap) | 18,415.70 | — | — | — | — | 505.89 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 653.69 k | +5.45% | inconclusive (ranges overlap) | 1,529.79 | — | — | — | — | 478.08 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 824.39 k | +6.91% | inconclusive (ranges overlap) | 1,213.02 | — | — | — | — | 91.77 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 780.99 k | +3.10% | inconclusive (ranges overlap) | 1,280.43 | — | — | — | — | 184.22 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 15.22 k | +3.20% | inconclusive (ranges overlap) | 65,712.20 | — | — | — | — | 504.39 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 412.12 k | +4.56% | inconclusive (ranges overlap) | 2,426.49 | — | — | — | — | 1,654.19 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 822.39 k | +6.56% | inconclusive (ranges overlap) | 1,215.96 | — | — | — | — | 111.95 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 62.38 k | +12.65% | inconclusive (ranges overlap) | 16,031.50 | — | — | — | — | 506.47 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 16.88 k | +11.37% | inconclusive (ranges overlap) | 59,228.90 | 55.38 µs | 80.38 µs | 97.29 µs | 139.12 µs | 5.03 MiB | 2.70 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 8.88 k | +3.40% | inconclusive (ranges overlap) | 112,635.00 | 51.75 µs | 83.83 µs | 101.79 µs | 130.21 µs | 5.06 MiB | 1.42 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 358.03 | -8.53% | regression candidate | 2,793,090.00 | 5.10 ms | 6.94 ms | 11.63 ms | 16.07 ms | 6.00 MiB | 57.28 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 29.47 k | -30.49% | inconclusive (ranges overlap) | 33,937.40 | 482.79 µs | 1.40 ms | 1.93 ms | 3.74 ms | 6.33 MiB | 4.71 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 12.17 k | -12.36% | inconclusive (ranges overlap) | 82,193.60 | 561.29 µs | 6.58 ms | 7.09 ms | 8.49 ms | 6.14 MiB | 1.95 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 381.32 | -2.04% | inconclusive (ranges overlap) | 2,622,460.00 | 90.87 ms | 202.03 ms | 442.46 ms | 720.73 ms | 6.31 MiB | 61.01 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 41.61 k | -9.35% | inconclusive (ranges overlap) | 24,030.40 | 126.04 µs | 251.00 µs | 396.08 µs | 613.79 µs | 6.08 MiB | 6.66 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 10.97 k | -11.81% | inconclusive (ranges overlap) | 91,128.80 | 127.62 µs | 5.05 ms | 7.86 ms | 9.20 ms | 6.14 MiB | 1.76 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 332.38 | -19.06% | regression candidate | 3,008,650.00 | 25.04 ms | 49.67 ms | 67.33 ms | 111.92 ms | 6.11 MiB | 53.18 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 343.83 | -8.51% | inconclusive (ranges overlap) | 2,908,430.00 | 339.10 ms | 840.34 ms | 1342.39 ms | 1596.20 ms | 5.64 MiB | 55.01 k |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.39 k | -6.56% | inconclusive (ranges overlap) | 720,223.00 | 91.13 ms | 185.93 ms | 219.52 ms | 748.00 ms | 6.77 MiB | 222.15 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.09 k | -22.97% | inconclusive (ranges overlap) | 917,844.00 | 107.41 ms | 258.48 ms | 622.00 ms | 853.26 ms | 6.69 MiB | 174.32 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.39 k | -6.45% | inconclusive (ranges overlap) | 720,755.00 | 88.66 ms | 176.86 ms | 196.56 ms | 223.51 ms | 6.63 MiB | 221.99 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.41 k | -12.68% | inconclusive (ranges overlap) | 710,773.00 | 96.80 ms | 228.10 ms | 823.86 ms | 1035.59 ms | 6.64 MiB | 225.11 k |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 84.78 k | +12.92% | inconclusive (ranges overlap) | 11,794.90 | 437.04 µs | 875.04 µs | 1.11 ms | 1.32 ms | 8.58 MiB | 13.57 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 367.14 | +22.21% | inconclusive (ranges overlap) | 2,723,730.00 | 172.03 ms | 357.65 ms | 822.26 ms | 927.10 ms | 8.59 MiB | 58.74 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 92.24 k | -3.60% | inconclusive (ranges overlap) | 10,841.20 | 668.54 µs | 1.73 ms | 2.04 ms | 2.20 ms | 13.06 MiB | 14.76 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 481.49 | +37.85% | inconclusive (ranges overlap) | 2,076,900.00 | 258.68 ms | 612.25 ms | 1117.00 ms | 1199.05 ms | 13.09 MiB | 77.04 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.30 k | -17.05% | inconclusive (ranges overlap) | 65,364.90 | 54.75 µs | 89.29 µs | 206.17 µs | 391.62 µs | 6.05 MiB | 2.45 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 15.23 k | -9.93% | inconclusive (ranges overlap) | 65,649.70 | 51.58 µs | 74.58 µs | 110.67 µs | 223.88 µs | 6.00 MiB | 2.44 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.25 k | -4.67% | inconclusive (ranges overlap) | 97,568.90 | 73.75 µs | 537.50 µs | 4.85 ms | 9.40 ms | 5.98 MiB | 1.64 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 45.84 k | -7.67% | inconclusive (ranges overlap) | 21,815.60 | 367.71 µs | 753.25 µs | 881.38 µs | 1.18 ms | 6.30 MiB | 7.33 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 27.74 k | -11.71% | inconclusive (ranges overlap) | 36,053.90 | 390.54 µs | 1.13 ms | 5.95 ms | 6.33 ms | 6.08 MiB | 4.44 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 7.56 k | -41.47% | inconclusive (ranges overlap) | 132,264.00 | 1.43 ms | 15.06 ms | 32.11 ms | 101.17 ms | 6.27 MiB | 1.21 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 39.04 k | -0.85% | inconclusive (ranges overlap) | 25,617.50 | 124.54 µs | 247.50 µs | 350.42 µs | 501.00 µs | 6.08 MiB | 6.25 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 21.42 k | -26.54% | inconclusive (ranges overlap) | 46,676.70 | 123.54 µs | 269.96 µs | 6.03 ms | 9.49 ms | 6.11 MiB | 3.43 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 8.97 k | -41.07% | inconclusive (ranges overlap) | 111,495.00 | 259.12 µs | 6.44 ms | 9.63 ms | 16.83 ms | 6.08 MiB | 1.44 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 61.32 k | -25.53% | inconclusive (ranges overlap) | 16,307.20 | 503.17 µs | 1.84 ms | 9.14 ms | 10.89 ms | 8.47 MiB | 9.81 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 9.74 k | -11.29% | inconclusive (ranges overlap) | 102,648.00 | 7.15 ms | 31.90 ms | 48.45 ms | 56.16 ms | 6.63 MiB | 1.56 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 110.66 k | +28.58% | improvement candidate | 9,036.33 | 565.00 µs | 1.06 ms | 1.18 ms | 1.27 ms | 13.02 MiB | 17.71 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 20.64 k | -35.38% | inconclusive (ranges overlap) | 48,445.60 | 1.62 ms | 16.25 ms | 23.13 ms | 34.72 ms | 9.22 MiB | 3.30 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.61 k | +36.36% | inconclusive (ranges overlap) | 53,720.80 | 46.88 µs | 106.71 µs | 239.62 µs | 768.54 µs | 5.05 MiB | 2.98 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.50 k | +3.27% | inconclusive (ranges overlap) | 95,214.60 | 46.33 µs | 83.17 µs | 112.21 µs | 172.25 µs | 5.92 MiB | 1.68 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 351.05 | -19.44% | inconclusive (ranges overlap) | 2,848,610.00 | 4.77 ms | 14.04 ms | 19.56 ms | 53.64 ms | 6.00 MiB | 56.17 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 40.05 k | -9.60% | inconclusive (ranges overlap) | 24,966.10 | 403.33 µs | 841.12 µs | 1.28 ms | 1.58 ms | 6.31 MiB | 6.41 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 11.79 k | -22.92% | regression candidate | 84,801.00 | 672.75 µs | 6.32 ms | 6.97 ms | 7.40 ms | 6.03 MiB | 1.89 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 378.14 | -9.78% | inconclusive (ranges overlap) | 2,644,510.00 | 85.99 ms | 173.13 ms | 271.44 ms | 350.09 ms | 6.34 MiB | 60.50 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 32.38 k | -15.08% | inconclusive (ranges overlap) | 30,880.10 | 131.04 µs | 296.54 µs | 415.08 µs | 611.71 µs | 6.02 MiB | 5.18 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.88 k | -4.54% | inconclusive (ranges overlap) | 77,641.20 | 162.25 µs | 3.89 ms | 5.19 ms | 5.45 ms | 6.02 MiB | 2.06 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 417.39 | -0.78% | inconclusive (ranges overlap) | 2,395,840.00 | 20.10 ms | 39.10 ms | 43.95 ms | 47.12 ms | 5.06 MiB | 66.78 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 64.17 k | -7.46% | inconclusive (ranges overlap) | 15,583.40 | 456.83 µs | 1.17 ms | 1.55 ms | 1.94 ms | 6.55 MiB | 10.27 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 382.73 | -6.59% | inconclusive (ranges overlap) | 2,612,840.00 | 158.09 ms | 379.16 ms | 857.35 ms | 966.26 ms | 6.63 MiB | 61.24 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 82.21 k | -22.66% | inconclusive (ranges overlap) | 12,163.90 | 763.04 µs | 1.77 ms | 2.14 ms | 2.31 ms | 9.11 MiB | 13.15 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 549.97 | -8.29% | inconclusive (ranges overlap) | 1,818,270.00 | 221.56 ms | 474.70 ms | 874.79 ms | 1061.34 ms | 9.19 MiB | 88.00 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 175.71 | +6.94% | inconclusive (ranges overlap) | 5,691,180.00 | — | — | — | — | 4.13 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 400.24 k | -41.08% | regression candidate | 2,498.50 | — | — | — | — | 4.56 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 346.33 | -11.24% | inconclusive (ranges overlap) | 2,887,390.00 | — | — | — | — | 4.61 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 361.00 | -8.21% | inconclusive (ranges overlap) | 2,770,080.00 | — | — | — | — | 4.67 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 399.99 | +2.95% | inconclusive (ranges overlap) | 2,500,080.00 | 10.00 ms | 13.61 ms | 19.18 ms | 147.87 ms | 4.64 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.51 k | +9.60% | inconclusive (ranges overlap) | 284,556.00 | — | — | — | — | 3.06 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 1.07 M | +16.59% | inconclusive (ranges overlap) | 930.42 | — | — | — | — | 3.50 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 6.85 k | +7.67% | inconclusive (ranges overlap) | 145,937.00 | — | — | — | — | 3.61 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 7.48 k | +12.86% | inconclusive (ranges overlap) | 133,777.00 | — | — | — | — | 3.67 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 119.18 | -34.29% | inconclusive (ranges overlap) | 8,390,580.00 | — | — | — | — | 3.06 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 347.79 k | -43.85% | inconclusive (ranges overlap) | 2,875.33 | — | — | — | — | 3.28 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 377.13 | -1.27% | inconclusive (ranges overlap) | 2,651,630.00 | — | — | — | — | 3.41 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 377.93 | +9.55% | inconclusive (ranges overlap) | 2,646,000.00 | — | — | — | — | 3.47 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 191.57 k | -39.70% | regression candidate | 5,219.92 | — | — | — | — | 3.70 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 289.77 | +7.43% | inconclusive (ranges overlap) | 3,451,020.00 | 13.04 ms | 19.03 ms | 28.81 ms | 42.30 ms | 3.70 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 4.06 M | -2.50% | inconclusive (ranges overlap) | 246.17 | — | — | — | — | 112.63 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 4.31 M | +7.46% | inconclusive (ranges overlap) | 231.92 | — | — | — | — | 113.56 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 4.37 M | +5.42% | inconclusive (ranges overlap) | 228.82 | — | — | — | — | 114.27 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 9.27 M | -0.35% | inconclusive (ranges overlap) | 107.88 | — | — | — | — | 114.50 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 6.02 M | +1.91% | inconclusive (ranges overlap) | 166.18 | — | — | — | — | 114.39 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.33 M | +0.32% | inconclusive (ranges overlap) | 157.87 | — | — | — | — | 114.34 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 18.98 M | +16.41% | inconclusive (ranges overlap) | 52.68 | — | — | — | — | 117.30 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 11.11 M | +11.53% | inconclusive (ranges overlap) | 90.04 | — | — | — | — | 118.52 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 10.40 M | +38.67% | inconclusive (ranges overlap) | 96.16 | — | — | — | — | 103.84 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 23.39 M | +0.99% | inconclusive (ranges overlap) | 42.75 | — | — | — | — | 123.28 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 12.59 M | +7.41% | inconclusive (ranges overlap) | 79.44 | — | — | — | — | 120.17 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.37 M | -0.69% | inconclusive (ranges overlap) | 96.45 | — | — | — | — | 110.38 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 487.96 k | -0.42% | inconclusive (ranges overlap) | 2,049.34 | — | — | — | — | 58.16 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 489.53 k | +1.13% | inconclusive (ranges overlap) | 2,042.76 | — | — | — | — | 59.39 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 364.72 k | -23.50% | regression candidate | 2,741.80 | — | — | — | — | 59.02 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 697.05 k | +2.43% | inconclusive (ranges overlap) | 1,434.61 | — | — | — | — | 61.17 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 510.51 k | +7.79% | inconclusive (ranges overlap) | 1,958.82 | — | — | — | — | 63.17 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 415.86 k | +2.55% | inconclusive (ranges overlap) | 2,404.64 | — | — | — | — | 65.27 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.12 M | -8.11% | regression candidate | 892.00 | — | — | — | — | 79.84 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 517.22 k | +8.12% | inconclusive (ranges overlap) | 1,933.43 | — | — | — | — | 66.11 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 515.76 k | +31.66% | inconclusive (ranges overlap) | 1,938.90 | — | — | — | — | 57.61 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 1.93 M | +0.20% | inconclusive (ranges overlap) | 517.58 | — | — | — | — | 59.64 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 513.82 k | -4.09% | inconclusive (ranges overlap) | 1,946.21 | — | — | — | — | 65.14 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 488.39 k | -5.99% | inconclusive (ranges overlap) | 2,047.54 | — | — | — | — | 93.70 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 923.55 k | +1.94% | inconclusive (ranges overlap) | 1,082.78 | — | — | — | — | 57.42 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.34 M | +2.62% | inconclusive (ranges overlap) | 743.56 | — | — | — | — | 61.06 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 1.98 M | -8.16% | regression candidate | 505.17 | — | — | — | — | 71.92 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.61 M | +1.32% | inconclusive (ranges overlap) | 277.34 | — | — | — | — | 62.83 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 164.22 k | +79.09% | inconclusive (ranges overlap) | 6,089.19 | 380.46 µs | 939.46 µs | 1.47 ms | 3.09 ms | 32.84 MiB | 26.28 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.36 k | -2.65% | inconclusive (ranges overlap) | 22,544.40 | 21.92 µs | 29.75 µs | 64.62 µs | 273.96 µs | 31.86 MiB | 7.10 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.80 k | -0.00% | inconclusive (ranges overlap) | 22,319.80 | 21.96 µs | 27.50 µs | 48.50 µs | 136.79 µs | 32.87 MiB | 7.17 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 50.74 k | -0.96% | inconclusive (ranges overlap) | 19,708.10 | 36.38 µs | 49.08 µs | 103.83 µs | 280.04 µs | 40.72 MiB | 8.12 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.83 M | +6.02% | inconclusive (ranges overlap) | 546.01 | 65.46 µs | 74.17 µs | 77.79 µs | 90.88 µs | 78.92 MiB | 293.03 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.43 M | +9.02% | inconclusive (ranges overlap) | 697.57 | 79.42 µs | 163.62 µs | 271.58 µs | 687.08 µs | 84.50 MiB | 229.37 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 155.04 k | +18.70% | inconclusive (ranges overlap) | 6,450.06 | 751.08 µs | 1.92 ms | 3.65 ms | 7.11 ms | 108.02 MiB | 24.81 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 432.74 k | +2.18% | inconclusive (ranges overlap) | 2,310.85 | 62.75 µs | 156.12 µs | 760.75 µs | 2.12 ms | 147.98 MiB | 484.67 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 884.98 k | +4.12% | inconclusive (ranges overlap) | 1,129.97 | 34.71 µs | 43.75 µs | 77.42 µs | 189.38 µs | 78.58 MiB | 141.60 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 16.41 k | +0.67% | inconclusive (ranges overlap) | 60,956.70 | 1.01 ms | 1.84 ms | 2.10 ms | 2.94 ms | 402.70 MiB | 1.08 G |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 762.21 k | +5.58% | inconclusive (ranges overlap) | 1,311.98 | 35.88 µs | 53.12 µs | 59.42 µs | 91.08 µs | 81.88 MiB | 121.95 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 162.08 k | -0.87% | inconclusive (ranges overlap) | 6,169.83 | 212.71 µs | 449.71 µs | 881.62 µs | 3.59 ms | 107.19 MiB | 25.93 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 4.21 k | -0.14% | inconclusive (ranges overlap) | 237,585.00 | 1.01 ms | 1.73 ms | 1.93 ms | 2.70 ms | 426.94 MiB | 1.10 G |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 299.36 k | +6.52% | inconclusive (ranges overlap) | 3,340.51 | 25.71 µs | 31.38 µs | 37.58 µs | 83.29 µs | 78.72 MiB | 47.90 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 286.34 k | +7.92% | inconclusive (ranges overlap) | 3,492.32 | 25.71 µs | 43.17 µs | 80.21 µs | 511.46 µs | 79.48 MiB | 45.81 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 133.06 k | -0.64% | inconclusive (ranges overlap) | 7,515.15 | 79.79 µs | 157.00 µs | 260.79 µs | 838.00 µs | 106.89 MiB | 21.29 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 79.88 k | +0.22% | inconclusive (ranges overlap) | 12,518.40 | 24.29 µs | 31.71 µs | 49.17 µs | 94.38 µs | 29.89 MiB | 12.78 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 63.83 k | -19.04% | regression candidate | 15,667.10 | 26.62 µs | 54.67 µs | 98.67 µs | 639.00 µs | 32.78 MiB | 10.21 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 85.93 k | +4.51% | inconclusive (ranges overlap) | 11,637.00 | 46.42 µs | 86.46 µs | 116.58 µs | 456.67 µs | 38.98 MiB | 13.75 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.52 M | +4.21% | improvement candidate | 283.78 | 67.04 µs | 74.62 µs | 81.92 µs | 143.00 µs | 74.86 MiB | 563.82 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 2.29 M | +23.80% | improvement candidate | 436.85 | 71.00 µs | 109.62 µs | 136.67 µs | 175.29 µs | 79.59 MiB | 366.26 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 121.96 k | -29.01% | inconclusive (ranges overlap) | 8,199.26 | 1.72 ms | 4.02 ms | 8.21 ms | 25.14 ms | 111.02 MiB | 19.51 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.66 M | +3.96% | improvement candidate | 602.07 | 36.67 µs | 43.54 µs | 52.33 µs | 88.08 µs | 79.05 MiB | 265.75 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.29 M | +6.55% | inconclusive (ranges overlap) | 773.24 | 40.79 µs | 113.71 µs | 210.46 µs | 554.62 µs | 78.16 MiB | 206.92 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 166.75 k | -11.38% | inconclusive (ranges overlap) | 5,997.20 | 358.00 µs | 831.46 µs | 1.58 ms | 3.50 ms | 109.80 MiB | 26.68 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 587.76 k | +2.58% | inconclusive (ranges overlap) | 1,701.36 | 26.54 µs | 53.12 µs | 88.08 µs | 205.50 µs | 76.47 MiB | 94.04 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 530.34 k | +2.60% | inconclusive (ranges overlap) | 1,885.57 | 26.67 µs | 39.92 µs | 54.17 µs | 80.00 µs | 77.62 MiB | 84.85 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 163.59 k | -1.16% | inconclusive (ranges overlap) | 6,112.96 | 114.08 µs | 250.54 µs | 375.58 µs | 1.18 ms | 109.62 MiB | 26.17 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 115.49 k | +2.37% | inconclusive (ranges overlap) | 8,658.97 | 32.38 µs | 53.33 µs | 63.88 µs | 77.67 µs | 34.31 MiB | 18.48 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 111.51 k | +2.67% | inconclusive (ranges overlap) | 8,967.89 | 33.04 µs | 60.33 µs | 108.04 µs | 253.42 µs | 34.05 MiB | 17.84 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 109.55 k | -1.37% | inconclusive (ranges overlap) | 9,128.45 | 69.92 µs | 115.62 µs | 570.58 µs | 2.39 ms | 42.72 MiB | 17.53 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.95 M | +5.87% | inconclusive (ranges overlap) | 168.08 | 78.75 µs | 193.92 µs | 259.33 µs | 348.46 µs | 81.12 MiB | 951.93 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 3.19 M | +16.36% | inconclusive (ranges overlap) | 313.52 | 85.75 µs | 161.62 µs | 236.71 µs | 299.71 µs | 77.09 MiB | 510.34 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 183.51 k | -29.19% | inconclusive (ranges overlap) | 5,449.16 | 2.07 ms | 4.42 ms | 24.85 ms | 50.54 ms | 113.94 MiB | 29.36 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.71 M | +6.18% | improvement candidate | 369.19 | 42.04 µs | 70.42 µs | 88.17 µs | 108.92 µs | 75.67 MiB | 433.38 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.75 M | +3.38% | inconclusive (ranges overlap) | 572.03 | 48.33 µs | 134.88 µs | 290.46 µs | 762.42 µs | 78.47 MiB | 279.70 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 268.43 k | -3.82% | inconclusive (ranges overlap) | 3,725.43 | 516.08 µs | 1.02 ms | 3.07 ms | 12.12 ms | 117.86 MiB | 42.95 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 830.92 k | +2.51% | inconclusive (ranges overlap) | 1,203.48 | 35.58 µs | 59.33 µs | 71.08 µs | 87.33 µs | 78.78 MiB | 132.95 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 683.92 k | -0.66% | inconclusive (ranges overlap) | 1,462.16 | 37.21 µs | 64.79 µs | 82.25 µs | 106.08 µs | 78.00 MiB | 109.43 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 188.60 k | -20.22% | inconclusive (ranges overlap) | 5,302.14 | 176.21 µs | 355.21 µs | 911.67 µs | 2.69 ms | 117.69 MiB | 30.18 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 131.51 k | +1.68% | inconclusive (ranges overlap) | 7,604.12 | 59.83 µs | 81.58 µs | 147.58 µs | 271.96 µs | 48.59 MiB | 21.04 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 129.57 k | -1.03% | inconclusive (ranges overlap) | 7,717.54 | 59.17 µs | 82.79 µs | 243.29 µs | 1.46 ms | 41.09 MiB | 20.73 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 175.25 k | -1.09% | inconclusive (ranges overlap) | 5,706.16 | 90.62 µs | 239.96 µs | 575.46 µs | 1.35 ms | 49.98 MiB | 28.04 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 9.15 M | -1.43% | inconclusive (ranges overlap) | 109.28 | 103.58 µs | 322.33 µs | 715.46 µs | 1.64 ms | 87.95 MiB | 1.46 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.67 M | +6.21% | inconclusive (ranges overlap) | 214.25 | 106.62 µs | 200.75 µs | 275.96 µs | 860.29 µs | 87.73 MiB | 746.78 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 195.72 k | -42.93% | regression candidate | 5,109.26 | 3.83 ms | 9.14 ms | 35.21 ms | 72.93 ms | 124.56 MiB | 31.32 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.65 M | +0.42% | inconclusive (ranges overlap) | 273.97 | 69.92 µs | 242.67 µs | 455.54 µs | 848.00 µs | 84.27 MiB | 584.00 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.54 M | -2.77% | inconclusive (ranges overlap) | 394.34 | 72.29 µs | 264.00 µs | 501.46 µs | 1.59 ms | 85.67 MiB | 405.74 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 220.42 k | -31.55% | inconclusive (ranges overlap) | 4,536.82 | 901.50 µs | 2.76 ms | 7.25 ms | 14.20 ms | 115.80 MiB | 35.27 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 997.93 k | -0.40% | inconclusive (ranges overlap) | 1,002.08 | 62.79 µs | 98.29 µs | 232.17 µs | 435.67 µs | 81.94 MiB | 159.67 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 914.57 k | +0.95% | inconclusive (ranges overlap) | 1,093.41 | 64.96 µs | 115.79 µs | 233.04 µs | 518.75 µs | 83.69 MiB | 146.33 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 278.89 k | +1.30% | inconclusive (ranges overlap) | 3,585.62 | 276.67 µs | 479.79 µs | 906.62 µs | 2.64 ms | 115.78 MiB | 44.62 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| copy-heavy | b971a15 | unknown | unknown | unknown |
| high-reclaim | b971a15 | unknown | unknown | unknown |
| low-reclaim | b971a15 | unknown | unknown | unknown |
| medium-reclaim | b971a15 | unknown | unknown | unknown |
| no-gain | b971a15 | unknown | unknown | unknown |
| ttl-50 | b971a15 | unknown | unknown | unknown |
| index-all-k16-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| generation-publication-adopt | unknown | unknown | unknown | unknown |
| generation-publication-get | unknown | unknown | unknown | unknown |
| generation-shell | unknown | unknown | unknown | unknown |
| churn-background | b971a15 | unknown | unknown | unknown |
| churn-cooperative | b971a15 | unknown | unknown | unknown |
| churn-disabled | b971a15 | unknown | unknown | unknown |
| forced-rotation-background | b971a15 | unknown | unknown | unknown |
| forced-rotation-cooperative | b971a15 | unknown | unknown | unknown |
| forced-rotation-disabled | b971a15 | unknown | unknown | unknown |
| idle-background | b971a15 | unknown | unknown | unknown |
| idle-cooperative | b971a15 | unknown | unknown | unknown |
| idle-disabled | b971a15 | unknown | unknown | unknown |
| mixed-background | b971a15 | unknown | unknown | unknown |
| mixed-cooperative | b971a15 | unknown | unknown | unknown |
| mixed-disabled | b971a15 | unknown | unknown | unknown |
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
| get-w1-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | b971a15 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-read-99-write-1 | 259.93 µs / 283.33 µs | 22 rec / 6656 B | 5.29 ms / 6.65 ms | 5.27 ms / 6.57 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 323.24 µs / 8.91 ms | 1 rec / 208 B | 5.08 ms / 17.73 ms | 5.00 ms / 17.57 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 290.60 µs / 7.13 ms | 32 rec / 6656 B | 5.16 ms / 12.56 ms | 5.08 ms / 12.43 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 289.86 µs / 7.82 ms | 1 rec / 208 B | 4.86 ms / 432.17 ms | 4.80 ms / 432.11 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 289.60 µs / 7.47 ms | 32 rec / 6656 B | 5.57 ms / 9.81 ms | 5.55 ms / 8.90 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 533.49 µs / 20.01 ms | 1 rec / 208 B | 5.24 ms / 16.71 ms | 5.18 ms / 16.62 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 17.29 ms / 664.69 ms | 3 rec / 624 B | 5.80 ms / 603.74 ms | 5.72 ms / 603.62 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 282.46 µs / 12.61 ms | 4 rec / 832 B | 5.29 ms / 419.56 ms | 5.16 ms / 419.35 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 351.31 µs / 20.13 ms | 4 rec / 832 B | 6.81 ms / 462.10 ms | 6.70 ms / 461.94 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 280.44 µs / 7.98 ms | 4 rec / 832 B | 5.24 ms / 20.88 ms | 5.15 ms / 20.85 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 71.09 µs / 12.44 ms | 4 rec / 832 B | 5.45 ms / 425.44 ms | 5.33 ms / 425.28 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 291.65 µs / 3.58 ms | 1 rec / 208 B | 10.47 ms / 431.80 ms | 10.38 ms / 431.74 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 300.73 µs / 3.72 ms | 1 rec / 208 B | 16.17 ms / 450.87 ms | 16.12 ms / 450.79 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 10.29 µs / 10.95 ms | 32 rec / 6656 B | 1.18 ms / 10.95 ms | 1.05 ms / 2.34 ms | 15.00 / 32.00 | 2 rec / 272 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 6.37 µs / 6.46 ms | 1 rec / 208 B | 124.56 µs / 7.67 ms | 936.60 µs / 4.78 ms | 32.00 / 32.00 | 15 rec / 2040 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 9.86 µs / 13.90 ms | 32 rec / 6656 B | 1.21 ms / 13.90 ms | — / 7.60 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 29.40 µs / 6.07 ms | 1 rec / 208 B | 188.48 µs / 20.90 ms | 911.88 µs / 5.68 ms | 29.75 / 32.00 | 26 rec / 3536 B | 14/0/0/5 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 9.90 µs / 11.46 ms | 32 rec / 6656 B | 2.04 ms / 11.44 ms | 728.12 µs / 2.07 ms | 14.00 / 32.00 | 30 rec / 4080 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 11.58 µs / 6.11 ms | 1 rec / 208 B | 144.34 µs / 16.49 ms | 894.23 µs / 5.90 ms | 30.22 / 32.00 | 28 rec / 3808 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 17.41 µs / 3.59 ms | 1 rec / 208 B | 310.63 µs / 47.75 ms | 4.35 ms / 46.13 ms | 31.60 / 32.00 | 52 rec / 7072 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 58.61 µs / 10.00 ms | 1 rec / 208 B | 223.45 µs / 14.51 ms | 4.42 ms / 13.30 ms | 31.23 / 32.00 | 116 rec / 15776 B | 12/0/0/1 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 13.40 µs / 143.89 ms | 32 rec / 6656 B | 4.37 ms / 161.68 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 831.48 µs / 19.56 ms | 1 rec / 208 B | 4.59 ms / 46.47 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 11.40 µs / 162.26 ms | 32 rec / 6656 B | 5.00 ms / 174.71 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 13.31 µs / 9.46 ms | 1 rec / 208 B | 5.20 ms / 163.30 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 12.14 µs / 144.72 ms | 32 rec / 6656 B | 4.36 ms / 160.97 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 29.53 µs / 10.30 ms | 1 rec / 208 B | 4.63 ms / 11.90 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 19.93 µs / 3.56 ms | 1 rec / 208 B | 10.30 ms / 433.75 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 16.80 µs / 1.33 ms | 1 rec / 208 B | 14.39 ms / 421.96 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
