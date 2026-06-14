# binance-lob-capture

C++17 Binance market-data capture and local order-book reconstruction.

The tool subscribes to Binance combined WebSocket streams for configured symbols:

- `<symbol>@depth@100ms`
- `<symbol>@depth5@100ms`
- `<symbol>@trade`

It writes two assignment deliverables:

- `market_data_*.csv`: one row per inbound logical message.
- `*_orderbook.csv`: top-5 book rows after applied depth updates and depth5 checkpoints.

Replay mode rebuilds order-book CSVs from saved capture files with no network calls.

## Full Build and Run Commands

Prerequisites:

- OS: Ubuntu 22.04 / Debian
- Compiler: GCC 12 (`g++-12`)
- Standard: C++17
- Build: CMake 3.16+, Ninja or Make
- Live dependencies: Boost.Beast/Asio, OpenSSL
- Optional fast parser: simdjson
- Build hygiene: Release build, `-Wall -Wextra -Wshadow` enabled in CMake
- `ENABLE_LIVE=AUTO` is the default: CMake builds the real Beast/OpenSSL client when dependencies are available, or the stub client otherwise. Use `-DENABLE_LIVE=ON` to require live support and `-DENABLE_LIVE=OFF` for core/replay-only sanitizer builds.
- Portability note: the fixed-point parser uses GCC/Clang `__int128` for overflow-safe scaled integer math; the submitted/tested target is Linux with GCC. MSVC would need an equivalent checked-multiply path.

Install dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
  cmake ninja-build \
  g++-12 \
  libssl-dev \
  libboost-all-dev \
  libsimdjson-dev \
  zlib1g-dev
```

Clone and build:

```bash
git clone https://github.com/devlearn12482/binance-lob-capture.git
cd binance-lob-capture
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_LIVE=ON -DENABLE_SIMDJSON=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Docker build, including tests:

```bash
docker build -t binance-lob-capture .
```

Strict warning and sanitizer builds:

```bash
# Warnings as errors
cmake -B build-werror -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_LIVE=ON -DENABLE_SIMDJSON=ON -DENABLE_WERROR=ON
cmake --build build-werror --parallel
ctest --test-dir build-werror --output-on-failure

# Address + undefined behavior sanitizers
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_LIVE=OFF -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

# Thread sanitizer, run separately from ASan
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_LIVE=OFF -DENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

TSan is best run on native Linux. Some Docker Desktop / WSL combinations fail
inside the sanitizer runtime before tests start with an "unexpected memory
mapping" message; that is an environment limitation, not a project-specific
race report.

## Run

Live capture, spot, one symbol, two minutes:

```bash
./build/binance_capture --venue spot --symbols BTCUSDT --duration 120 --output-dir ./output
```

USD-M futures:

```bash
./build/binance_capture --venue usdm --symbols BTCUSDT --duration 120 --output-dir ./output
```

Multiple symbols:

```bash
./build/binance_capture --venue spot --symbols BTCUSDT,ETHUSDT --duration 120 --output-dir ./output
```

Outputs are written to `./output/`:

```text
output/
  market_data_spot_BTCUSDT_<date>.csv
  market_data_spot_BTCUSDT_<date>_snapshots.csv
  market_data_spot_BTCUSDT_<date>_orderbook.csv
```

Verify CSV schema:

```bash
MARKET=$(ls ./output/market_data_spot_BTCUSDT_20??-??-??.csv | head -1)
BOOK=$(ls ./output/market_data_spot_BTCUSDT_20??-??-??_orderbook.csv | head -1)
head -2 "$MARKET"
head -2 "$BOOK"
awk -F',' 'NR==2{print NF}' "$BOOK"
```

Expected order-book column count: `26`.

Symbol list format:

- `--symbols BTCUSDT,ETHUSDT`: comma-separated, uppercase symbols.
- Repeated symbols are deduplicated.
- Symbols are lowercased only when building Binance stream URLs.

Docker example:

Linux/macOS:

```bash
docker build -t binance-lob-capture .
docker run --rm -v "$PWD/output:/work/output" binance-lob-capture \
  --venue spot --symbols BTCUSDT --duration 120 --output-dir /work/output
```

PowerShell:

```powershell
docker build -t binance-lob-capture .
docker run --rm -v ${PWD}\output:/work/output binance-lob-capture `
  --venue spot --symbols BTCUSDT --duration 120 --output-dir /work/output
```

The program creates the output directory if it does not already exist.

Replay with no network:

