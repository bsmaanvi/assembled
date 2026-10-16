# Intraday — queue notifications for team leads

A C++20 backend with a small browser dashboard and a configurable queue catalog. Configure queue backlog and SLA
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
by Git. Only one server process may use a state file. Old terminal-prototype `rules.txt` files are not read or overwritten. The web
server is now the only application entry point.
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
backlog = warning. Use **Add a queue** on the rules page to register another
queue ID and display name. New queues wait for observations; registration does
not create tickets or an upstream integration. All notifications belong to the trusted demo team lead.

## Product semantics

- Equivalent definitions (queue, rule type, and backlog threshold) cannot be
  created or edited into duplicates. HTTP 409 identifies the existing rule,
  even if it is disabled. Existing duplicates in old state are preserved rather
  than silently deleted. A no-op edit of a rule is allowed.
- Queue IDs are explicitly registered in a shared, persisted catalog. Unknown
  queues are rejected in events and rule definitions. Queue IDs are stable,
  lowercase identifiers; display names can be changed in a future extension.
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
| `queue_catalog.hpp` | Shared queue registration and validation |
| `rule_collection.*` | Configuration validation, duplicate prevention, ID and queue indexes |
| `notification_engine.*` | Indexed evaluation, episodes, deduplication, retention, and per-queue ordering |
| `lifecycle.hpp`, `errors.hpp` | Typed lifecycle values and domain error categories |
| `engine_state.cpp` | Encode and validate persisted engine state |
| `json_codec.hpp` | JSON conversion and typed input validation |
| `application.*` | Serialize mutations; propose, save, and publish complete state |
| `server.cpp` | HTTP routes, local-only serving, and writer lock |
| `web/` | Dashboard, rule editor, toasts, and polling |
| `fixtures/` | Labeled synthetic data with known outcomes |

The browser displays decisions; it never evaluates rules. A mutex serializes
application reads and writes within the process. An advisory file lock rejects
another server using the same state file. No authentication or production
permissions system is implied by the demo identity.


## Tests

```sh
./test.sh
```

This builds the server, runs 17 configuration unit tests and 20 notification
unit tests, then runs 24 HTTP/persistence tests. Three additional regressions
use the supplied assignment file when configured; otherwise they are explicitly skipped. Every test has a comment describing its purpose. HTTP tests
use isolated state files and cover immediate rule evaluation, edits, independent
recoveries, priority, abrupt process restart, duplicate/stale/conflicting events,
batch rollback, failed saves, writer exclusion, and corrupt-state preservation, schema migration, queue creation, duplicate
configuration prevention, and retention with stable IDs.

To include the supplied assignment feed without committing it to the repository:

```sh
INTRADAY_EVENTS_FILE=/path/to/events.jsonl bash test.sh
```

## HTTP API

| Method | Endpoint | Purpose |
| --- | --- | --- |
| GET | `/api/health` | Process readiness |
| GET | `/api/state` | Queue metrics, rules, active alerts, and history |
| POST | `/api/queues` | Explicitly register a queue ID and display name |
| POST | `/api/rules` | Create a rule; immediately evaluate latest data |
| PATCH | `/api/rules/{id}` | Replace definition and/or change enabled state |
| POST | `/api/events` | Ingest one queue snapshot |
| POST | `/api/replay` | Import raw JSONL atomically |
| POST | `/api/demo/next` | Advance one synthetic scenario step |
| POST | `/api/maintenance/compact` | Explicitly prune unprotected historical state |
