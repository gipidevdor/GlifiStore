# GlifiStore benchmark report

Generated at `2026-08-27T03:28:10+00:00` from 180 result(s).

> Results from GitHub-hosted runners are suitable for observing large regressions, not for absolute performance claims. Runner contention and hardware can vary.

No retained baseline is available; throughput deltas are not shown.

| Suite | Benchmark | Configuration | Median ops/s | Δ ops/s | Interpretation | Median ns/op | p50 | p95 | p99 | p99.9 | RSS | Duplex |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| index-all-k16-v64 | index_insert | k=16, v=64, w=1, t=1, uniform | 13.38 M | — | — | 74.74 | — | — | — | — | 40.73 MiB | 0.00 |
| index-all-k16-v64 | index_replace | k=16, v=64, w=1, t=1, uniform | 12.94 M | — | — | 77.26 | — | — | — | — | 42.33 MiB | 0.00 |
| index-all-k16-v64 | index_find_hit | k=16, v=64, w=1, t=1, uniform | 14.10 M | — | — | 70.94 | — | — | — | — | 42.33 MiB | 0.00 |
| index-all-k16-v64 | index_find_miss | k=16, v=64, w=1, t=1, uniform | 14.06 M | — | — | 71.10 | — | — | — | — | 48.47 MiB | 0.00 |
| index-all-k16-v64 | index_churn_miss | k=16, v=64, w=1, t=1, uniform | 17.32 M | — | — | 57.72 | — | — | — | — | 64.73 MiB | 0.00 |
| index-all-k16-v64 | index_erase | k=16, v=64, w=1, t=1, uniform | 13.79 M | — | — | 72.50 | — | — | — | — | 64.73 MiB | 0.00 |
| store-get-v1024 | store_get_copy | k=16, v=1024, w=1, t=1, uniform | 1.82 M | — | — | 549.59 | — | — | — | — | 607.42 MiB | 0.00 |
| store-get-v16 | store_get_copy | k=16, v=16, w=1, t=1, uniform | 3.86 M | — | — | 258.92 | — | — | — | — | 222.39 MiB | 0.00 |
| store-get-v256 | store_get_copy | k=16, v=256, w=1, t=1, uniform | 2.87 M | — | — | 348.95 | — | — | — | — | 307.34 MiB | 0.00 |
| store-get-v262144 | store_get_copy | k=16, v=262144, w=1, t=1, uniform | 16.63 k | — | — | 60,120.20 | — | — | — | — | 504.59 MiB | 0.00 |
| store-get-v4096 | store_get_copy | k=16, v=4096, w=1, t=1, uniform | 786.56 k | — | — | 1,271.36 | — | — | — | — | 1,771.74 MiB | 0.00 |
| store-get-v64 | store_get_copy | k=16, v=64, w=1, t=1, uniform | 3.76 M | — | — | 266.05 | — | — | — | — | 235.11 MiB | 0.00 |
| store-get-v65536 | store_get_copy | k=16, v=65536, w=1, t=1, uniform | 64.50 k | — | — | 15,502.90 | — | — | — | — | 506.72 MiB | 0.00 |
| store-put-batch-v1024 | store_put_batch | k=16, v=1024, w=1, t=1, uniform | 606.44 k | — | — | 1,648.97 | — | — | — | — | 608.42 MiB | 0.00 |
| store-put-batch-v16 | store_put_batch | k=16, v=16, w=1, t=1, uniform | 709.85 k | — | — | 1,408.74 | — | — | — | — | 223.58 MiB | 0.00 |
| store-put-batch-v256 | store_put_batch | k=16, v=256, w=1, t=1, uniform | 703.61 k | — | — | 1,421.25 | — | — | — | — | 313.94 MiB | 0.00 |
| store-put-batch-v262144 | store_put_batch | k=16, v=262144, w=1, t=1, uniform | 16.70 k | — | — | 59,896.20 | — | — | — | — | 504.12 MiB | 0.00 |
| store-put-batch-v4096 | store_put_batch | k=16, v=4096, w=1, t=1, uniform | 406.43 k | — | — | 2,460.46 | — | — | — | — | 1,778.22 MiB | 0.00 |
| store-put-batch-v64 | store_put_batch | k=16, v=64, w=1, t=1, uniform | 708.90 k | — | — | 1,410.63 | — | — | — | — | 239.45 MiB | 0.00 |
| store-put-batch-v65536 | store_put_batch | k=16, v=65536, w=1, t=1, uniform | 63.60 k | — | — | 15,723.80 | — | — | — | — | 505.94 MiB | 0.00 |
| store-put-get-v1024 | store_put_get_copy | k=16, v=1024, w=1, t=1, uniform | 715.61 k | — | — | 1,397.41 | — | — | — | — | 602.73 MiB | 0.00 |
| store-put-get-v16 | store_put_get_copy | k=16, v=16, w=1, t=1, uniform | 922.12 k | — | — | 1,084.45 | — | — | — | — | 216.75 MiB | 0.00 |
| store-put-get-v256 | store_put_get_copy | k=16, v=256, w=1, t=1, uniform | 871.95 k | — | — | 1,146.85 | — | — | — | — | 307.84 MiB | 0.00 |
| store-put-get-v262144 | store_put_get_copy | k=16, v=262144, w=1, t=1, uniform | 15.49 k | — | — | 64,566.40 | — | — | — | — | 504.58 MiB | 0.00 |
| store-put-get-v4096 | store_put_get_copy | k=16, v=4096, w=1, t=1, uniform | 457.72 k | — | — | 2,184.73 | — | — | — | — | 1,775.59 MiB | 0.00 |
| store-put-get-v64 | store_put_get_copy | k=16, v=64, w=1, t=1, uniform | 941.98 k | — | — | 1,061.59 | — | — | — | — | 233.84 MiB | 0.00 |
| store-put-get-v65536 | store_put_get_copy | k=16, v=65536, w=1, t=1, uniform | 63.67 k | — | — | 15,704.90 | — | — | — | — | 506.02 MiB | 0.00 |
| store-put-v1024 | store_put | k=16, v=1024, w=1, t=1, uniform | 448.79 k | — | — | 2,228.24 | — | — | — | — | 607.50 MiB | 0.00 |
| store-put-v16 | store_put | k=16, v=16, w=1, t=1, uniform | 542.21 k | — | — | 1,844.31 | — | — | — | — | 211.30 MiB | 0.00 |
| store-put-v256 | store_put | k=16, v=256, w=1, t=1, uniform | 519.65 k | — | — | 1,924.39 | — | — | — | — | 307.81 MiB | 0.00 |
| store-put-v262144 | store_put | k=16, v=262144, w=1, t=1, uniform | 17.22 k | — | — | 58,070.80 | — | — | — | — | 504.20 MiB | 0.00 |
| store-put-v4096 | store_put | k=16, v=4096, w=1, t=1, uniform | 316.68 k | — | — | 3,157.80 | — | — | — | — | 1,775.42 MiB | 0.00 |
| store-put-v64 | store_put | k=16, v=64, w=1, t=1, uniform | 547.86 k | — | — | 1,825.28 | — | — | — | — | 238.23 MiB | 0.00 |
| store-put-v65536 | store_put | k=16, v=65536, w=1, t=1, uniform | 53.15 k | — | — | 18,814.80 | — | — | — | — | 506.30 MiB | 0.00 |
| store-read-after-write-v1024 | store_read_after_write_copy | k=16, v=1024, w=1, t=1, uniform | 758.30 k | — | — | 1,318.73 | — | — | — | — | 601.20 MiB | 0.00 |
| store-read-after-write-v16 | store_read_after_write_copy | k=16, v=16, w=1, t=1, uniform | 1.00 M | — | — | 998.46 | — | — | — | — | 213.47 MiB | 0.00 |
| store-read-after-write-v256 | store_read_after_write_copy | k=16, v=256, w=1, t=1, uniform | 928.39 k | — | — | 1,077.14 | — | — | — | — | 307.45 MiB | 0.00 |
| store-read-after-write-v262144 | store_read_after_write_copy | k=16, v=262144, w=1, t=1, uniform | 15.87 k | — | — | 63,015.50 | — | — | — | — | 504.67 MiB | 0.00 |
| store-read-after-write-v4096 | store_read_after_write_copy | k=16, v=4096, w=1, t=1, uniform | 470.52 k | — | — | 2,125.31 | — | — | — | — | 1,774.75 MiB | 0.00 |
| store-read-after-write-v64 | store_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 956.20 k | — | — | 1,045.80 | — | — | — | — | 240.50 MiB | 0.00 |
| store-read-after-write-v65536 | store_read_after_write_copy | k=16, v=65536, w=1, t=1, uniform | 62.73 k | — | — | 15,940.90 | — | — | — | — | 506.69 MiB | 0.00 |
| durable-group-w1-c1-p1-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 14.75 k | — | — | 67,816.70 | 63.38 µs | 87.54 µs | 101.75 µs | 113.71 µs | 4.91 MiB | 2.36 M |
| durable-group-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 9.23 k | — | — | 108,371.00 | 58.00 µs | 67.12 µs | 76.42 µs | 96.71 µs | 4.94 MiB | 1.48 M |
| durable-group-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=1 | 422.26 | — | — | 2,368,200.00 | 4.97 ms | 6.01 ms | 11.61 ms | 15.62 ms | 4.95 MiB | 67.56 k |
| durable-group-w1-c1-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 43.57 k | — | — | 22,951.20 | 377.62 µs | 742.17 µs | 868.88 µs | 973.58 µs | 6.23 MiB | 6.97 M |
| durable-group-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 13.61 k | — | — | 73,498.50 | 412.58 µs | 6.37 ms | 7.20 ms | 7.69 ms | 5.97 MiB | 2.18 M |
| durable-group-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=32 | 337.07 | — | — | 2,966,730.00 | 89.52 ms | 203.17 ms | 499.32 ms | 832.39 ms | 6.25 MiB | 53.93 k |
| durable-group-w1-c1-p8-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 47.55 k | — | — | 21,028.60 | 115.04 µs | 204.92 µs | 277.96 µs | 362.62 µs | 4.91 MiB | 7.61 M |
| durable-group-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 12.17 k | — | — | 82,179.20 | 129.42 µs | 5.48 ms | 7.85 ms | 12.43 ms | 5.94 MiB | 1.95 M |
| durable-group-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=1, owner-bound, p=8 | 357.58 | — | — | 2,796,610.00 | 24.97 ms | 46.04 ms | 55.14 ms | 79.11 ms | 5.92 MiB | 57.21 k |
| durable-group-w1-c4-p32-batch1 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 4.48 k | — | — | 223,275.00 | 23.90 ms | 100.11 ms | 152.74 ms | 193.07 ms | 6.56 MiB | 716.60 k |
| durable-group-w1-c4-p32-batch128 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.48 k | — | — | 675,153.00 | 86.77 ms | 171.09 ms | 212.58 ms | 231.83 ms | 6.67 MiB | 236.98 k |
| durable-group-w1-c4-p32-batch16 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.51 k | — | — | 663,230.00 | 84.88 ms | 167.89 ms | 194.04 ms | 216.77 ms | 5.66 MiB | 241.24 k |
| durable-group-w1-c4-p32-batch32 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 1.35 k | — | — | 739,165.00 | 97.05 ms | 196.46 ms | 227.04 ms | 261.99 ms | 6.64 MiB | 216.46 k |
| durable-group-w1-c4-p32-batch4 | server_tcp_read_after_write_durable-group | k=16, v=64, w=1, t=4, owner-bound, p=32 | 12.47 k | — | — | 80,161.20 | 9.31 ms | 29.28 ms | 76.27 ms | 83.76 ms | 6.61 MiB | 2.00 M |
| durable-group-w2-c2-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 67.79 k | — | — | 14,751.60 | 464.46 µs | 1.03 ms | 1.44 ms | 1.75 ms | 8.45 MiB | 10.85 M |
| durable-group-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=2, t=2, owner-bound, p=32 | 356.92 | — | — | 2,801,720.00 | 185.52 ms | 450.45 ms | 790.55 ms | 895.80 ms | 8.55 MiB | 57.11 k |
| durable-group-w4-c4-p32-get-only | server_tcp_get_only_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 97.66 k | — | — | 10,239.40 | 616.33 µs | 1.28 ms | 1.82 ms | 1.95 ms | 12.94 MiB | 15.63 M |
| durable-group-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-group | k=16, v=64, w=4, t=4, owner-bound, p=32 | 496.90 | — | — | 2,012,490.00 | 265.29 ms | 556.35 ms | 782.65 ms | 841.17 ms | 9.14 MiB | 79.50 k |
| durable-periodic-w1-c1-p1-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 19.73 k | — | — | 50,692.50 | 49.62 µs | 75.38 µs | 89.33 µs | 117.46 µs | 5.84 MiB | 3.16 M |
| durable-periodic-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 12.86 k | — | — | 77,754.40 | 57.54 µs | 81.29 µs | 109.54 µs | 330.29 µs | 5.87 MiB | 2.06 M |
| durable-periodic-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.56 k | — | — | 94,666.30 | 74.29 µs | 503.92 µs | 5.16 ms | 6.90 ms | 5.86 MiB | 1.69 M |
| durable-periodic-w1-c1-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 49.12 k | — | — | 20,360.20 | 341.92 µs | 673.75 µs | 803.42 µs | 995.79 µs | 6.20 MiB | 7.86 M |
| durable-periodic-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 27.76 k | — | — | 36,023.60 | 402.04 µs | 3.91 ms | 5.48 ms | 5.69 ms | 5.98 MiB | 4.44 M |
| durable-periodic-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=32 | 15.07 k | — | — | 66,363.20 | 884.50 µs | 6.87 ms | 7.53 ms | 16.89 ms | 6.20 MiB | 2.41 M |
| durable-periodic-w1-c1-p8-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 34.08 k | — | — | 29,341.10 | 139.79 µs | 423.25 µs | 1.17 ms | 1.61 ms | 5.91 MiB | 5.45 M |
| durable-periodic-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 27.19 k | — | — | 36,782.10 | 134.96 µs | 290.62 µs | 2.66 ms | 4.45 ms | 5.87 MiB | 4.35 M |
| durable-periodic-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=1, t=1, owner-bound, p=8 | 10.57 k | — | — | 94,621.30 | 296.29 µs | 5.10 ms | 8.25 ms | 17.31 ms | 5.91 MiB | 1.69 M |
| durable-periodic-w2-c2-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 67.42 k | — | — | 14,832.80 | 479.04 µs | 1.00 ms | 1.56 ms | 2.06 ms | 8.50 MiB | 10.79 M |
| durable-periodic-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=2, t=2, owner-bound, p=32 | 10.86 k | — | — | 92,047.10 | 5.71 ms | 16.73 ms | 23.37 ms | 25.71 ms | 8.50 MiB | 1.74 M |
| durable-periodic-w4-c4-p32-get-only | server_tcp_get_only_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 95.56 k | — | — | 10,464.20 | 738.92 µs | 1.63 ms | 2.05 ms | 2.35 ms | 12.87 MiB | 15.29 M |
| durable-periodic-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-periodic | k=16, v=64, w=4, t=4, owner-bound, p=32 | 13.79 k | — | — | 72,505.00 | 2.10 ms | 20.35 ms | 31.63 ms | 39.05 ms | 12.87 MiB | 2.21 M |
| durable-sync-w1-c1-p1-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 18.25 k | — | — | 54,786.40 | 49.42 µs | 101.71 µs | 171.67 µs | 275.46 µs | 5.81 MiB | 2.92 M |
| durable-sync-w1-c1-p1-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 10.78 k | — | — | 92,773.80 | 48.58 µs | 87.21 µs | 100.83 µs | 120.04 µs | 5.84 MiB | 1.72 M |
| durable-sync-w1-c1-p1-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=1 | 421.53 | — | — | 2,372,330.00 | 4.92 ms | 6.10 ms | 8.02 ms | 14.86 ms | 5.86 MiB | 67.44 k |
| durable-sync-w1-c1-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 38.13 k | — | — | 26,224.50 | 410.75 µs | 858.50 µs | 1.29 ms | 1.65 ms | 6.14 MiB | 6.10 M |
| durable-sync-w1-c1-p32-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 15.03 k | — | — | 66,555.60 | 549.46 µs | 5.10 ms | 5.77 ms | 6.11 ms | 5.87 MiB | 2.40 M |
| durable-sync-w1-c1-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=32 | 399.30 | — | — | 2,504,380.00 | 80.09 ms | 155.08 ms | 168.01 ms | 184.06 ms | 6.20 MiB | 63.89 k |
| durable-sync-w1-c1-p8-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 34.34 k | — | — | 29,124.20 | 127.62 µs | 255.88 µs | 398.58 µs | 490.42 µs | 5.83 MiB | 5.49 M |
| durable-sync-w1-c1-p8-read-99-write-1 | server_tcp_read_99_write_1_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 13.90 k | — | — | 71,963.30 | 153.00 µs | 3.34 ms | 4.70 ms | 7.13 ms | 5.87 MiB | 2.22 M |
| durable-sync-w1-c1-p8-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=1, t=1, owner-bound, p=8 | 454.10 | — | — | 2,202,170.00 | 19.96 ms | 36.95 ms | 40.06 ms | 47.94 ms | 5.89 MiB | 72.66 k |
| durable-sync-w2-c2-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 63.97 k | — | — | 15,633.30 | 505.96 µs | 1.18 ms | 1.45 ms | 1.55 ms | 8.33 MiB | 10.23 M |
| durable-sync-w2-c2-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=2, t=2, owner-bound, p=32 | 386.31 | — | — | 2,588,590.00 | 161.14 ms | 321.99 ms | 363.96 ms | 426.88 ms | 8.44 MiB | 61.81 k |
| durable-sync-w4-c4-p32-get-only | server_tcp_get_only_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 95.99 k | — | — | 10,417.80 | 620.08 µs | 1.54 ms | 2.01 ms | 2.23 ms | 12.80 MiB | 15.36 M |
| durable-sync-w4-c4-p32-read-after-write | server_tcp_read_after_write_durable-sync | k=16, v=64, w=4, t=4, owner-bound, p=32 | 602.46 | — | — | 1,659,870.00 | 217.82 ms | 444.88 ms | 517.93 ms | 596.28 ms | 9.03 MiB | 96.39 k |
| embedded-durable-group-all | store_durable_group_put | k=16, v=64, w=1, t=1, uniform | 143.86 | — | — | 6,951,040.00 | — | — | — | — | 4.19 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_get_copy | k=16, v=64, w=1, t=1, uniform | 408.00 k | — | — | 2,451.00 | — | — | — | — | 4.50 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_put_get_copy | k=16, v=64, w=1, t=1, uniform | 306.49 | — | — | 3,262,750.00 | — | — | — | — | 4.52 MiB | 0.00 |
| embedded-durable-group-all | store_durable_group_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 379.83 | — | — | 2,632,760.00 | — | — | — | — | 4.59 MiB | 0.00 |
| embedded-durable-group-parallel-put | store_durable_group_put | k=16, v=64, w=1, t=4, single-worker | 400.50 | — | — | 2,496,890.00 | 9.99 ms | 14.37 ms | 19.77 ms | 30.59 ms | 4.80 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put | k=16, v=64, w=1, t=1, uniform | 3.47 k | — | — | 287,973.00 | — | — | — | — | 3.12 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_get_copy | k=16, v=64, w=1, t=1, uniform | 726.83 k | — | — | 1,375.83 | — | — | — | — | 3.30 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_put_get_copy | k=16, v=64, w=1, t=1, uniform | 6.96 k | — | — | 143,674.00 | — | — | — | — | 3.39 MiB | 0.00 |
| embedded-durable-periodic-all | store_durable_periodic_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 7.21 k | — | — | 138,673.00 | — | — | — | — | 3.45 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put | k=16, v=64, w=1, t=1, uniform | 207.40 | — | — | 4,821,500.00 | — | — | — | — | 3.09 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_get_copy | k=16, v=64, w=1, t=1, uniform | 372.95 k | — | — | 2,681.33 | — | — | — | — | 3.30 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_put_get_copy | k=16, v=64, w=1, t=1, uniform | 383.85 | — | — | 2,605,200.00 | — | — | — | — | 3.37 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_read_after_write_copy | k=16, v=64, w=1, t=1, uniform | 402.56 | — | — | 2,484,080.00 | — | — | — | — | 3.45 MiB | 0.00 |
| embedded-durable-sync-all | store_durable_recovery_open | k=16, v=64, w=1, t=1, uniform | 186.61 k | — | — | 5,358.75 | — | — | — | — | 3.78 MiB | 0.00 |
| embedded-durable-sync-parallel-put | store_durable_put | k=16, v=64, w=4, t=4, worker-affine | 311.77 | — | — | 3,207,490.00 | 12.01 ms | 17.49 ms | 21.93 ms | 34.99 ms | 3.58 MiB | 0.00 |
| get-w1-owner-bound | store_parallel_get_copy | k=16, v=64, w=1, t=1, owner-bound | 3.67 M | — | — | 272.43 | — | — | — | — | 237.30 MiB | 0.00 |
| get-w1-uniform | store_parallel_get_copy | k=16, v=64, w=1, t=1, uniform | 3.79 M | — | — | 263.55 | — | — | — | — | 236.87 MiB | 0.00 |
| get-w1-zipf | store_parallel_get_copy | k=16, v=64, w=1, t=1, zipf | 3.77 M | — | — | 265.24 | — | — | — | — | 240.31 MiB | 0.00 |
| get-w2-owner-bound | store_parallel_get_copy | k=16, v=64, w=2, t=2, owner-bound | 8.22 M | — | — | 121.60 | — | — | — | — | 163.34 MiB | 0.00 |
| get-w2-uniform | store_parallel_get_copy | k=16, v=64, w=2, t=2, uniform | 6.04 M | — | — | 165.56 | — | — | — | — | 165.17 MiB | 0.00 |
| get-w2-zipf | store_parallel_get_copy | k=16, v=64, w=2, t=2, zipf | 6.22 M | — | — | 160.74 | — | — | — | — | 175.91 MiB | 0.00 |
| get-w4-owner-bound | store_parallel_get_copy | k=16, v=64, w=4, t=4, owner-bound | 17.32 M | — | — | 57.75 | — | — | — | — | 144.44 MiB | 0.00 |
| get-w4-uniform | store_parallel_get_copy | k=16, v=64, w=4, t=4, uniform | 10.75 M | — | — | 93.02 | — | — | — | — | 125.47 MiB | 0.00 |
| get-w4-zipf | store_parallel_get_copy | k=16, v=64, w=4, t=4, zipf | 10.30 M | — | — | 97.05 | — | — | — | — | 128.16 MiB | 0.00 |
| get-w8-owner-bound | store_parallel_get_copy | k=16, v=64, w=8, t=8, owner-bound | 23.97 M | — | — | 41.71 | — | — | — | — | 129.95 MiB | 0.00 |
| get-w8-uniform | store_parallel_get_copy | k=16, v=64, w=8, t=8, uniform | 11.60 M | — | — | 86.17 | — | — | — | — | 150.98 MiB | 0.00 |
| get-w8-zipf | store_parallel_get_copy | k=16, v=64, w=8, t=8, zipf | 11.24 M | — | — | 88.96 | — | — | — | — | 130.37 MiB | 0.00 |
| put-w1-owner-bound | store_parallel_put | k=16, v=64, w=1, t=1, owner-bound | 595.33 k | — | — | 1,679.73 | — | — | — | — | 87.98 MiB | 0.00 |
| put-w1-uniform | store_parallel_put | k=16, v=64, w=1, t=1, uniform | 605.09 k | — | — | 1,652.65 | — | — | — | — | 86.19 MiB | 0.00 |
| put-w1-zipf | store_parallel_put | k=16, v=64, w=1, t=1, zipf | 605.75 k | — | — | 1,650.84 | — | — | — | — | 90.08 MiB | 0.00 |
| put-w2-owner-bound | store_parallel_put | k=16, v=64, w=2, t=2, owner-bound | 812.01 k | — | — | 1,231.52 | — | — | — | — | 64.84 MiB | 0.00 |
| put-w2-uniform | store_parallel_put | k=16, v=64, w=2, t=2, uniform | 601.30 k | — | — | 1,663.07 | — | — | — | — | 83.23 MiB | 0.00 |
| put-w2-zipf | store_parallel_put | k=16, v=64, w=2, t=2, zipf | 462.69 k | — | — | 2,161.28 | — | — | — | — | 65.45 MiB | 0.00 |
| put-w4-owner-bound | store_parallel_put | k=16, v=64, w=4, t=4, owner-bound | 1.31 M | — | — | 761.59 | — | — | — | — | 64.02 MiB | 0.00 |
| put-w4-uniform | store_parallel_put | k=16, v=64, w=4, t=4, uniform | 558.34 k | — | — | 1,791.02 | — | — | — | — | 61.25 MiB | 0.00 |
| put-w4-zipf | store_parallel_put | k=16, v=64, w=4, t=4, zipf | 522.23 k | — | — | 1,914.88 | — | — | — | — | 58.36 MiB | 0.00 |
| put-w8-owner-bound | store_parallel_put | k=16, v=64, w=8, t=8, owner-bound | 2.08 M | — | — | 479.72 | — | — | — | — | 60.97 MiB | 0.00 |
| put-w8-uniform | store_parallel_put | k=16, v=64, w=8, t=8, uniform | 484.77 k | — | — | 2,062.81 | — | — | — | — | 83.52 MiB | 0.00 |
| put-w8-zipf | store_parallel_put | k=16, v=64, w=8, t=8, zipf | 433.42 k | — | — | 2,307.22 | — | — | — | — | 76.80 MiB | 0.00 |
| raw-w1-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=1, t=1, owner-bound | 1.16 M | — | — | 862.23 | — | — | — | — | 81.97 MiB | 0.00 |
| raw-w2-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=2, t=2, owner-bound | 1.58 M | — | — | 633.91 | — | — | — | — | 65.31 MiB | 0.00 |
| raw-w4-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=4, t=4, owner-bound | 2.51 M | — | — | 397.87 | — | — | — | — | 63.77 MiB | 0.00 |
| raw-w8-owner-bound | store_parallel_read_after_write_copy | k=16, v=64, w=8, t=8, owner-bound | 3.87 M | — | — | 258.51 | — | — | — | — | 62.17 MiB | 0.00 |
| client-api-w1-c1-p32-read-after-write-v64 | cpp_client_pipeline_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 319.55 k | — | — | 3,129.39 | 182.54 µs | 485.50 µs | 1.02 ms | 2.10 ms | 38.48 MiB | 51.13 M |
| volatile-w1-c1-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 44.38 k | — | — | 22,534.00 | 22.08 µs | 26.71 µs | 51.08 µs | 217.62 µs | 31.25 MiB | 7.10 M |
| volatile-w1-c1-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 45.63 k | — | — | 21,916.00 | 21.83 µs | 28.33 µs | 52.88 µs | 154.92 µs | 32.28 MiB | 7.30 M |
| volatile-w1-c1-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=1 | 49.84 k | — | — | 20,066.10 | 38.58 µs | 52.33 µs | 100.38 µs | 219.75 µs | 38.45 MiB | 7.97 M |
| volatile-w1-c1-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.82 M | — | — | 549.22 | 65.54 µs | 110.71 µs | 189.83 µs | 296.12 µs | 101.56 MiB | 291.32 M |
| volatile-w1-c1-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 1.36 M | — | — | 733.19 | 84.12 µs | 159.71 µs | 242.00 µs | 385.92 µs | 106.33 MiB | 218.22 M |
| volatile-w1-c1-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=128 | 438.23 k | — | — | 2,281.89 | 317.38 µs | 691.75 µs | 2.12 ms | 6.10 ms | 128.00 MiB | 70.12 M |
| volatile-w1-c1-p32-get-only-v1024 | server_tcp_get_only_volatile | k=16, v=1024, w=1, t=1, owner-bound, p=32 | 441.11 k | — | — | 2,267.01 | 61.08 µs | 91.12 µs | 174.04 µs | 314.38 µs | 151.41 MiB | 494.04 M |
| volatile-w1-c1-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 894.91 k | — | — | 1,117.43 | 34.54 µs | 37.83 µs | 41.46 µs | 52.21 µs | 98.23 MiB | 143.19 M |
| volatile-w1-c1-p32-get-only-v65536 | server_tcp_get_only_volatile | k=16, v=65536, w=1, t=1, owner-bound, p=32 | 16.31 k | — | — | 61,313.00 | 1.03 ms | 1.89 ms | 2.15 ms | 4.67 ms | 402.00 MiB | 1.07 G |
| volatile-w1-c1-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 755.83 k | — | — | 1,323.05 | 35.96 µs | 57.54 µs | 86.50 µs | 183.96 µs | 103.86 MiB | 120.93 M |
| volatile-w1-c1-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=32 | 349.77 k | — | — | 2,858.99 | 110.75 µs | 199.33 µs | 567.92 µs | 1.83 ms | 127.84 MiB | 55.96 M |
| volatile-w1-c1-p8-get-only-v262144 | server_tcp_get_only_volatile | k=16, v=262144, w=1, t=1, owner-bound, p=8 | 4.28 k | — | — | 233,826.00 | 992.17 µs | 1.68 ms | 1.89 ms | 2.76 ms | 426.88 MiB | 1.12 G |
| volatile-w1-c1-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 293.14 k | — | — | 3,411.37 | 26.12 µs | 34.71 µs | 60.42 µs | 152.92 µs | 97.06 MiB | 46.90 M |
| volatile-w1-c1-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 282.07 k | — | — | 3,545.20 | 26.29 µs | 49.21 µs | 87.54 µs | 320.75 µs | 97.78 MiB | 45.13 M |
| volatile-w1-c1-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=1, t=1, owner-bound, p=8 | 176.97 k | — | — | 5,650.57 | 69.71 µs | 146.00 µs | 243.58 µs | 810.88 µs | 125.45 MiB | 28.32 M |
| volatile-w2-c2-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 80.47 k | — | — | 12,427.10 | 24.42 µs | 45.46 µs | 70.71 µs | 158.79 µs | 37.75 MiB | 12.88 M |
| volatile-w2-c2-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 80.19 k | — | — | 12,470.80 | 24.17 µs | 32.00 µs | 51.96 µs | 123.79 µs | 31.94 MiB | 12.83 M |
| volatile-w2-c2-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=1 | 86.15 k | — | — | 11,607.40 | 46.29 µs | 82.75 µs | 122.12 µs | 220.46 µs | 38.75 MiB | 13.78 M |
| volatile-w2-c2-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 3.47 M | — | — | 288.37 | 69.12 µs | 79.67 µs | 103.79 µs | 139.29 µs | 79.50 MiB | 554.85 M |
| volatile-w2-c2-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 2.21 M | — | — | 453.46 | 74.42 µs | 152.62 µs | 227.88 µs | 344.25 µs | 85.03 MiB | 352.84 M |
| volatile-w2-c2-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=128 | 449.31 k | — | — | 2,225.62 | 576.96 µs | 1.63 ms | 3.05 ms | 6.61 ms | 115.19 MiB | 71.89 M |
| volatile-w2-c2-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.64 M | — | — | 609.16 | 37.54 µs | 43.04 µs | 46.42 µs | 53.12 µs | 79.41 MiB | 262.65 M |
| volatile-w2-c2-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 1.32 M | — | — | 758.29 | 39.21 µs | 87.21 µs | 150.08 µs | 301.04 µs | 85.22 MiB | 211.00 M |
| volatile-w2-c2-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=32 | 374.91 k | — | — | 2,667.32 | 208.50 µs | 508.38 µs | 1.00 ms | 1.99 ms | 110.78 MiB | 59.99 M |
| volatile-w2-c2-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 594.09 k | — | — | 1,683.24 | 26.42 µs | 47.21 µs | 83.79 µs | 234.46 µs | 80.08 MiB | 95.05 M |
| volatile-w2-c2-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 460.89 k | — | — | 2,169.73 | 29.62 µs | 64.50 µs | 138.88 µs | 872.29 µs | 86.44 MiB | 73.74 M |
| volatile-w2-c2-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=2, t=2, owner-bound, p=8 | 241.62 k | — | — | 4,138.71 | 93.71 µs | 225.33 µs | 368.54 µs | 800.50 µs | 117.25 MiB | 38.66 M |
| volatile-w4-c4-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 113.94 k | — | — | 8,776.78 | 33.88 µs | 78.50 µs | 155.08 µs | 578.46 µs | 32.73 MiB | 18.23 M |
| volatile-w4-c4-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 113.08 k | — | — | 8,843.26 | 32.92 µs | 63.46 µs | 129.62 µs | 458.67 µs | 33.89 MiB | 18.09 M |
| volatile-w4-c4-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=1 | 111.10 k | — | — | 9,001.12 | 71.33 µs | 109.96 µs | 190.75 µs | 503.12 µs | 42.55 MiB | 17.78 M |
| volatile-w4-c4-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 5.67 M | — | — | 176.32 | 83.58 µs | 183.58 µs | 251.88 µs | 378.04 µs | 80.70 MiB | 907.43 M |
| volatile-w4-c4-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 3.01 M | — | — | 331.80 | 90.96 µs | 204.88 µs | 402.42 µs | 1.54 ms | 81.34 MiB | 482.23 M |
| volatile-w4-c4-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=128 | 507.45 k | — | — | 1,970.64 | 1.07 ms | 2.67 ms | 7.25 ms | 30.35 ms | 115.48 MiB | 81.19 M |
| volatile-w4-c4-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 2.62 M | — | — | 381.90 | 44.04 µs | 72.88 µs | 88.67 µs | 108.04 µs | 78.64 MiB | 418.96 M |
| volatile-w4-c4-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 1.72 M | — | — | 580.53 | 51.92 µs | 123.92 µs | 195.54 µs | 333.50 µs | 84.94 MiB | 275.61 M |
| volatile-w4-c4-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=32 | 395.33 k | — | — | 2,529.51 | 356.96 µs | 804.12 µs | 1.44 ms | 5.63 ms | 115.70 MiB | 63.25 M |
| volatile-w4-c4-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 839.71 k | — | — | 1,190.88 | 36.50 µs | 82.25 µs | 167.50 µs | 402.08 µs | 80.38 MiB | 134.35 M |
| volatile-w4-c4-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 715.45 k | — | — | 1,397.72 | 36.67 µs | 62.46 µs | 77.71 µs | 97.83 µs | 79.55 MiB | 114.47 M |
| volatile-w4-c4-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=4, t=4, owner-bound, p=8 | 205.95 k | — | — | 4,855.51 | 183.04 µs | 367.33 µs | 927.50 µs | 3.53 ms | 118.83 MiB | 32.95 M |
| volatile-w8-c8-p1-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 131.42 k | — | — | 7,609.22 | 60.58 µs | 92.71 µs | 192.04 µs | 310.25 µs | 40.12 MiB | 21.03 M |
| volatile-w8-c8-p1-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 129.62 k | — | — | 7,715.06 | 61.67 µs | 108.67 µs | 199.25 µs | 386.83 µs | 41.95 MiB | 20.74 M |
| volatile-w8-c8-p1-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=1 | 167.28 k | — | — | 5,978.13 | 89.25 µs | 132.79 µs | 308.33 µs | 1.16 ms | 48.48 MiB | 26.76 M |
| volatile-w8-c8-p128-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 9.15 M | — | — | 109.25 | 104.00 µs | 167.42 µs | 1.48 ms | 3.21 ms | 87.70 MiB | 1.46 G |
| volatile-w8-c8-p128-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 4.91 M | — | — | 203.49 | 114.96 µs | 543.38 µs | 1.05 ms | 2.40 ms | 86.84 MiB | 786.30 M |
| volatile-w8-c8-p128-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=128 | 387.12 k | — | — | 2,583.15 | 2.17 ms | 6.95 ms | 22.96 ms | 63.13 ms | 121.81 MiB | 61.94 M |
| volatile-w8-c8-p32-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 3.67 M | — | — | 272.66 | 66.25 µs | 86.54 µs | 97.58 µs | 117.50 µs | 86.44 MiB | 586.81 M |
| volatile-w8-c8-p32-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 2.41 M | — | — | 415.20 | 76.21 µs | 126.21 µs | 183.00 µs | 330.04 µs | 93.33 MiB | 385.36 M |
| volatile-w8-c8-p32-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=32 | 343.10 k | — | — | 2,914.58 | 617.54 µs | 1.91 ms | 4.01 ms | 15.92 ms | 120.95 MiB | 54.90 M |
| volatile-w8-c8-p8-get-only-v64 | server_tcp_get_only_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 1.01 M | — | — | 988.99 | 61.62 µs | 79.96 µs | 89.25 µs | 103.75 µs | 99.94 MiB | 161.78 M |
| volatile-w8-c8-p8-read-99-write-1-v64 | server_tcp_read_99_write_1_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 933.77 k | — | — | 1,070.92 | 64.08 µs | 116.29 µs | 201.42 µs | 304.58 µs | 95.97 MiB | 149.40 M |
| volatile-w8-c8-p8-read-after-write-v64 | server_tcp_read_after_write_volatile | k=16, v=64, w=8, t=8, owner-bound, p=8 | 335.03 k | — | — | 2,984.80 | 221.96 µs | 496.58 µs | 1.31 ms | 3.62 ms | 124.08 MiB | 53.60 M |

