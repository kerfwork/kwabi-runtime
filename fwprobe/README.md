# fwprobe — error-firewall probes

Diagnostics for the `error-firewall` design. Not shipped code.

`notes/error-firewall-design.md` is the conclusion; these are the measurements
it rests on. Run them before trusting any claim in that document.

## Run

```sh
cd notes/runtime-skeleton/fwprobe

make probe  PG=18      # catching, restoring, ordering, nesting
make probe2 PG=18      # the decisive one: does partial work survive?
```

`PG` accepts 16, 17 or 18 and picks the matching `pg_config` and port
(5434 / 5433 / 5432). The same source builds for all three; only `pg_config`
differs.

## What each probe answers

### `probe.sql`

| # | question | answer |
|---|---|---|
| 1 | after `PG_TRY`+`FlushErrorState`, can the backend still `palloc`? | yes |
| 2 | is the transaction still usable? | yes |
| 3 | does the subtransaction pattern work? | yes |
| 4 | can it be called repeatedly inside one transaction? | yes |
| 5 | does it work nested inside a `SAVEPOINT`? | yes |
| 6 | does the message survive without a context switch? | yes |
| 7 | does state leak across 10 calls? | no |
| 9–11 | does copy-before vs copy-after rollback matter? | both appear to work |
| 12 | backend healthy afterwards? | yes |

### `probe2.sql` — the decisive one

| # | question | answer |
|---|---|---|
| 1 | insert, raise, swallow with `PG_TRY` alone — does the write survive? | **yes — 1 row** |
| 2 | same, inside an internal subtransaction | **no — 0 rows** |
| 3 | which tags survived? | only the no-subxact one |
| 4 | both in one transaction, then `COMMIT` | no-subxact row **persists**; subxact row gone |

## Why probe 1 of `probe.sql` is a trap

It asks "does the backend survive a caught error?" — and the answer is yes, even
without a subtransaction. That looks like permission to skip the subtransaction.

It is not. `probe2` shows why: survival is not the property that matters.
**The work done before the error is not undone**, so an extension that catches
an error and continues has silently committed a partial effect while telling
its caller the attempt failed.

`probe.sql` measures whether the backend crashes. `probe2.sql` measures whether
the semantics are honest. The second is the one that decides the design.

## Measurement traps found while building these

Both produced a false positive in the first version of `fwprobe2.c`:

- **`SPI_execute(..., read_only=true, ...)` cannot see the caller's own
  uncommitted writes.** It skips `CommandCounterIncrement`. Counting with it
  reported 0 rows that were in fact present, which made a surviving write look
  rolled back. Counts here use `read_only=false`.
- **Counting all rows conflates cases.** The first version counted the whole
  table, so a row left behind by function `a` was attributed to function `b`
  in the same transaction. Rows are now tagged by source.

A third, about the ordering rule:

- **`CopyErrorData` after the rollback still returns the right message.** It
  does not crash, because `AbortSubTransaction` switches `CurrentMemoryContext`
  to `TransactionAbortContext` first. So the "switch before copying" rule cannot
  be validated by comparing output — it is about which context owns the data and
  therefore when it is freed. Both orderings return identical text on 16, 17 and
  18.

## Build note

`PG_MODULE_MAGIC` must appear exactly **once** per module. With two source files
linked into one bundle, having it in both gives:

```
duplicate symbol '_Pg_magic_func'
```

It lives in `fwprobe.c`; `fwprobe2.c` deliberately omits it.

## Cross-version results

`probe2` produces identical results on 16.15, 17.11 and 18.6:

```
no_subxact(tag=1):  caught=yes own_rows_survived=1  <-- PARTIAL WORK SURVIVED
with_subxact(tag=2): caught=yes own_rows_survived=0  (rolled back)
```

That matters more than the result itself: the firewall's contract has to be the
same on every major, or the ABI is not an ABI.
