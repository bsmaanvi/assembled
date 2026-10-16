# Synthetic queue scenario

`synthetic-queues.jsonl` is hand-written test/demo data, not the assignment attachment.
With Billing backlog >20 and SLA rules configured before import, it creates
four notifications: initial backlog, initial SLA breach, then both again after
recovery. Re-importing the same events creates no notifications. The replay
uses input timestamps and stable chronological sorting, not wall-clock delays.

Use a fresh state file for this scenario. Importing earlier observations after
a later demo correctly reports them as stale. The UI always imports into the
current workspace; it does not reset existing state.

Staffing and volume counts are synthetic as well. They provide dashboard and
notification context without adding new trigger conditions. Forecast volume
covers the next 15 minutes, not the actual-volume observation window.