## Run metadata

| Suite | Commit | Platform | Architecture | Compiler |
| --- | --- | --- | --- | --- |
| index-all-k16-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v256 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v4096 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-get-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v256 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v4096 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-batch-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v256 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v4096 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-get-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v256 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v4096 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-put-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v256 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v4096 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| store-read-after-write-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p1-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c1-p8-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch128 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch16 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch32 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w1-c4-p32-batch4 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w2-c2-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-group-w4-c4-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p1-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w1-c1-p8-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w2-c2-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-periodic-w4-c4-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p1-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-99-write-1 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w1-c1-p8-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w2-c2-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-get-only | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| durable-sync-w4-c4-p32-read-after-write | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-all | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-group-parallel-put | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-periodic-all | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-all | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| embedded-durable-sync-parallel-put | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w1-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w2-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w4-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| get-w8-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w1-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w2-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w4-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-uniform | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| put-w8-zipf | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w1-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w2-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w4-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| raw-w8-owner-bound | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| client-api-w1-c1-p32-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p1-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p128-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v1024 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-get-only-v65536 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p32-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v262144 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w1-c1-p8-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p1-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p128-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p32-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w2-c2-p8-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p1-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p128-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p32-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w4-c4-p8-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p1-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p128-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p32-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-get-only-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-99-write-1-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |
| volatile-w8-c8-p8-read-after-write-v64 | 31bd35f-dirty | macos | arm64 | Apple LLVM 21.0.0 (clang-2100.1.1.101) |

