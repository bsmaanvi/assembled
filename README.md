# Intraday — queue notifications for team leads

A C++20 backend with a small browser dashboard. Configure queue backlog and SLA
rules, replay observations, and see prioritized active violations, notification
history, new-alert toasts, and staffing/volume context for each queue. No SQL or
external service is required.

## Run

Requires a C++20 compiler, POSIX filesystem APIs, and pthreads (tested on Linux).
The JSON and HTTP libraries are pinned and included in `third_party/`; building
needs no network access or package-manager installation.

```sh
./build.sh
./build/intraday-server
```

The server binds only to the loopback interface on port **8080**. Open that port
in your local browser; use VS Code port forwarding when working remotely. Start
from the repository root so the `web` directory can be found.

```sh
./build/intraday-server --port 8081 --state data/another-demo.json --web web
```

The JSON state file and its `.lock` file live in `data/` by default and are ignored
by Git. Only one server process may use a state file. Existing terminal-prototype
`rules.txt` files are separate and are never overwritten or automatically migrated.
Use a new state filename to begin a fresh demonstration.

## First product walkthrough

1. Open **Notification rules**. Create a Billing backlog rule with threshold 20.
2. Create a Billing SLA-breach rule. Its target comes from observations.
3. Return to Overview and advance **Next demo event**:
   - First three steps establish baseline observations for the three queues.
   - Step 4 crosses Billing's backlog threshold: one warning.
   - Step 5 breaches Billing's SLA: a critical alert appears first.
   - Step 6 updates the ongoing violations without repeated notifications.
   - Step 7 clears backlog, leaving the SLA alert active.
   - Step 8 clears both conditions.
   - Step 9 starts new episodes and notifies again.
4. Edit the backlog threshold to 30 while Billing has 25 waiting. Its active
   alert clears with reason **Cleared by rule change**. History retains the
   original threshold and observation that caused the notification.
5. Lower the threshold to 22: a new notification is created immediately from
   the latest snapshot. Editing again to 20 keeps the ongoing episode quiet.
6. Stop and restart the backend with the same state file. Rules, latest metrics,
   active episodes, deduplication records, and history are restored.

The demo feed is explicitly synthetic, manually advanced, and persisted via its
cursor. It is not a live production feed. Priority defaults are SLA = critical,
backlog = warning. All notifications belong to the trusted demo team lead.

## JSONL replay

Import `fixtures/synthetic-queues.jsonl` into a fresh workspace after configuring
the two Billing rules. Its seven events produce **four** notifications and leave
two active violations. Re-importing the same file creates no new notifications.
The assignment's actual `events.jsonl` has not been supplied yet.

The importer accepts queue snapshots in the assignment schema, including UTC
`ts` strings in `YYYY-MM-DDTHH:MM:SSZ` format. The four contextual metrics
`agents_available`, `agents_on_call`, `volume_last_15m`, and
`volume_forecast_next_15m` are stored and displayed. They are optional,
nonnegative integer counts: omitted/null fields mean **not reported**, not zero.
Older state files still load; previously discarded values cannot be recovered.
Known agent event types are skipped and counted explicitly; other event types
are rejected. Queue events are stably sorted by timestamp. Equal-time events
retain input order. A bad batch makes no changes, including conflicts discovered
partway through evaluation. The current workspace is not implicitly reset.

## Product semantics

- Comparisons are strict: 20 tickets do not violate a threshold of 20; oldest
  wait equal to the SLA target is not a breach.
- One notification is created per continuous violation, per rule and queue.
  Further observations update the active panel without changing original history.
- An observed recovery clears only the corresponding violation. Missing events
  do not mean recovery. Freshness/stale-feed indicators are intentionally deferred.
- Creating or enabling a rule checks the latest snapshot immediately. With no
  snapshot it waits for data. Threshold-only edits retain the episode if still
  violated; edits that clear it record a configuration-change reason. Changing
  the rule type or queue ends the previous episode and evaluates the new scope.
- Disabling closes the active episode without claiming the queue recovered.
- Duplicate event IDs with identical retained snapshot fields are ignored. Reusing an ID
  with different retained fields returns a conflict. Fields outside our snapshot model are
  not part of this normalized identity. Staffing and volume counts are included.
- Events older than a queue's latest accepted observation cannot overwrite its
  metrics or clear its alerts. Their IDs are still retained for safe retries.
- The dashboard shows current available/on-call counts and distinct actual
  (last 15 minutes) / forecast (next 15 minutes) volumes. These are different
  windows, so their comparison is not a forecast-error measurement. Low
  availability does not establish that agents are offline or caused a breach.
- Active alerts show current staffing. Toasts and history include the original
  triggering snapshot's staffing/volume context, which later snapshots and
  rule edits do not rewrite. Contextual changes alone do not create new alerts.
- The frontend polls every 1.5 seconds. On initial load it shows history without
  replaying old toasts. Toast display is best-effort; persisted notification
  creation is independent of browser connectivity.