```bash
./build/binance_capture --venue spot --symbols BTCUSDT \
  --replay sample/market_data_spot_BTCUSDT_sample.csv --output-dir sample
```

## Output Files

For one symbol:

```text
market_data_<venue>_<SYMBOL>_<date>.csv
market_data_<venue>_<SYMBOL>_<date>_snapshots.csv
market_data_<venue>_<SYMBOL>_<date>_orderbook.csv
```

For multiple symbols, market-data files may use `multi` or shard tags; each order-book file is still per symbol.

Market-data CSV header:

```text
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

Order-book CSV header, exactly 26 columns:

```text
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

Quick schema check:

```bash
awk -F',' 'NR==2{print NF}' ./output/*_orderbook.csv
```

Expected result: `26`.

## Sample

`sample/` contains a real 60-second BTCUSDT spot capture:

```text
sample/market_data_spot_BTCUSDT_sample.csv
sample/market_data_spot_BTCUSDT_sample_snapshots.csv
sample/market_data_spot_BTCUSDT_sample_orderbook.csv
```

Sample summary:

- Market-data rows: 4,079
- REST snapshot sidecar rows: 1
- Order-book rows: 1,188
- Crossed order-book rows: 0
- Replay reproduces the committed order-book file from the sample inputs.

Replay check:

```bash
./build/binance_capture --venue spot --symbols BTCUSDT \
  --replay sample/market_data_spot_BTCUSDT_sample.csv --output-dir sample
```

Crossed-book check:

```powershell
$d = Import-Csv .\sample\market_data_spot_BTCUSDT_sample_orderbook.csv
($d | Where-Object { [long]$_.bid0 -gt 0 -and [long]$_.ask0 -gt 0 -and [long]$_.bid0 -ge [long]$_.ask0 }).Count
```

Expected result: `0`.

## Policies

Scaling:

- Prices and quantities are parsed from Binance decimal strings into `int64`.
- Both use fixed scale `10^8`.
- No binary floating point is used for CSV integer columns.
- Non-zero precision beyond 8 decimal places is rejected.
- Integer combine is overflow-checked.

Timestamps:

- Default policy is receive wall-clock time.
- Market rows use `recv_tsec` and `recv_tnsec`.
- Order-book rows use the same timestamp as the source market-data row.
- `--time-policy event` uses Binance event time where available and falls back to receive time for depth5.

Order-book row markers:

- `type=D`: applied differential depth update.
- `type=S`: depth5 snapshot row.
- `type=T`: trade annotation row, only with `--integrate-trades`.
- `side=N`: full two-sided top-5 snapshot.

## Stream Semantics

Venue URLs:

- Spot: `wss://stream.binance.com:9443/stream?streams=`
- USD-M: `wss://fstream.binance.com/public/stream?streams=`

Depth updates:

- Quantities are absolute values.
- Quantity `0` removes a level.
- Spot uses `U/u`.
- USD-M uses `U/u/pu`.
- Message kind is classified from the stream name, not the payload shape.

Depth5:

- Treated as exchange top-5 replace snapshots.
- Emits `type=S` order-book rows.
- Cross-checked against the diff-maintained book with a `depth5_mismatch` counter.

Trades:

- Do not modify the book by default.
- `--integrate-trades` emits annotation rows only.

Reconnects and gaps:

- `conn_epoch` increments on reconnect.
- `conn_seq` restarts at 0 per connection.
- A new epoch invalidates the symbol book and forces resync.
- Spot gap: `U > book_id + 1`.
- USD-M gap: `pu != previous u`.
- Duplicate/stale diffs are ignored.

Resync:

- Live mode fetches REST depth snapshots:
  - Spot: `api.binance.com/api/v3/depth?limit=5000`
  - USD-M: `fapi.binance.com/fapi/v1/depth?limit=1000`
- Incoming diffs are buffered while waiting for the snapshot.
- Buffered diffs with `u <= lastUpdateId` are dropped, then remaining bridged diffs are applied.
- Every buffered diff applied during snapshot bridging emits its own `type=D`
  row at that diff's original selected timestamp; resync never collapses
  multiple applied events into one output row.
- If the REST call fails, the book stays not-ready and retries on later events.
- Replay uses saved `*_snapshots.csv` sidecars when present; without a sidecar, it uses depth5 rows as deterministic top-5 checkpoints.

## Performance and Observability

JSON parsing:

