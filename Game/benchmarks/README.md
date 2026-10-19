# Network Exchange Strategy Benchmark (CSC 581)

Compares two ZeroMQ object-state exchange strategies — **Full-State** and **Delta-State**
— built on `Engine::ExchangeProtocol` (game-engine) and `ExchangeStateTracking`/
`SyntheticScene`/`NetworkPerformanceBenchmark` (this repo). See the top-of-file comment
in `Game/src/NetworkPerformanceBenchmark.cpp` for the full design rationale; this file
covers methodology, how to reproduce the results, and the machine/build environment they
were collected on.

## The two strategies

Both strategies use the *same* wire record layout (`Engine::ExchangeObjectState`: id +
position + velocity, 20 bytes, big-endian/IEEE-754 explicit encoding — see
`game-engine/Engine/src/Engine/Network/ExchangeProtocol.h`) and the *same* ZeroMQ PUB/SUB
topology. The only thing that differs is **how many records each exchange carries**:

- **Full-State**: every iteration sends every tracked object's complete state.
- **Delta-State**: every iteration sends only the objects whose state changed since the
  last exchange (`ExchangeSenderState::ComputeChanged` in `Game/src/
  ExchangeStateTracking.cpp`). The very first iteration naturally sends every object
  (nothing has a "last sent" value yet), with no special-cased branch for it.

A receiver applies a Full-State frame by replacing its entire tracked set, and a
Delta-State frame by merging the frame's records into whatever it already has
(`ExchangeReceiverState::Apply`). Correctness — that both strategies reconstruct the
exact same final object state from the exact same synthetic update sequence — is
verified by `NetworkExchangeCorrectnessTest` (in-process, no sockets) **before** any
timing result is trusted; see the main checkpoint report for its pass/fail results.

## Topology

One ZeroMQ PUB socket (publisher) binds; `client_count` SUB sockets, each on its own
thread, connect and subscribe to everything — identical topology for both strategies, so
the only independent variable is the data format/amount, not transport or routing.

## Reproducing

```bash
cd spare-parts
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target NetworkPerformanceBenchmark NetworkExchangeCorrectnessTest BenchmarkStatsTest

# Correctness first — must pass before trusting any timing below.
./build/bin/NetworkExchangeCorrectnessTest
./build/bin/BenchmarkStatsTest

# Full suite (what produced results/benchmark_raw.csv):
./build/bin/NetworkPerformanceBenchmark --experiment=all --iterations=20000 --repetitions=5 \
    --out-dir=Game/benchmarks/results

# Derive the summary CSV + plots from the raw CSV:
python3 Game/benchmarks/plot_results.py
```

`--experiment=1|2|3` runs just one experiment family; `--iterations=N`/`--repetitions=N`
override the defaults.

## Methodology

- **Iterations per run: 20,000** (within the assignment's suggested 10,000–100,000
  range). Chosen empirically: a pilot at 500 iterations already showed stable,
  low-noise timings even for the largest (500-object) condition, and 20,000 keeps the
  *entire* 100-run suite (3 experiments x conditions x 2 strategies x 5 repetitions)
  under a minute wall-clock on the reference machine below, while still being far above
  what would mostly measure timer-resolution noise.
- **Repetitions: 5** per condition/strategy, measured only (no warmup repetitions pooled
  into these).
- **Warmup policy**: before timing starts, the publisher sends a burst of sentinel
  frames (ignored by receivers, tagged with a reserved `IterationSeq`) until every
  subscriber has observed at least one, bounded by a 2s timeout, plus a fixed 50ms
  settle delay — standard ZeroMQ PUB/SUB "slow joiner" mitigation. None of this counts
  toward `elapsed_ms`.
- **Timing boundary**: wall-clock `std::chrono::steady_clock` time from the first
  real-iteration send to the moment the *slowest* of the `client_count` receivers has
  decoded and applied the *last* iteration's frame — the complete
  encode → ZeroMQ send → receive → decode → apply path, not just encoding.
- **Build mode**: Release (`-DCMAKE_BUILD_TYPE=Release`). Debug numbers would not be
  representative and were not used for any reported result.
- **Byte/message accounting**: counted at the **wire level** — `total_messages_sent` =
  `iterations * client_count` (PUB/SUB fans one logical send out to every connected
  subscriber over its own connection), `total_bytes_sent` = (sum of the publisher's
  actual `EncodeExchangeFrame()` output sizes across all iterations) `* client_count`.
  This is what actually changes as `client_count` grows (Experiment 2's whole point),
  and it is the *authoritative* ZeroMQ payload size — never `sizeof()` on a C++ struct.
- **Determinism**: no `random_device`/randomness anywhere. `SyntheticScene.cpp` builds
  every object's initial state and per-object velocity as a pure function of its index,
  so the exact same update sequence is reproduced on every run and by both strategies —
  required for the Full == Delta final-state equivalence check to mean anything.
- **Statistics**: sample mean and **sample variance** (`sum((x - mean)^2) / (n - 1)`,
  i.e. Bessel's correction, matching Python's `statistics.variance`), computed in
  `plot_results.py` straight from `benchmark_raw.csv` — never hand-typed.

## Results

- `results/benchmark_raw.csv` — one row per repetition (100 rows for the full suite).
- `results/benchmark_summary.csv` — one row per condition/strategy (20 rows), with `n`,
  `mean_elapsed_ms`, `variance_elapsed_ms2`, `stddev_elapsed_ms`, `mean_bytes_sent`,
  `mean_messages_sent`, `throughput_iterations_per_sec`, `bytes_per_iteration` — derived
  deterministically from the raw CSV by `plot_results.py`, never edited by hand.
- `results/plots/*.png` — the four report-ready graphs (object-count scaling,
  client-count scaling, scene-composition time, scene-composition bytes).
- `results/benchmark_run.log` — the console output of the exact suite invocation that
  produced the committed raw CSV (useful for spotting any retries/anomalies).

## Machine / build environment (as collected)

| | |
|---|---|
| OS | macOS (Darwin 25.6.0), arm64 |
| CPU | Apple M4, 10 cores |
| Compiler | Apple clang 17.0.0 |
| Build type | Release (`-DCMAKE_BUILD_TYPE=Release`) |
| ZeroMQ (libzmq) | 4.3.5 |
| Transport | ZeroMQ PUB/SUB over `tcp://127.0.0.1` (loopback) |

Absolute timings are specific to this machine; the *relative* Full-vs-Delta comparisons
and the qualitative scaling trends are the evidence this checkpoint is meant to produce.

## Known limitations

- All conditions ran on one machine with loopback TCP, not a real multi-host network —
  appropriate for isolating the data-format/amount variable (the assignment's own
  guidance), but loopback has near-zero latency/packet loss, so real-WAN behavior (e.g.
  packet loss interacting with PUB/SUB's drop-on-full-queue semantics) is not
  represented here.
- `client_count` is capped at 4 (matching this project's `Engine::MaxPlayers`/existing
  multiplayer demo scale), not a large-scale fan-out test.
- The benchmark's synthetic objects are not rendered/simulated through the real `Game`
  object model — they are intentionally minimal (id + position + velocity) so the
  comparison isolates the exchange strategy itself, per the assignment's own guidance
  not to drag in SDL/rendering overhead.