## Code map

| File | Responsibility |
| --- | --- |
| `queue_rule.hpp`, `queue_snapshot.hpp` | Rule and observation data types |
| `rule_collection.*` | In-memory configuration validation and ID assignment |
| `notification_engine.*` | Evaluation, episodes, deduplication, and per-queue ordering |
| `engine_state.cpp` | Encode and validate persisted engine state |
| `json_codec.hpp` | JSON conversion and typed input validation |
| `application.*` | Serialize mutations; propose, save, and publish complete state |
| `server.cpp` | HTTP routes, local-only serving, and writer lock |
| `web/` | Dashboard, rule editor, toasts, and polling |
| `fixtures/` | Labeled synthetic data with known outcomes |
| `main.cpp`, `rule_store.*` | Earlier terminal prototype, retained for learning/regression |

The browser displays decisions; it never evaluates rules. A mutex serializes
application reads and writes within the process. An advisory file lock rejects
another server using the same state file. No authentication or production
permissions system is implied by the demo identity.

## Persistence and failure guarantees

For each mutation, copy the current rules and engine, apply and validate the
change, serialize the whole candidate into a unique same-directory temporary
file, flush the file, and atomically rename it over the snapshot. Only after a
successful save is the candidate published in memory.

This keeps event deduplication, latest observations, active episodes, and
notification creation in the same persisted snapshot. A failed save does not
consume a rule ID or expose partial changes. A process killed after a successful
save can restore the state and safely handle event retries. Corrupt or unsupported
state stops startup and is not silently replaced.

This is a **single-process, single-writer** design. Whole-state copies and writes
are deliberately simple, not a high-throughput event store. Event IDs and history
are unbounded until the 32 MiB snapshot limit; reaching it rejects further saves.
There is no automatic retention policy. HTTP payloads are capped at 2 MiB.

The temporary file is fsynced, but the parent directory is not; we do **not**
promise complete machine/power-loss durability. There is no claim of exactly-once
external delivery or exactly-once browser toasts. Production evolution would need
retention, stronger durable storage, and partitioned processing, with a per-entity
ordering contract. A circuit breaker, MCP server, or consensus cluster does not
solve the failures addressed by this MVP.

## Tests

```sh
./test.sh
```

This builds the server, runs 14 configuration unit tests and 18 notification
unit tests, then runs the existing terminal integration suite and the HTTP/
persistence suite. Every test has a comment describing its purpose. HTTP tests
use isolated state files and cover immediate rule evaluation, edits, independent
recoveries, priority, abrupt process restart, duplicate/stale/conflicting events,
batch rollback, failed saves, writer exclusion, and corrupt-state preservation.

Optional browser acceptance checks require Python Playwright and a Chromium
installation (already available in the development environment):

```sh
python3 tests/browser_smoke.py
```

Set `CHROMIUM` to your Chromium executable if it differs from `/usr/bin/chromium`.
This test uses temporary state, operates the real forms, verifies toasts and
recovery, and checks desktop/mobile layouts. Screenshots go to
`/tmp/intraday-artifacts` by default; override with `ARTIFACT_DIR`.

## HTTP API

| Method | Endpoint | Purpose |
| --- | --- | --- |
| GET | `/api/health` | Process readiness |
| GET | `/api/state` | Queue metrics, rules, active alerts, and history |
| POST | `/api/rules` | Create a rule; immediately evaluate latest data |
| PATCH | `/api/rules/{id}` | Replace definition and/or change enabled state |
| POST | `/api/events` | Ingest one queue snapshot |
| POST | `/api/replay` | Import raw JSONL atomically |
| POST | `/api/demo/next` | Advance one synthetic scenario step |

Mutation requests require `X-Intraday-Client: dashboard`. The backend provides no
cross-origin permissions. This reduces accidental cross-site mutations; it is
not an authentication mechanism. Errors use 400 for validation, 404 for unknown
rule IDs, 409 for conflicting event IDs, and 500 for failed persistence.

## Interview framing and AI use

The product decision is a focused queue-monitoring workflow for team leads.
Agent self-reminders, supervisor summaries, long-call timers, forecast-based rules, and real
notification integrations are deferred. Composite staffing/backlog rules and
individual-agent offline tracking are also deferred; this change adds context,
not an unsupported inference about staffing causes. Demonstrate a complete configuration →
observation → alert → suppression → recovery loop before discussing extensions.

The systems-design contribution is idempotency, per-queue ordering, and atomic
snapshot recovery—not extra infrastructure. The failure tests are evidence for
those guarantees and their limits.

OpenAI Codex assisted with design discussion, implementation, and test generation.
Generated changes were checked with unit tests, actual HTTP requests, restart and
save-failure tests, and Chromium interaction tests. The candidate should review
and explain these decisions personally; these checks do not substitute for that
understanding. Third-party libraries and their licenses are listed in
`third_party/README.md`.