- `-DENABLE_SIMDJSON=ON` uses simdjson on the depth payload parse path.
- Without simdjson, a small built-in parser walks the known Binance depth payload keys once.
- Each WebSocket frame is split once into stream name and inner `data`; message kind is then classified from the stream name.
- Depth parsing extracts sequence fields, levels, and Binance `E`/`T` event time together, so depth/depth5 messages are not scanned again just to timestamp the audit row. Trades use a separate lightweight event-time lookup because they are not book-state messages.

Allocations:

- CSV format buffers are reused between rows.
- The writer ring owns reusable string slots, so steady-state writes retain capacity instead of allocating a fresh buffer per row.
- Each shard reuses the envelope `stream`/`data` buffers instead of allocating new strings for every split.
- Each symbol reuses `DepthEvent` bid/ask vectors; parsing clears them while retaining capacity.
- The order book stores scaled integers, not decimal strings or floating-point values.
- Remaining intentional allocations are WebSocket frame materialization and hash-map node changes when new price levels are inserted.

Order-book structure:

- The full book uses hash maps keyed by scaled price, giving average `O(1)` insert/update/delete per level.
- Fixed top-64 bid/ask caches stay sorted by price; top-5 output reads from the first five cache entries.
- Common hot path for `U` price-level changes is `O(U * 64)`, which is `O(U)` because the cache size is fixed.
- Filling an output row from cached top-5 levels is `O(1)`.
- Rare fallback: deleting a visible cached level can trigger a full-side cache rebuild by scanning the hash map. This preserves correctness when the next best level is outside the fixed cache.
- Trade-off: a full ordered-map book is simpler and always sorted, but every update costs `O(log N)`. A pure fixed top-N array is faster, but cannot recover the next best level after a visible deletion. This implementation uses hash maps for full-depth correctness plus fixed top caches for the common low-latency path.

I/O and back-pressure:

- Each output file has one writer.
- CSV writes go through a single-producer/single-consumer ring buffer with 8,192 record slots by default.
- If the ring is full, the producer spins briefly, then sleeps/yields until a slot frees. It does not drop rows.
- This intentionally favors audit completeness over maximum throughput: every parsed market-data row should either reach disk or surface a write error.
- The shard thread also runs Beast's `io_context`; when it waits on a full writer ring, socket reads pause too. That lets OS/TCP receive buffers apply natural back-pressure upstream instead of letting memory grow without bound.
- `--sync-writes` is available for deterministic inline writes during debugging.

Threading:

- One WebSocket connection runs per shard.
- Each symbol belongs to exactly one shard, so a book is mutated by one thread only.
- Each shard owns its writers and books; shared state is limited to atomic metrics counters.
- Async write mode creates one market-data writer thread, one snapshot-sidecar writer thread, and one order-book writer thread per symbol owned by the shard. For `N` symbols on one shard, that is `N + 2` writer threads plus the shard/io_context thread.

Metrics:

- Counters printed on exit include messages by kind, parse errors, gaps, reconnects, resyncs, rows written, depth5 mismatches, and buffer overflows.

Limits:

- Each symbol consumes three combined streams. Configuration is rejected when
  any shard would exceed Binance's 1,024-stream per-connection limit; increase
  `--shards` to spread a larger symbol set across connections.

## Tests

```bash
cmake --build build --target unit_tests
./build/unit_tests
```

Tests cover CSV escaping, decimal scaling, parser behavior, diff semantics, stale/duplicate events, gap handling, USD-M `pu`, resync buffering, sharding, and the SPSC writer ring.

Reviewer checklist:

| Step | Command |
| --- | --- |
| Build from clean checkout | `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_LIVE=ON && cmake --build build --parallel` |
| Run tests | `ctest --test-dir build --output-on-failure` |
| Live 300s capture | `./build/binance_capture --venue spot --symbols BTCUSDT --duration 300 --output-dir ./output` |
| Column count check | `awk -F',' 'NR==2{print NF}' ./output/*_orderbook.csv` |
| No secrets in source/config | `git grep -i -E "api_key|secret|password" -- ':!README.md' ':!.gitignore' ':!.env.example'` |

## Security

- No API keys or credentials are needed.
- TLS certificate chain and hostname are verified.
- `.gitignore` excludes build outputs, generated CSVs, `.env`, and `config/secrets.json`.

## GitHub Submission

```bash
git init
git add .
git commit -m "Initial submission: Binance WebSocket capture + LOB"
git remote add origin https://github.com/devlearn12482/binance-lob-capture.git
git branch -M main
git push -u origin main
git tag -a v1.0.0 -m "Submission v1.0.0"
git push origin v1.0.0
```

See `notes.md` for a compact assignment checklist and Binance sequencing reference.
