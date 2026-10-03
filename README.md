<!--
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Orchestrator Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# orchestrator-translator

The **coordination layer** of the GNU AI stack for GNU/Hurd: it
starts, drives, collects and merges N instances of
[neuron-translator](https://github.com/gnu-ai/neuron-translator),
and archives every run in PostgreSQL through
[data-base-translator](https://github.com/gnu-ai/data-base-translator).
It computes nothing itself: the scheduler distributes, the
aggregator decides (PLAN.md section 1). The user dialogues with
the stack through
[inference-translator](https://github.com/gnu-ai/inference-translator),
which reads the trio JSON this translator serves.

License: GPLv3+ — Language: C23, POSIX.1-2008, Hurd `trivfs`.
Project and roadmap: [PLAN.md](PLAN.md); frozen contracts as
implemented: [SPEC.md](SPEC.md).

## Build

```console
$ ./autogen.sh            # autoreconf, once
$ mkdir build && cd build && ../configure
$ make
$ make check
```

No external dependency: the core is C23/POSIX and compiles
everywhere; the Hurd server face links the trivfs libraries only
on GNU/Hurd.

## Mount (GNU/Hurd)

```console
# settrans -a /orchestrate orchestrator-translator \
      --storage db:/db
# echo '{"instances": 3, "topologies": ["2,4,1", "2,8,1"], \
          "input": [0.5, 0.3], "aggregate": "majority"}' | tee /orchestrate
# cat /orchestrate
{"run_id": 1, "state": "done", "aggregate_strategy": "majority",
 "output": "[1,1]", "confidence": 0.667}
```

The scheduler mounts the instances
(`settrans -a /llm<N> /hurd/sigmoid-neuron-translator` by
default, `--neuron` to override), writes each one its topology
line then its input line, reads the state text back, persists
every instance row through `/db` as soon as it is collected, and
the aggregator merges the outputs (majority vote or uniform mean).

## Commands

One JSON line per `write`, one response line served by `read`
(one cursor per reader — the same protocol as `/db`):

- a task descriptor, bare or under `"run"` — executes a run
  (synchronous in phase 1);
- `{"command": "status"}` — the trio JSON type 2;
- `{"command": "result"}` — the trio JSON type 3.

Contract violations are answers (`{"invalid": "<reason>"}`), only
the transport fails as `EIO`. The complete contract — descriptor
fields and bounds, wire formats to the instances, status and
result shapes, confidence — is [SPEC.md](SPEC.md).

## Verification mode (any POSIX system)

Without Hurd, the same binary runs the same engine over standard
streams, with pluggable transports for the tests:

```console
$ echo '{"instances": 2, "topologies": ["2,4,1"], \
          "input": [0.5, 0.3], "aggregate": "mean"}' \
    | src/orchestrator-translator --instances dir:inst --storage mem
```

- `--instances node:/llm` (default) or `dir:<path>`: a directory
  where a file that behaves like `/llm<N>` is enough
  (commands append to `llm<N>.in`, the state text is read from
  `llm<N>.out`);
- `--storage db:/db` (default), `dbexec:<binary>` (the
  db-translator verification binary spawned as a child, same
  line protocol — used by the acceptance test), or `mem` (unit
  tests only, never production).

## Tests

`make check` runs:

- `test_contract` — descriptor and command parsing, the
  orchestrator-to-neuron wire formats, majority/mean aggregation,
  confidence (unit level);
- `test_engine` — a full run of the real scheduler and aggregator
  over simulated instances, with the mem twin of `/db` asserting
  the persisted rows;
- `test_cli.sh` — GNU `--version`/`--help` conformance;
- `test_orchestrate.sh` — the protocol end to end over the
  verification binary;
- `test_pg.sh` — **the phase 1 acceptance scenario**: a run with
  2 instances then a run with 8 instances, persisted through the
  real data-base-translator into a disposable PostgreSQL cluster,
  every row read back from SQL (`runs`, `run_instances`, inputs
  and outputs as jsonb, topologies in order). Skips (77) when the
  PostgreSQL tools or a built db-translator are not available.

## Status

- Phase 0 — contracts, `SPEC.md`, storage layer
  (`storage_write`/`storage_read`): **done**.
- Phase 1 — MVP: scheduler, aggregator (majority/mean),
  `/orchestrate` translator, persistence from the first
  execution, trio JSON status/result: **done**.
- Phase 2 — supervisor and parallelism: not started.
- Phase 3 — network acquisition (httpfs, inference): not started.
