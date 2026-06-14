# Assignment and Binance Notes

## Assignment checklist

- Capture Binance combined streams for each configured symbol:
  `<symbol>@depth@100ms`, `<symbol>@depth5@100ms`, and `<symbol>@trade`.
- Write market-data rows with the exact assignment header:
  `recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json`.
- Write order-book rows with the exact 26-column assignment header.
- Use deterministic integer scaling for price and quantity columns; this project
  uses scale `10^8`.
- Maintain local books from diff updates using Binance sequencing rules and emit
  rows after applied diffs and depth5 snapshots (`type=S`). Trades are annotation-only when
  `--integrate-trades` is enabled.
- Support replay without network calls from captured CSV artifacts.

## Binance stream references

- Spot combined streams base:
  `wss://stream.binance.com:9443/stream?streams=`
- USD-M public combined streams base:
  `wss://fstream.binance.com/public/stream?streams=`
- Spot diff depth uses `U` and `u`; stale events with `u <= lastUpdateId` are
  dropped after a REST snapshot, and gaps are detected when `U > book_id + 1`.
- USD-M diff depth includes `pu`; after the first bridged event, every event's
  `pu` must equal the previous event's `u`.
- REST snapshot resync buffers incoming diffs, fetches a depth snapshot, drops
  buffered events with `u <= lastUpdateId`, and applies the remaining bridged
  events in order, emitting one row for every applied buffered diff.
