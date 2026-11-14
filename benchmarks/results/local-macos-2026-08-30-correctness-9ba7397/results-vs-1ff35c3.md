# GlifiStore benchmark report

Generated at `2026-08-30T19:02:50+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

Baseline report: `2026-08-29T13:29:17+00:00`.

Environment identity: **compatible**; throughput deltas are shown.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 12.96 M | +0.09% | inconclusive (ranges overlap) | 77.18 | — | — | — | — | 40.75 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 12.02 M | +27.83% | improvement candidate | 83.21 | — | — | — | — | 42.34 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 13.73 M | +28.57% | improvement candidate | 72.86 | — | — | — | — | 42.34 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 14.05 M | +2.33% | inconclusive (ranges overlap) | 71.18 | — | — | — | — | 48.48 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 16.87 M | +0.68% | inconclusive (ranges overlap) | 59.29 | — | — | — | — | 64.78 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 11.65 M | -0.21% | inconclusive (ranges overlap) | 85.80 | — | — | — | — | 64.78 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.88 M | -1.24% | inconclusive (ranges overlap) | 530.83 | — | — | — | — | 479.19 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 4.53 M | +25.42% | inconclusive (ranges overlap) | 220.88 | — | — | — | — | 91.45 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 3.21 M | +17.06% | inconclusive (ranges overlap) | 311.13 | — | — | — | — | 184.00 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.84 k | +7.72% | inconclusive (ranges overlap) | 59,385.50 | — | — | — | — | 504.64 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 812.34 k | +9.05% | improvement candidate | 1,231.02 | — | — | — | — | 1,648.78 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 4.26 M | +30.45% | improvement candidate | 234.94 | — | — | — | — | 111.98 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 65.10 k | +2.31% | inconclusive (ranges overlap) | 15,362.10 | — | — | — | — | 505.98 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 565.02 k | -0.22% | inconclusive (ranges overlap) | 1,769.86 | — | — | — | — | 481.61 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 702.61 k | +7.21% | inconclusive (ranges overlap) | 1,423.27 | — | — | — | — | 93.56 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 683.93 k | +7.98% | improvement candidate | 1,462.14 | — | — | — | — | 187.78 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 14.22 k | -6.48% | inconclusive (ranges overlap) | 70,302.80 | — | — | — | — | 504.12 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 389.68 k | +5.50% | inconclusive (ranges overlap) | 2,566.24 | — | — | — | — | 1,653.31 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 671.71 k | +10.70% | inconclusive (ranges overlap) | 1,488.73 | — | — | — | — | 112.03 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 69.36 k | +29.12% | inconclusive (ranges overlap) | 14,417.40 | — | — | — | — | 505.94 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 622.51 k | +2.48% | inconclusive (ranges overlap) | 1,606.41 | — | — | — | — | 476.45 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 771.32 k | +9.74% | improvement candidate | 1,296.48 | — | — | — | — | 92.30 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 752.50 k | +11.82% | improvement candidate | 1,328.90 | — | — | — | — | 184.03 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.20 k | +3.29% | inconclusive (ranges overlap) | 65,794.80 | — | — | — | — | 504.31 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 407.86 k | +4.30% | improvement candidate | 2,451.80 | — | — | — | — | 1,652.50 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 779.29 k | +9.18% | inconclusive (ranges overlap) | 1,283.22 | — | — | — | — | 111.58 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 61.02 k | +12.81% | inconclusive (ranges overlap) | 16,388.00 | — | — | — | — | 505.95 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 363.21 k | -3.88% | inconclusive (ranges overlap) | 2,753.23 | — | — | — | — | 477.70 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 434.99 k | +8.97% | improvement candidate | 2,298.89 | — | — | — | — | 91.78 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 411.31 k | +4.18% | inconclusive (ranges overlap) | 2,431.23 | — | — | — | — | 185.28 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 15.46 k | +11.89% | inconclusive (ranges overlap) | 64,702.00 | — | — | — | — | 504.03 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 269.26 k | +1.71% | inconclusive (ranges overlap) | 3,713.85 | — | — | — | — | 1,652.13 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 433.22 k | +7.60% | inconclusive (ranges overlap) | 2,308.30 | — | — | — | — | 111.94 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 51.27 k | +8.05% | inconclusive (ranges overlap) | 19,504.10 | — | — | — | — | 505.87 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 632.21 k | +1.99% | inconclusive (ranges overlap) | 1,581.74 | — | — | — | — | 478.66 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 795.35 k | +3.14% | inconclusive (ranges overlap) | 1,257.30 | — | — | — | — | 92.34 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 776.14 k | +2.46% | inconclusive (ranges overlap) | 1,288.43 | — | — | — | — | 183.98 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 15.88 k | +7.69% | inconclusive (ranges overlap) | 62,976.10 | — | — | — | — | 504.56 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 406.65 k | +3.18% | inconclusive (ranges overlap) | 2,459.09 | — | — | — | — | 1,652.27 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 808.78 k | +4.80% | inconclusive (ranges overlap) | 1,236.43 | — | — | — | — | 112.20 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 59.25 k | +7.01% | inconclusive (ranges overlap) | 16,876.50 | — | — | — | — | 505.97 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.59 k | +22.66% | inconclusive (ranges overlap) | 53,778.20 | 50.04 µs | 84.83 µs | 93.50 µs | 147.04 µs | 6.00 MiB | 2.98 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 9.57 k | +11.46% | inconclusive (ranges overlap) | 104,494.00 | 48.25 µs | 73.17 µs | 99.79 µs | 177.88 µs | 6.02 MiB | 1.53 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 360.95 | -7.78% | inconclusive (ranges overlap) | 2,770,490.00 | 5.03 ms | 6.23 ms | 9.22 ms | 20.19 ms | 6.03 MiB | 57.75 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 38.34 k | -9.54% | inconclusive (ranges overlap) | 26,079.20 | 461.83 µs | 3.32 ms | 5.91 ms | 6.57 ms | 5.34 MiB | 6.14 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 14.31 k | +3.12% | inconclusive (ranges overlap) | 69,858.30 | 425.17 µs | 5.96 ms | 6.57 ms | 7.13 ms | 6.13 MiB | 2.29 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 319.01 | -18.05% | inconclusive (ranges overlap) | 3,134,660.00 | 92.96 ms | 209.44 ms | 575.13 ms | 831.35 ms | 6.33 MiB | 51.04 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 27.02 k | -41.13% | regression candidate | 37,002.80 | 161.25 µs | 595.88 µs | 1.17 ms | 2.68 ms | 6.03 MiB | 4.32 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.48 k | +0.27% | inconclusive (ranges overlap) | 80,148.60 | 127.21 µs | 4.88 ms | 5.93 ms | 14.95 ms | 6.09 MiB | 2.00 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 358.56 | -12.69% | regression candidate | 2,788,920.00 | 24.90 ms | 47.08 ms | 59.04 ms | 102.19 ms | 6.11 MiB | 57.37 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 349.30 | -7.06% | inconclusive (ranges overlap) | 2,862,870.00 | 339.99 ms | 830.73 ms | 1408.47 ms | 1547.36 ms | 6.69 MiB | 55.89 k |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.46 k | -1.58% | inconclusive (ranges overlap) | 683,812.00 | 90.08 ms | 210.24 ms | 718.61 ms | 778.85 ms | 6.73 MiB | 233.98 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.30 k | -7.79% | inconclusive (ranges overlap) | 766,750.00 | 93.93 ms | 194.16 ms | 433.43 ms | 555.02 ms | 6.69 MiB | 208.67 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.57 k | +5.81% | inconclusive (ranges overlap) | 637,242.00 | 89.85 ms | 237.83 ms | 331.10 ms | 687.54 ms | 5.80 MiB | 251.08 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.27 k | -21.02% | inconclusive (ranges overlap) | 785,816.00 | 98.03 ms | 241.97 ms | 905.78 ms | 945.73 ms | 6.67 MiB | 203.61 k |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 66.02 k | -12.07% | inconclusive (ranges overlap) | 15,148.10 | 448.79 µs | 1.11 ms | 1.55 ms | 1.83 ms | 6.61 MiB | 10.56 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 377.13 | +25.54% | inconclusive (ranges overlap) | 2,651,590.00 | 159.05 ms | 329.73 ms | 705.41 ms | 1009.89 ms | 8.55 MiB | 60.34 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 113.03 k | +18.13% | inconclusive (ranges overlap) | 8,846.92 | 617.83 µs | 1.46 ms | 2.48 ms | 2.86 ms | 13.09 MiB | 18.09 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 508.21 | +45.50% | inconclusive (ranges overlap) | 1,967,700.00 | 270.18 ms | 546.22 ms | 939.03 ms | 1044.74 ms | 13.02 MiB | 81.31 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.75 k | +1.67% | inconclusive (ranges overlap) | 53,330.60 | 51.17 µs | 72.00 µs | 98.67 µs | 123.21 µs | 5.98 MiB | 3.00 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 13.15 k | -22.25% | inconclusive (ranges overlap) | 76,050.10 | 57.54 µs | 87.50 µs | 106.04 µs | 307.04 µs | 5.95 MiB | 2.10 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.14 k | -5.64% | inconclusive (ranges overlap) | 98,572.50 | 75.54 µs | 643.67 µs | 4.82 ms | 7.12 ms | 6.00 MiB | 1.62 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 39.15 k | -21.14% | inconclusive (ranges overlap) | 25,543.90 | 385.38 µs | 869.21 µs | 1.09 ms | 1.26 ms | 5.25 MiB | 6.26 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 30.62 k | -2.53% | inconclusive (ranges overlap) | 32,658.60 | 406.75 µs | 3.12 ms | 25.42 ms | 25.70 ms | 6.00 MiB | 4.90 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 13.08 k | +1.23% | inconclusive (ranges overlap) | 76,471.20 | 926.00 µs | 6.87 ms | 8.28 ms | 22.81 ms | 6.33 MiB | 2.09 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 37.91 k | -3.72% | inconclusive (ranges overlap) | 26,380.60 | 119.12 µs | 216.62 µs | 289.79 µs | 1.04 ms | 6.06 MiB | 6.07 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 24.34 k | -16.56% | inconclusive (ranges overlap) | 41,093.10 | 127.58 µs | 283.88 µs | 965.33 µs | 13.98 ms | 6.05 MiB | 3.89 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 13.02 k | -14.45% | inconclusive (ranges overlap) | 76,805.30 | 256.00 µs | 4.96 ms | 6.97 ms | 9.97 ms | 6.08 MiB | 2.08 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 76.45 k | -7.17% | inconclusive (ranges overlap) | 13,081.20 | 472.21 µs | 1.13 ms | 1.39 ms | 1.57 ms | 8.50 MiB | 12.23 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 14.59 k | +32.89% | inconclusive (ranges overlap) | 68,525.50 | 2.02 ms | 16.20 ms | 31.37 ms | 43.90 ms | 8.52 MiB | 2.33 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 84.76 k | -1.52% | inconclusive (ranges overlap) | 11,797.90 | 726.62 µs | 1.60 ms | 2.10 ms | 2.20 ms | 13.02 MiB | 13.56 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 24.47 k | -23.39% | inconclusive (ranges overlap) | 40,867.00 | 2.53 ms | 25.49 ms | 48.07 ms | 63.10 ms | 13.03 MiB | 3.92 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 17.38 k | +27.31% | inconclusive (ranges overlap) | 57,539.90 | 55.88 µs | 90.79 µs | 104.54 µs | 176.25 µs | 5.02 MiB | 2.78 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.10 k | -0.70% | inconclusive (ranges overlap) | 99,017.60 | 53.25 µs | 88.62 µs | 106.25 µs | 145.04 µs | 5.97 MiB | 1.62 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 411.09 | -5.66% | inconclusive (ranges overlap) | 2,432,580.00 | 4.98 ms | 5.97 ms | 7.24 ms | 12.10 ms | 5.97 MiB | 65.77 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 33.18 k | -25.12% | inconclusive (ranges overlap) | 30,140.00 | 406.75 µs | 981.38 µs | 1.54 ms | 2.45 ms | 6.19 MiB | 5.31 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 15.06 k | -1.54% | inconclusive (ranges overlap) | 66,390.30 | 532.33 µs | 5.35 ms | 6.03 ms | 6.24 ms | 6.03 MiB | 2.41 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 398.89 | -4.83% | inconclusive (ranges overlap) | 2,506,970.00 | 84.99 ms | 173.64 ms | 408.14 ms | 829.84 ms | 6.31 MiB | 63.82 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 39.06 k | +2.45% | inconclusive (ranges overlap) | 25,598.80 | 127.67 µs | 227.04 µs | 301.71 µs | 457.12 µs | 5.95 MiB | 6.25 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 13.51 k | +0.14% | inconclusive (ranges overlap) | 74,010.10 | 160.38 µs | 4.24 ms | 5.65 ms | 6.11 ms | 5.97 MiB | 2.16 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 414.39 | -1.50% | inconclusive (ranges overlap) | 2,413,200.00 | 20.04 ms | 39.99 ms | 42.94 ms | 46.99 ms | 6.02 MiB | 66.30 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 62.23 k | -10.26% | inconclusive (ranges overlap) | 16,069.80 | 495.62 µs | 1.23 ms | 1.59 ms | 1.76 ms | 6.55 MiB | 9.96 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 403.92 | -1.42% | inconclusive (ranges overlap) | 2,475,750.00 | 152.32 ms | 334.67 ms | 625.64 ms | 882.16 ms | 6.58 MiB | 64.63 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 81.13 k | -23.68% | inconclusive (ranges overlap) | 12,326.00 | 712.46 µs | 1.61 ms | 2.09 ms | 2.21 ms | 9.13 MiB | 12.98 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 583.99 | -2.62% | inconclusive (ranges overlap) | 1,712,350.00 | 209.00 ms | 500.61 ms | 841.15 ms | 919.09 ms | 9.08 MiB | 93.44 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 150.17 | -8.61% | inconclusive (ranges overlap) | 6,659,100.00 | — | — | — | — | 4.00 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 506.69 k | -25.41% | inconclusive (ranges overlap) | 1,973.58 | — | — | — | — | 4.50 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 283.85 | -27.25% | inconclusive (ranges overlap) | 3,522,980.00 | — | — | — | — | 4.58 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 303.79 | -22.76% | inconclusive (ranges overlap) | 3,291,800.00 | — | — | — | — | 4.66 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 318.13 | -18.12% | inconclusive (ranges overlap) | 3,143,400.00 | 10.84 ms | 22.27 ms | 27.65 ms | 530.00 ms | 4.75 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.36 k | +4.84% | inconclusive (ranges overlap) | 297,481.00 | — | — | — | — | 3.05 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 1.08 M | +17.56% | inconclusive (ranges overlap) | 922.75 | — | — | — | — | 3.47 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 6.16 k | -3.16% | inconclusive (ranges overlap) | 162,255.00 | — | — | — | — | 3.58 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 5.13 k | -22.56% | inconclusive (ranges overlap) | 194,969.00 | — | — | — | — | 3.67 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 176.81 | -2.52% | inconclusive (ranges overlap) | 5,655,830.00 | — | — | — | — | 3.03 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 371.24 k | -40.06% | regression candidate | 2,693.67 | — | — | — | — | 3.34 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 397.71 | +4.12% | inconclusive (ranges overlap) | 2,514,360.00 | — | — | — | — | 3.42 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 390.68 | +13.25% | inconclusive (ranges overlap) | 2,559,620.00 | — | — | — | — | 3.52 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 183.94 k | -42.10% | regression candidate | 5,436.42 | — | — | — | — | 3.72 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 285.49 | +5.84% | inconclusive (ranges overlap) | 3,502,800.00 | 12.99 ms | 21.55 ms | 29.76 ms | 380.36 ms | 3.59 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 4.41 M | +5.86% | inconclusive (ranges overlap) | 226.73 | — | — | — | — | 113.47 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 4.43 M | +10.47% | inconclusive (ranges overlap) | 225.59 | — | — | — | — | 113.53 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 4.33 M | +4.47% | inconclusive (ranges overlap) | 230.90 | — | — | — | — | 113.45 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 9.96 M | +7.04% | inconclusive (ranges overlap) | 100.43 | — | — | — | — | 112.98 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 6.35 M | +7.62% | inconclusive (ranges overlap) | 157.36 | — | — | — | — | 120.78 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.78 M | +7.38% | improvement candidate | 147.49 | — | — | — | — | 114.14 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 19.33 M | +18.54% | inconclusive (ranges overlap) | 51.73 | — | — | — | — | 118.66 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 11.35 M | +14.02% | inconclusive (ranges overlap) | 88.08 | — | — | — | — | 117.42 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 7.79 M | +3.92% | inconclusive (ranges overlap) | 128.32 | — | — | — | — | 104.45 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 25.46 M | +9.92% | inconclusive (ranges overlap) | 39.27 | — | — | — | — | 130.05 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 12.57 M | +7.22% | inconclusive (ranges overlap) | 79.58 | — | — | — | — | 120.95 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 10.99 M | +5.31% | inconclusive (ranges overlap) | 90.96 | — | — | — | — | 110.27 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 489.31 k | -0.15% | inconclusive (ranges overlap) | 2,043.67 | — | — | — | — | 61.42 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 495.70 k | +2.41% | inconclusive (ranges overlap) | 2,017.36 | — | — | — | — | 57.36 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 482.73 k | +1.25% | inconclusive (ranges overlap) | 2,071.53 | — | — | — | — | 57.34 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 684.13 k | +0.53% | inconclusive (ranges overlap) | 1,461.70 | — | — | — | — | 69.55 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 502.96 k | +6.20% | inconclusive (ranges overlap) | 1,988.23 | — | — | — | — | 62.03 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 416.81 k | +2.79% | inconclusive (ranges overlap) | 2,399.15 | — | — | — | — | 59.94 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.12 M | -8.51% | regression candidate | 895.90 | — | — | — | — | 66.56 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 505.35 k | +5.64% | inconclusive (ranges overlap) | 1,978.81 | — | — | — | — | 60.33 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 392.60 k | +0.22% | inconclusive (ranges overlap) | 2,547.13 | — | — | — | — | 61.70 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 1.97 M | +2.42% | inconclusive (ranges overlap) | 506.37 | — | — | — | — | 60.19 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 528.09 k | -1.42% | inconclusive (ranges overlap) | 1,893.62 | — | — | — | — | 80.08 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 508.44 k | -2.13% | inconclusive (ranges overlap) | 1,966.78 | — | — | — | — | 74.77 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 919.87 k | +1.53% | inconclusive (ranges overlap) | 1,087.12 | — | — | — | — | 57.41 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.13 M | -13.77% | inconclusive (ranges overlap) | 884.95 | — | — | — | — | 62.44 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 2.10 M | -2.43% | inconclusive (ranges overlap) | 475.52 | — | — | — | — | 70.50 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.73 M | +4.92% | inconclusive (ranges overlap) | 267.83 | — | — | — | — | 61.58 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 149.91 k | +63.47% | inconclusive (ranges overlap) | 6,670.71 | 400.08 µs | 831.29 µs | 1.67 ms | 4.61 ms | 30.75 MiB | 23.99 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.59 k | -2.15% | inconclusive (ranges overlap) | 22,427.50 | 22.04 µs | 29.38 µs | 53.79 µs | 134.79 µs | 31.69 MiB | 7.13 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.39 k | -0.93% | inconclusive (ranges overlap) | 22,529.30 | 22.08 µs | 31.54 µs | 57.88 µs | 138.83 µs | 29.75 MiB | 7.10 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 52.60 k | +2.66% | inconclusive (ranges overlap) | 19,013.00 | 36.58 µs | 54.17 µs | 101.58 µs | 293.92 µs | 35.08 MiB | 8.42 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.78 M | +2.82% | inconclusive (ranges overlap) | 563.00 | 67.25 µs | 77.83 µs | 98.75 µs | 170.67 µs | 71.25 MiB | 284.19 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.44 M | +9.17% | improvement candidate | 696.61 | 78.25 µs | 93.12 µs | 100.96 µs | 117.08 µs | 77.84 MiB | 229.68 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 124.44 k | -4.72% | inconclusive (ranges overlap) | 8,035.78 | 930.50 µs | 2.31 ms | 4.67 ms | 13.96 ms | 101.44 MiB | 19.91 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 420.53 k | -0.70% | inconclusive (ranges overlap) | 2,377.97 | 63.92 µs | 78.75 µs | 120.33 µs | 219.21 µs | 134.81 MiB | 470.99 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 887.30 k | +4.40% | inconclusive (ranges overlap) | 1,127.02 | 34.92 µs | 45.71 µs | 86.71 µs | 167.58 µs | 71.22 MiB | 141.97 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 17.20 k | +5.55% | inconclusive (ranges overlap) | 58,136.50 | 977.75 µs | 1.81 ms | 2.10 ms | 2.36 ms | 387.98 MiB | 1.13 G |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 763.52 k | +5.76% | inconclusive (ranges overlap) | 1,309.71 | 35.88 µs | 53.33 µs | 60.00 µs | 84.25 µs | 74.11 MiB | 122.16 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 153.38 k | -6.18% | regression candidate | 6,519.57 | 219.75 µs | 448.46 µs | 826.92 µs | 3.15 ms | 99.89 MiB | 24.54 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 4.42 k | +4.90% | inconclusive (ranges overlap) | 226,170.00 | 962.21 µs | 1.66 ms | 1.95 ms | 2.17 ms | 412.36 MiB | 1.16 G |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 295.45 k | +5.13% | inconclusive (ranges overlap) | 3,384.65 | 26.25 µs | 43.88 µs | 76.08 µs | 189.88 µs | 71.59 MiB | 47.27 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 281.17 k | +5.97% | inconclusive (ranges overlap) | 3,556.54 | 26.21 µs | 46.46 µs | 90.79 µs | 285.42 µs | 71.81 MiB | 44.99 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 128.00 k | -4.42% | inconclusive (ranges overlap) | 7,812.42 | 79.79 µs | 152.67 µs | 251.50 µs | 811.00 µs | 100.03 MiB | 20.48 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 79.09 k | -0.77% | inconclusive (ranges overlap) | 12,644.00 | 24.50 µs | 32.17 µs | 44.83 µs | 119.62 µs | 27.16 MiB | 12.65 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 77.56 k | -1.63% | inconclusive (ranges overlap) | 12,894.10 | 24.75 µs | 45.58 µs | 74.83 µs | 158.58 µs | 30.11 MiB | 12.41 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 85.00 k | +3.38% | inconclusive (ranges overlap) | 11,764.90 | 44.96 µs | 83.21 µs | 134.46 µs | 442.62 µs | 34.64 MiB | 13.60 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.45 M | +2.16% | inconclusive (ranges overlap) | 289.48 | 69.62 µs | 100.96 µs | 122.79 µs | 155.62 µs | 67.66 MiB | 552.71 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 2.29 M | +23.80% | inconclusive (ranges overlap) | 436.83 | 71.46 µs | 116.42 µs | 145.79 µs | 181.67 µs | 73.20 MiB | 366.27 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 166.15 k | -3.29% | inconclusive (ranges overlap) | 6,018.55 | 1.38 ms | 3.22 ms | 5.11 ms | 8.09 ms | 103.63 MiB | 26.58 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.64 M | +2.55% | inconclusive (ranges overlap) | 610.39 | 37.50 µs | 57.38 µs | 93.67 µs | 191.96 µs | 67.56 MiB | 262.13 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.30 M | +6.86% | inconclusive (ranges overlap) | 771.05 | 39.92 µs | 88.67 µs | 200.79 µs | 513.21 µs | 71.11 MiB | 207.51 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 155.47 k | -17.37% | inconclusive (ranges overlap) | 6,431.91 | 407.92 µs | 980.50 µs | 1.65 ms | 3.29 ms | 103.34 MiB | 24.88 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 579.68 k | +1.17% | inconclusive (ranges overlap) | 1,725.10 | 26.42 µs | 33.92 µs | 42.75 µs | 71.71 µs | 71.47 MiB | 92.75 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 528.21 k | +2.19% | inconclusive (ranges overlap) | 1,893.19 | 27.25 µs | 52.04 µs | 85.33 µs | 164.04 µs | 72.09 MiB | 84.51 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 170.71 k | +3.14% | inconclusive (ranges overlap) | 5,858.07 | 116.62 µs | 251.88 µs | 405.17 µs | 925.42 µs | 100.80 MiB | 27.31 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 114.34 k | +1.36% | inconclusive (ranges overlap) | 8,745.79 | 32.62 µs | 55.21 µs | 70.58 µs | 118.29 µs | 32.16 MiB | 18.29 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 110.87 k | +2.08% | inconclusive (ranges overlap) | 9,019.58 | 33.46 µs | 57.25 µs | 83.46 µs | 246.96 µs | 34.47 MiB | 17.74 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 110.18 k | -0.80% | inconclusive (ranges overlap) | 9,076.07 | 71.46 µs | 122.50 µs | 285.12 µs | 651.54 µs | 44.16 MiB | 17.63 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.06 M | -9.99% | inconclusive (ranges overlap) | 197.69 | 92.62 µs | 362.58 µs | 749.04 µs | 2.23 ms | 69.39 MiB | 809.36 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 3.15 M | +15.05% | inconclusive (ranges overlap) | 317.08 | 83.25 µs | 167.67 µs | 252.33 µs | 364.12 µs | 73.22 MiB | 504.61 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 285.84 k | +10.29% | inconclusive (ranges overlap) | 3,498.42 | 1.91 ms | 3.98 ms | 10.28 ms | 18.69 ms | 110.61 MiB | 45.73 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.61 M | +2.38% | inconclusive (ranges overlap) | 382.91 | 44.08 µs | 106.62 µs | 171.08 µs | 363.38 µs | 67.72 MiB | 417.85 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.72 M | +1.65% | inconclusive (ranges overlap) | 581.77 | 48.25 µs | 86.04 µs | 108.75 µs | 137.88 µs | 71.44 MiB | 275.02 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 195.59 k | -29.92% | regression candidate | 5,112.77 | 571.38 µs | 1.28 ms | 3.59 ms | 11.06 ms | 112.41 MiB | 31.29 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 824.91 k | +1.77% | inconclusive (ranges overlap) | 1,212.26 | 37.17 µs | 80.33 µs | 155.08 µs | 725.21 µs | 69.39 MiB | 131.99 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 704.29 k | +2.30% | inconclusive (ranges overlap) | 1,419.88 | 39.83 µs | 105.75 µs | 166.50 µs | 516.50 µs | 72.20 MiB | 112.69 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 229.57 k | -2.90% | inconclusive (ranges overlap) | 4,356.03 | 177.12 µs | 340.00 µs | 804.04 µs | 1.48 ms | 106.34 MiB | 36.73 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 129.71 k | +0.29% | inconclusive (ranges overlap) | 7,709.63 | 60.79 µs | 90.50 µs | 255.04 µs | 736.75 µs | 45.58 MiB | 20.75 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 126.17 k | -3.64% | inconclusive (ranges overlap) | 7,926.11 | 61.42 µs | 89.58 µs | 202.00 µs | 552.58 µs | 37.87 MiB | 20.19 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 169.50 k | -4.34% | inconclusive (ranges overlap) | 5,899.74 | 93.08 µs | 284.33 µs | 601.71 µs | 1.22 ms | 47.67 MiB | 27.12 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 8.55 M | -7.92% | inconclusive (ranges overlap) | 116.97 | 110.25 µs | 371.88 µs | 799.54 µs | 1.46 ms | 80.16 MiB | 1.37 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.04 M | -7.96% | inconclusive (ranges overlap) | 247.22 | 110.58 µs | 219.17 µs | 285.75 µs | 383.96 µs | 77.22 MiB | 647.18 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 294.99 k | -13.98% | inconclusive (ranges overlap) | 3,389.94 | 3.39 ms | 7.50 ms | 25.86 ms | 46.95 ms | 110.03 MiB | 47.20 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.65 M | +0.36% | inconclusive (ranges overlap) | 274.13 | 66.67 µs | 87.79 µs | 99.50 µs | 117.04 µs | 76.23 MiB | 583.65 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.59 M | -0.65% | inconclusive (ranges overlap) | 385.96 | 70.88 µs | 106.46 µs | 130.17 µs | 162.79 µs | 79.66 MiB | 414.55 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 272.00 k | -15.53% | inconclusive (ranges overlap) | 3,676.46 | 924.88 µs | 1.94 ms | 7.00 ms | 11.70 ms | 109.92 MiB | 43.52 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 1.00 M | -0.15% | inconclusive (ranges overlap) | 999.59 | 63.96 µs | 108.04 µs | 213.04 µs | 724.17 µs | 82.52 MiB | 160.06 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 891.03 k | -1.65% | inconclusive (ranges overlap) | 1,122.30 | 65.62 µs | 105.46 µs | 236.75 µs | 399.50 µs | 77.47 MiB | 142.56 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 282.24 k | +2.52% | inconclusive (ranges overlap) | 3,543.08 | 270.79 µs | 516.62 µs | 1.92 ms | 4.01 ms | 109.67 MiB | 45.16 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| copy-heavy | 9ba7397 | unknown | unknown | unknown |
| high-reclaim | 9ba7397 | unknown | unknown | unknown |
| low-reclaim | 9ba7397 | unknown | unknown | unknown |
| medium-reclaim | 9ba7397 | unknown | unknown | unknown |
| no-gain | 9ba7397 | unknown | unknown | unknown |
| ttl-50 | 9ba7397 | unknown | unknown | unknown |
| index-all-k16-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| generation-publication-adopt | unknown | unknown | unknown | unknown |
| generation-publication-get | unknown | unknown | unknown | unknown |
| generation-shell | unknown | unknown | unknown | unknown |
| churn-background | 9ba7397 | unknown | unknown | unknown |
| churn-cooperative | 9ba7397 | unknown | unknown | unknown |
| churn-disabled | 9ba7397 | unknown | unknown | unknown |
| forced-rotation-background | 9ba7397 | unknown | unknown | unknown |
| forced-rotation-cooperative | 9ba7397 | unknown | unknown | unknown |
| forced-rotation-disabled | 9ba7397 | unknown | unknown | unknown |
| idle-background | 9ba7397 | unknown | unknown | unknown |
| idle-cooperative | 9ba7397 | unknown | unknown | unknown |
| idle-disabled | 9ba7397 | unknown | unknown | unknown |
| mixed-background | 9ba7397 | unknown | unknown | unknown |
| mixed-cooperative | 9ba7397 | unknown | unknown | unknown |
| mixed-disabled | 9ba7397 | unknown | unknown | unknown |
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
| get-w1-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | 9ba7397 | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-read-99-write-1 | 270.89 µs / 7.08 ms | 30 rec / 6656 B | 4.98 ms / 14.42 ms | 4.95 ms / 14.07 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 288.07 µs / 20.03 ms | 1 rec / 208 B | 4.95 ms / 13.99 ms | 4.90 ms / 13.98 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 266.71 µs / 8.33 ms | 32 rec / 6656 B | 4.76 ms / 11.99 ms | 4.71 ms / 11.61 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 288.63 µs / 12.73 ms | 1 rec / 208 B | 5.89 ms / 423.92 ms | 5.83 ms / 423.84 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 272.18 µs / 7.98 ms | 31 rec / 6656 B | 4.86 ms / 18.17 ms | 4.84 ms / 17.59 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 285.29 µs / 20.61 ms | 1 rec / 208 B | 5.18 ms / 16.21 ms | 5.11 ms / 16.16 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 17.00 ms / 680.00 ms | 3 rec / 624 B | 5.71 ms / 647.57 ms | 5.65 ms / 647.41 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 281.83 µs / 2.39 ms | 4 rec / 832 B | 5.08 ms / 414.05 ms | 4.99 ms / 413.88 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 272.03 µs / 5.19 ms | 4 rec / 832 B | 5.75 ms / 289.46 ms | 5.66 ms / 289.36 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 274.63 µs / 3.73 ms | 4 rec / 832 B | 4.73 ms / 424.97 ms | 4.65 ms / 424.80 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 43.27 µs / 9.89 ms | 4 rec / 832 B | 5.99 ms / 525.88 ms | 5.84 ms / 525.77 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 289.71 µs / 8.96 ms | 1 rec / 208 B | 10.15 ms / 434.87 ms | 10.10 ms / 434.78 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 325.06 µs / 15.55 ms | 1 rec / 208 B | 15.20 ms / 172.48 ms | 15.16 ms / 172.42 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 6.15 µs / 16.11 ms | 32 rec / 6656 B | 1.25 ms / 16.09 ms | 717.67 µs / 2.16 ms | 14.00 / 32.00 | 2 rec / 272 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 5.75 µs / 3.24 ms | 1 rec / 208 B | 127.48 µs / 7.26 ms | 888.40 µs / 7.25 ms | 32.00 / 32.00 | 29 rec / 3944 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 10.04 µs / 11.08 ms | 32 rec / 6656 B | 935.65 µs / 25.00 ms | — / 10.89 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 7.65 µs / 199.54 µs | 1 rec / 208 B | 102.64 µs / 21.69 ms | 644.02 µs / 1.83 ms | 32.00 / 32.00 | 27 rec / 3672 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 11.35 µs / 10.14 ms | 32 rec / 6656 B | 1.37 ms / 13.65 ms | 638.75 µs / 6.68 ms | 13.00 / 32.00 | 15 rec / 2040 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 7.00 µs / 2.75 ms | 1 rec / 208 B | 106.66 µs / 7.24 ms | 741.16 µs / 1.36 ms | 32.00 / 32.00 | 15 rec / 2040 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 10.98 µs / 5.86 ms | 1 rec / 208 B | 219.80 µs / 11.24 ms | 3.36 ms / 11.23 ms | 31.53 / 32.00 | 53 rec / 7208 B | 13/0/0/1 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 78.37 µs / 9.98 ms | 1 rec / 208 B | 192.99 µs / 17.04 ms | 4.45 ms / 17.01 ms | 31.23 / 32.00 | 116 rec / 15776 B | 12/0/0/1 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 8.71 µs / 131.06 ms | 32 rec / 6656 B | 4.32 ms / 160.78 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 11.72 µs / 4.96 ms | 1 rec / 208 B | 4.71 ms / 12.51 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 7.65 µs / 143.03 ms | 32 rec / 6656 B | 4.31 ms / 143.03 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 12.21 µs / 768.38 µs | 1 rec / 208 B | 4.93 ms / 429.58 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 9.28 µs / 141.86 ms | 32 rec / 6656 B | 4.55 ms / 157.81 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 11.26 µs / 2.72 ms | 1 rec / 208 B | 4.72 ms / 12.64 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 26.52 µs / 9.94 ms | 1 rec / 208 B | 9.75 ms / 424.70 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 14.90 µs / 4.93 ms | 1 rec / 208 B | 13.43 ms / 443.98 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