## Durable pipeline profile

Queue and service values are per-sample averages; maxima are the worst observed operation across all measured samples. Commit timing is the v1 batch publication boundary and is not available for unbatched durable-sync.

| Suite | Queue avg/max | Queue peak | Store avg/max | Commit avg/max | Batch avg/max | Pending | Closes r/b/a/d | Rejected/expired/failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| durable-group-w1-c1-p1-read-99-write-1 | 281.60 µs / 332.38 µs | 32 rec / 6656 B | 4.82 ms / 12.41 ms | 4.80 ms / 12.34 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p1-read-after-write | 296.31 µs / 10.21 ms | 1 rec / 208 B | 4.27 ms / 12.48 ms | 4.21 ms / 12.39 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-99-write-1 | 281.76 µs / 6.55 ms | 27 rec / 6656 B | 4.88 ms / 11.46 ms | 4.86 ms / 11.33 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p32-read-after-write | 318.81 µs / 10.10 ms | 1 rec / 208 B | 5.55 ms / 425.75 ms | 5.51 ms / 425.70 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-99-write-1 | 306.40 µs / 5.87 ms | 32 rec / 6656 B | 5.43 ms / 13.78 ms | 5.29 ms / 13.63 ms | 1.00 / 32.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c1-p8-read-after-write | 313.56 µs / 10.03 ms | 1 rec / 208 B | 5.00 ms / 17.40 ms | 4.95 ms / 17.27 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch1 | 1.20 ms / 13.73 ms | 4 rec / 832 B | 442.56 µs / 7.10 ms | 422.57 µs / 7.09 ms | 1.00 / 1.00 | 0 rec / 0 B | 800/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch128 | 277.18 µs / 473.42 µs | 4 rec / 832 B | 5.00 ms / 22.46 ms | 4.91 ms / 22.37 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch16 | 306.22 µs / 3.09 ms | 4 rec / 832 B | 4.88 ms / 18.70 ms | 4.76 ms / 18.57 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch32 | 285.40 µs / 2.22 ms | 4 rec / 832 B | 5.52 ms / 25.36 ms | 5.44 ms / 25.27 ms | 4.00 / 4.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w1-c4-p32-batch4 | 24.35 µs / 10.03 ms | 4 rec / 832 B | 466.34 µs / 7.89 ms | 388.71 µs / 7.82 ms | 4.00 / 4.00 | 0 rec / 0 B | 200/0/0/0 | 0/0/0 |
| durable-group-w2-c2-p32-read-after-write | 320.71 µs / 2.71 ms | 1 rec / 208 B | 10.80 ms / 446.14 ms | 10.76 ms / 446.05 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-group-w4-c4-p32-read-after-write | 316.11 µs / 770.25 µs | 1 rec / 208 B | 15.63 ms / 313.35 ms | 15.59 ms / 313.32 ms | 1.00 / 1.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-99-write-1 | 7.71 µs / 10.07 ms | 32 rec / 6656 B | 2.10 ms / 10.03 ms | 688.92 µs / 6.50 ms | 14.00 / 32.00 | 2 rec / 272 B | 0/0/0/1 | 0/0/0 |
| durable-periodic-w1-c1-p1-read-after-write | 4.92 µs / 25.42 µs | 1 rec / 208 B | 123.23 µs / 8.17 ms | 863.93 µs / 6.79 ms | 32.00 / 32.00 | 22 rec / 2992 B | 9/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-99-write-1 | 6.26 µs / 10.51 ms | 29 rec / 6656 B | 1.46 ms / 10.47 ms | — / 1.92 ms | 0.00 / 32.00 | 15 rec / 2040 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p32-read-after-write | 5.52 µs / 34.46 µs | 1 rec / 208 B | 93.60 µs / 15.58 ms | 528.88 µs / 7.05 ms | 32.00 / 32.00 | 24 rec / 3264 B | 15/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-99-write-1 | 9.62 µs / 9.15 ms | 32 rec / 6656 B | 868.75 µs / 15.91 ms | — / 7.12 ms | 0.00 / 32.00 | 17 rec / 2312 B | 0/0/0/0 | 0/0/0 |
| durable-periodic-w1-c1-p8-read-after-write | 9.31 µs / 582.50 µs | 1 rec / 208 B | 123.32 µs / 9.93 ms | 621.53 µs / 1.61 ms | 30.11 / 32.00 | 29 rec / 3944 B | 9/0/0/1 | 0/0/0 |
| durable-periodic-w2-c2-p32-read-after-write | 8.61 µs / 763.42 µs | 1 rec / 208 B | 274.39 µs / 18.22 ms | 4.48 ms / 14.71 ms | 31.60 / 32.00 | 52 rec / 7072 B | 14/0/0/1 | 0/0/0 |
| durable-periodic-w4-c4-p32-read-after-write | 53.44 µs / 2.84 ms | 1 rec / 208 B | 227.22 µs / 15.27 ms | 3.05 ms / 12.80 ms | 27.17 / 32.00 | 116 rec / 15776 B | 10/0/0/8 | 0/0/0 |
| durable-sync-w1-c1-p1-read-99-write-1 | 7.49 µs / 121.21 ms | 32 rec / 6656 B | 4.14 ms / 146.77 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p1-read-after-write | 12.40 µs / 1.50 ms | 1 rec / 208 B | 4.46 ms / 18.79 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-99-write-1 | 6.69 µs / 128.72 ms | 32 rec / 6656 B | 3.51 ms / 128.69 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p32-read-after-write | 10.70 µs / 761.58 µs | 1 rec / 208 B | 4.94 ms / 16.69 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-99-write-1 | 10.07 µs / 128.01 ms | 31 rec / 6656 B | 4.01 ms / 163.05 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w1-c1-p8-read-after-write | 10.07 µs / 1.43 ms | 1 rec / 208 B | 4.27 ms / 11.26 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w2-c2-p32-read-after-write | 11.73 µs / 659.38 µs | 1 rec / 208 B | 10.23 ms / 35.00 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
| durable-sync-w4-c4-p32-read-after-write | 13.19 µs / 187.50 µs | 1 rec / 208 B | 13.13 ms / 37.74 ms | — / — | 0.00 / 0.00 | 0 rec / 0 B | 0/0/0/0 | 0/0/0 |
