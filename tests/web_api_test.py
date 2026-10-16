"""Real HTTP/persistence tests; every test uses its own temporary state file."""
import json
import os
from datetime import datetime, timezone
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/intraday-server'


class WebApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'state.json'
        self.process = None
        self.start()
        self.addCleanup(self.stop)

    def start(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.base = f'http://127.0.0.1:{self.port}'
        self.log = open(Path(self.temp.name) / 'server.log', 'a')
        self.process = subprocess.Popen([
            str(BINARY), '--port', str(self.port), '--state', str(self.path),
            '--web', str(ROOT / 'web'),
        ], stdout=self.log, stderr=self.log)
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.close()
                self.fail((Path(self.temp.name) / 'server.log').read_text())
            try:
                self.request('/api/health')
                return
            except (OSError, urllib.error.URLError):
                time.sleep(.03)
        self.fail('Server did not start')

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        if hasattr(self, 'log'):
            self.log.close()

    def request(self, path, method='GET', body=None, status=200, raw=False, header=True):
        data = None if body is None else (body.encode() if raw else json.dumps(body).encode())
        headers = {'Content-Type': 'application/x-ndjson' if raw else 'application/json'}
        if header:
            headers['X-Intraday-Client'] = 'dashboard'
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            response = urllib.request.urlopen(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            payload = response.read().decode()
            self.assertEqual(response.status, status, payload)
            return json.loads(payload)

    def view(self):
        return self.request('/api/state')

    def rule(self, threshold=20, kind='backlog', queue='billing'):
        return self.request('/api/rules', 'POST', {
            'type': kind, 'queue_id': queue, 'threshold': threshold,
        }, status=201)['id']

    def event(self, id, time=100, waiting=23, oldest=0, queue='billing', **kwargs):
        return self.request('/api/events', 'POST', {
            'type': 'queue_snapshot', 'event_id': id, 'observed_at_sec': time,
            'queue_id': queue, 'tickets_waiting': waiting,
            'longest_wait_sec': oldest, 'sla_target_sec': 120,
        }, **kwargs)

    # A new rule immediately evaluates cached data, and later snapshots stay quiet.
    def test_new_rule_evaluates_latest_snapshot(self):
        self.event('first')
        id = self.rule()
        view = self.view()
        self.assertEqual(len(view['active_alerts']), 1)
        self.assertEqual(view['notifications'][0]['trigger'], 'rule_change')
        self.assertEqual(view['notifications'][0]['rule_id'], id)
        self.assertEqual(self.event('next', 101)['new_notifications'], [])

    # Without a snapshot, creation saves the rule without inventing queue data.
    def test_waiting_for_data(self):
        self.rule()
        view = self.view()
        self.assertEqual(view['rules'][0]['status'], 'waiting_for_data')
        self.assertEqual(view['notifications'], [])
        self.assertIsNone(view['queues'][0]['snapshot'])

    # Still-violating edits preserve an episode; equality clears it, preserving evidence.
    def test_threshold_edits_preserve_or_clear(self):
        id = self.rule()
        self.event('first')
        original = self.view()['notifications'][0]
        for threshold in (22, 15):
            self.request(f'/api/rules/{id}', 'PATCH', {'type': 'backlog', 'queue_id': 'billing', 'threshold': threshold})
            self.assertEqual(len(self.view()['notifications']), 1)
            self.assertEqual(self.view()['active_alerts'][0]['threshold'], threshold)
        self.request(f'/api/rules/{id}', 'PATCH', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 23})
        self.assertEqual(self.view()['active_alerts'], [])
        history = self.view()['notifications'][0]
        self.assertEqual(history['threshold'], original['threshold'])
        self.assertEqual(history['close_reason'], 'rule_changed')
        self.request(f'/api/rules/{id}', 'PATCH', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 20})
        self.assertEqual(len(self.view()['notifications']), 2)

    # Backlog recovery cannot resolve a simultaneous SLA violation, which sorts first.
    def test_priority_and_independent_recovery(self):
        self.rule()
        self.rule(kind='sla')
        self.event('both', oldest=150)
        self.assertEqual([a['severity'] for a in self.view()['active_alerts']], ['critical', 'warning'])
        self.event('backlog-clear', time=101, waiting=10, oldest=150)
        view = self.view()
        self.assertEqual(len(view['active_alerts']), 1)
        self.assertEqual(view['active_alerts'][0]['type'], 'sla')
        self.assertEqual(view['notifications'][0]['close_reason'], 'recovered')

    # Disable clears the panel; enabling rechecks cached data, while enabling twice is quiet.
    def test_disable_and_reenable(self):
        id = self.rule()
        self.event('first')
        self.request(f'/api/rules/{id}', 'PATCH', {'enabled': False})
        self.assertEqual(self.view()['active_alerts'], [])
        self.assertEqual(self.view()['notifications'][0]['close_reason'], 'rule_disabled')
        self.request(f'/api/rules/{id}', 'PATCH', {'enabled': True})
        self.request(f'/api/rules/{id}', 'PATCH', {'enabled': True})
        self.assertEqual(len(self.view()['notifications']), 2)

    # Abrupt process termination preserves acknowledged state, deduplication, and episodes.
    def test_restart_preserves_state_and_suppression(self):
        self.rule()
        self.event('first')
        before = self.view()
        self.stop()
        self.start()
        self.assertEqual(self.view(), before)
        self.assertEqual(self.event('first')['outcome'], 'duplicate')
        self.assertEqual(self.event('next', 101)['new_notifications'], [])
        self.assertEqual(self.event('old-clear', 90, waiting=0)['outcome'], 'stale')
        self.stop()
        self.start()
        self.assertEqual(self.event('old-clear', 90, waiting=0)['outcome'], 'duplicate')
        self.event('clear', 102, waiting=0)
        self.assertEqual(len(self.event('again', 103)['new_notifications']), 1)

    # A conflicting retry or invalid request must not change any application state.
    def test_invalid_requests_and_conflicting_ids(self):
        self.rule()
        self.event('first')
        before = self.view()
        self.event('first', waiting=0, status=409)
        self.event('invalid', waiting=-1, status=400)
        self.request('/api/rules', 'POST', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 1.5}, status=400)
        self.request('/api/rules', 'POST', {'type': 'backlog', 'queue_id': 'other', 'threshold': 20}, status=400)
        self.request('/api/rules/999', 'PATCH', {'enabled': False}, status=404)
        self.assertEqual(self.view(), before)
        self.assertEqual(self.rule(kind='sla'), 2)

    # JSONL import sorts queue events, skips agent types explicitly, and retries safely.
    def test_jsonl_replay(self):
        self.rule()
        self.rule(kind='sla')
        data = (ROOT / 'fixtures/synthetic-queues.jsonl').read_text()
        lines = data.splitlines()
        agent = json.dumps({'type': 'adherence_check'})
        result = self.request('/api/replay', 'POST', '\n'.join(reversed(lines)) + '\n' + agent, raw=True)
        self.assertEqual(result['notifications_created'], 4)
        self.assertEqual(result['processed'], 7)
        self.assertEqual(result['skipped_agent_events'], 1)
        result = self.request('/api/replay', 'POST', data, raw=True)
        self.assertEqual(result['duplicates'], 7)
        self.assertEqual(result['notifications_created'], 0)

    # Invalid replay batches roll back even when an earlier line would have alerted.
    def test_replay_is_all_or_nothing(self):
        self.rule()
        before = self.view()
        line = (ROOT / 'fixtures/synthetic-queues.jsonl').read_text().splitlines()[1]
        self.request('/api/replay', 'POST', line + '\n{bad', raw=True, status=400)
        self.assertEqual(self.view(), before)
        conflict = json.loads(line)
        conflict['tickets_waiting'] = 99
        self.request('/api/replay', 'POST', line + '\n' + json.dumps(conflict), raw=True, status=409)
        self.assertEqual(self.view(), before)

    # A failed final rename leaves in-memory state and the proposed ID unchanged.
    def test_save_failure_does_not_publish_candidate(self):
        self.rule()
        before = self.view()
        payload = self.path.read_bytes()
        self.path.unlink()
        self.path.mkdir()
        (self.path / 'preserved').write_bytes(payload)
        self.request('/api/rules', 'POST', {'type': 'sla', 'queue_id': 'vip'}, status=500)
        self.assertEqual(self.view(), before)
        self.assertEqual(list(Path(self.temp.name).glob('state.json.tmp.*')), [])
        (self.path / 'preserved').unlink()
        self.path.rmdir()
        self.path.write_bytes(payload)
        self.assertEqual(self.rule(kind='sla'), 2)

    # Missing client headers are rejected so another website cannot mutate the demo.
    def test_mutation_requires_client_header(self):
        self.request('/api/demo/next', 'POST', {}, header=False, status=403)
        self.assertEqual(self.view()['demo_cursor'], 0)

    # A second writer exits rather than risk overwriting the first server's state.
    def test_second_writer_is_rejected(self):
        result = subprocess.run([
            str(BINARY), '--state', str(self.path), '--port', str(self.port),
            '--web', str(ROOT / 'web'),
        ], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('already using this state file', result.stderr)

    # A corrupted or unsupported state file is never silently reset on startup.
    def test_corrupt_state_is_not_overwritten(self):
        self.stop()
        for content in ('{bad', '{"schema_version":999,"rules":[]}', '{"schema_version":1,"rules":[]}'):
            with self.subTest(content=content):
                self.path.write_text(content)
                result = subprocess.run([str(BINARY), '--state', str(self.path), '--web', str(ROOT / 'web')], capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.path.read_text(), content)


    # Staffing and volume survive ingestion/restart; notifications retain original context.
    def test_queue_context_and_notification_evidence(self):
        self.rule()
        first = {'type': 'queue_snapshot', 'event_id': 'context-1', 'observed_at_sec': 100,
                 'queue_id': 'billing', 'tickets_waiting': 23, 'longest_wait_sec': 90,
                 'sla_target_sec': 120, 'agents_available': 4, 'agents_on_call': 12,
                 'volume_last_15m': 78, 'volume_forecast_next_15m': 50}
        response = self.request('/api/events', 'POST', first)
        self.assertEqual(response['new_notifications'][0]['context']['agents_available'], 4)
        second = {**first, 'event_id': 'context-2', 'observed_at_sec': 101,
                  'agents_available': 0, 'agents_on_call': 16, 'volume_last_15m': 90,
                  'volume_forecast_next_15m': 70}
        self.request('/api/events', 'POST', second)
        view = self.view()
        self.assertEqual(view['queues'][0]['snapshot']['agents_available'], 0)
        self.assertEqual(view['queues'][0]['snapshot']['volume_forecast_next_15m'], 70)
        self.assertEqual(view['active_alerts'][0]['context']['agents_on_call'], 16)
        self.assertEqual(len(view['notifications']), 1)
        self.assertEqual(view['notifications'][0]['context']['agents_on_call'], 12)
        self.assertEqual(view['notifications'][0]['context']['volume_last_15m'], 78)
        self.stop()
        self.start()
        self.assertEqual(self.view(), view)
        self.assertEqual(self.request('/api/events', 'POST', second)['outcome'], 'duplicate')
        self.request('/api/events', 'POST', {**second, 'agents_available': 1}, status=409)
        self.assertEqual(self.view(), view)

    # Optional metrics must be nonnegative integers; rejection must not consume event IDs.
    def test_invalid_context_is_rejected(self):
        before = self.view()
        event = {'type': 'queue_snapshot', 'event_id': 'context', 'observed_at_sec': 100,
                 'queue_id': 'billing', 'tickets_waiting': 23, 'longest_wait_sec': 90,
                 'sla_target_sec': 120}
        for field in ('agents_available', 'agents_on_call', 'volume_last_15m', 'volume_forecast_next_15m'):
            for invalid in (-1, 1.5, '4', True, 2147483648):
                with self.subTest(field=field, value=invalid):
                    self.request('/api/events', 'POST', {**event, field: invalid}, status=400)
                    self.assertEqual(self.view(), before)
        self.assertEqual(self.request('/api/events', 'POST', event)['outcome'], 'processed')
        self.assertIsNone(self.view()['queues'][0]['snapshot']['agents_available'])

    # Older snapshot files without context fields still restore without inventing zeros.
    def test_legacy_state_restores_missing_context(self):
        self.rule()
        self.event('legacy')
        self.stop()
        saved = json.loads(self.path.read_text())
        fields = ('agents_available', 'agents_on_call', 'volume_last_15m', 'volume_forecast_next_15m')
        for collection in ('events', 'latest'):
            for event in saved['engine'][collection]:
                for field in fields:
                    event.pop(field, None)
        for notification in saved['engine']['notifications']:
            notification.pop('context', None)
        self.path.write_text(json.dumps(saved))
        self.start()
        view = self.view()
        for field in fields:
            self.assertIsNone(view['queues'][0]['snapshot'][field])
            self.assertIsNone(view['notifications'][0]['context'][field])
        self.assertEqual(self.event('legacy')['outcome'], 'duplicate')

    # Persisted notification context must match its source event, not silently drift.
    def test_corrupt_notification_context_is_rejected(self):
        self.rule()
        self.event('first')
        self.stop()
        saved = json.loads(self.path.read_text())
        saved['engine']['notifications'][0]['context']['agents_available'] = 99
        content = json.dumps(saved)
        self.path.write_text(content)
        result = subprocess.run([str(BINARY), '--state', str(self.path), '--web', str(ROOT / 'web')],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('context disagrees', result.stderr)
        self.assertEqual(self.path.read_text(), content)

    # Duplicate create/edit attempts identify the existing rule without changing state.
    def test_duplicate_rules_are_rejected(self):
        id = self.rule()
        self.request(f'/api/rules/{id}', 'PATCH', {'enabled': False})
        before = self.view()
        conflict = self.request('/api/rules', 'POST', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 20}, status=409)
        self.assertEqual(conflict['existing_rule_id'], id)
        self.assertEqual(self.view(), before)
        second = self.rule(threshold=30)
        self.request(f'/api/rules/{second}', 'PATCH', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 20}, status=409)
        self.assertEqual(self.view()['rules'][1]['threshold'], 30)
        self.assertEqual(second, 2)
        self.request(f'/api/rules/{id}', 'PATCH', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 20})

    # Register a queue, configure it, process an event, and restore all of it after restart.
    def test_custom_queue_lifecycle(self):
        self.event('unknown-first', queue='refunds', status=400)
        self.request('/api/queues', 'POST', {'id': 'refunds', 'name': 'Refunds'}, status=201)
        self.assertEqual(len(self.view()['queues']), 4)
        id = self.rule(queue='refunds')
        self.assertEqual(self.view()['rules'][0]['status'], 'waiting_for_data')
        self.event('unknown-first', queue='refunds')
        before = self.view()
        self.assertEqual(before['active_alerts'][0]['id'], id)
        self.assertEqual(before['queues'][-1]['snapshot']['tickets_waiting'], 23)
        self.stop()
        self.start()
        self.assertEqual(self.view(), before)
        self.assertEqual(self.event('unknown-first', queue='refunds')['outcome'], 'duplicate')

    # Invalid/duplicate catalog entries cannot mutate or replace existing queues.
    def test_queue_validation(self):
        before = self.view()
        for id, name in [('Bad ID', 'Name'), ('valid', ''), ('valid', '   '), ('a'*65, 'Name'), ('x', 'bad\nname')]:
            self.request('/api/queues', 'POST', {'id': id, 'name': name}, status=400)
        self.request('/api/queues', 'POST', {'id': 'billing', 'name': 'Replacement'}, status=409)
        self.assertEqual(self.view(), before)

    # Schema-1 state upgrades on save while preserving old duplicates and observations.
    def test_schema_one_migration_preserves_rules(self):
        self.rule()
        self.stop()
        saved = json.loads(self.path.read_text())
        saved['schema_version'] = 1
        saved.pop('queues')
        saved['engine'].pop('next_notification_id', None)
        duplicate = {**saved['rules'][0], 'id': 2}
        saved['rules'].append(duplicate)
        self.path.write_text(json.dumps(saved))
        self.start()
        self.assertEqual(len(self.view()['rules']), 2)
        self.request('/api/rules', 'POST', {'type': 'backlog', 'queue_id': 'billing', 'threshold': 20}, status=409)
        self.request('/api/queues', 'POST', {'id': 'refunds', 'name': 'Refunds'}, status=201)
        self.assertEqual(json.loads(self.path.read_text())['schema_version'], 2)
        self.assertEqual(len(self.view()['rules']), 2)

    # Sparse notification IDs, active context, and deduplication survive cleanup/restart.
    def test_retention_compaction_preserves_active_state(self):
        self.rule()
        self.event('open-1', time=1)
        self.event('clear-1', time=2, waiting=0)
        self.event('open-2', time=3)
        self.event('clear-2', time=4, waiting=0)
        self.event('open-3', time=5)
        self.request('/api/maintenance/compact', 'POST', {'history_limit': 0, 'event_limit': 0})
        view = self.view()
        self.assertEqual([n['id'] for n in view['notifications']], [3])
        self.assertEqual(view['active_alerts'][0]['notification_id'], 3)
        self.stop()
        self.start()
        self.assertEqual(self.view(), view)
        self.assertEqual(self.event('open-3', time=5)['outcome'], 'duplicate')
        self.assertEqual(self.event('open-1', time=1)['outcome'], 'stale')
        self.event('clear-3', time=6, waiting=0)
        self.assertEqual(self.view()['notifications'][0]['close_reason'], 'recovered')
        self.event('open-4', time=7)
        self.assertEqual(self.view()['notifications'][-1]['id'], 4)
        self.assertEqual(self.view()['active_alerts'][0]['notification_id'], 4)

    # Cleanup retains all equal-watermark events, preventing replay from undoing tie order.
    def test_retention_protects_equal_timestamp_events(self):
        self.rule()
        self.event('first-clear', time=100, waiting=0)
        self.event('second-open', time=100, waiting=23)
        self.request('/api/maintenance/compact', 'POST', {'history_limit': 0, 'event_limit': 0})
        self.assertEqual(self.event('first-clear', time=100, waiting=0)['outcome'], 'duplicate')
        self.assertEqual(len(self.view()['active_alerts']), 1)

    # Invalid retention targets are rejected before any record can be removed.
    def test_invalid_retention_request_is_atomic(self):
        self.rule()
        self.event('first')
        before = self.view()
        self.request('/api/maintenance/compact', 'POST', {'history_limit': -1}, status=400)
        self.request('/api/maintenance/compact', 'POST', {'event_limit': 1.5}, status=400)
        self.assertEqual(self.view(), before)

    def supplied_events(self):
        path = os.environ.get('INTRADAY_EVENTS_FILE')
        if not path:
            self.skipTest('Set INTRADAY_EVENTS_FILE to the supplied assignment events.jsonl')
        text = Path(path).read_text()
        rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        self.assertEqual(len(rows), 96, 'This regression expects the reviewed assignment fixture')
        return text, [event for event in rows if event['type'] == 'queue_snapshot']

    def reference_episodes(self, events, rules):
        # Independent reference: small Python state machine, no C++ engine calls.
        active, history, latest = {}, [], {}
        for event in sorted(events, key=lambda event: event['ts']):
            latest[event['queue_id']] = event
            for rule in rules:
                if rule['queue_id'] != event['queue_id']:
                    continue
                value = event['tickets_waiting'] if rule['type'] == 'backlog' else event['longest_wait_sec']
                threshold = 20 if rule['type'] == 'backlog' else event['sla_target_sec']
                id = rule['id']
                if value > threshold and id not in active:
                    record = {'rule_id': id, 'event_id': event['event_id'], 'queue_id': event['queue_id'],
                              'observed_value': value, 'threshold': threshold, 'close_reason': ''}
                    history.append(record)
                    active[id] = record
                elif value <= threshold and id in active:
                    active.pop(id)['close_reason'] = 'recovered'
        return history, latest, active

    # The supplied feed yields two recovered Billing episodes with independently checked evidence.
    def test_supplied_assignment_billing_rules(self):
        text, events = self.supplied_events()
        rules = [dict(id=self.rule(), type='backlog', queue_id='billing'),
                 dict(id=self.rule(kind='sla'), type='sla', queue_id='billing')]
        expected, latest, active = self.reference_episodes(events, rules)
        self.assertEqual(len(expected), 2)
        result = self.request('/api/replay', 'POST', text, raw=True)
        self.assertEqual(result, {'processed': 24, 'duplicates': 0, 'stale': 0,
                                 'skipped_agent_events': 72, 'notifications_created': 2})
        view = self.view()
        for actual, wanted in zip(view['notifications'], expected):
            for key, value in wanted.items():
                self.assertEqual(actual[key], value)
        self.assertEqual(len(view['notifications']), len(expected))
        self.assertEqual(active, {})
        self.assertEqual(view['active_alerts'], [])
        self.assertEqual(view['notifications'][0]['event_id'], 'evt_01HXYZ039')
        self.assertEqual(view['notifications'][1]['event_id'], 'evt_01HXYZ049')
        for queue in view['queues']:
            snapshot = queue['snapshot']
            original = latest[queue['id']]
            for field in ('tickets_waiting', 'longest_wait_sec', 'sla_target_sec', 'agents_available',
                          'agents_on_call', 'volume_last_15m', 'volume_forecast_next_15m'):
                self.assertEqual(snapshot[field], original.get(field))
        self.stop()
        self.start()
        self.assertEqual(self.view(), view)
        retry = self.request('/api/replay', 'POST', text, raw=True)
        self.assertEqual(retry['duplicates'], 24)
        self.assertEqual(retry['notifications_created'], 0)
        self.assertEqual(self.view(), view)

    # All-queue replay produces three recovered episodes; pruning/restart never re-alerts old data.
    def test_supplied_assignment_all_queues_and_retention(self):
        text, events = self.supplied_events()
        rules = []
        for queue in ('billing', 'tier_2', 'vip'):
            for kind in ('backlog', 'sla'):
                rules.append(dict(id=self.rule(kind=kind, queue=queue), type=kind, queue_id=queue))
        expected, _, _ = self.reference_episodes(events, rules)
        self.assertEqual(len(expected), 3)
        result = self.request('/api/replay', 'POST', text, raw=True)
        self.assertEqual(result['notifications_created'], 3)
        for actual, wanted in zip(self.view()['notifications'], expected):
            for key, value in wanted.items():
                self.assertEqual(actual[key], value)
        self.request('/api/maintenance/compact', 'POST', {'history_limit': 1, 'event_limit': 0})
        before = self.view()
        self.assertEqual([notification['id'] for notification in before['notifications']], [3])
        self.stop()
        self.start()
        self.assertEqual(self.view(), before)
        retry = self.request('/api/replay', 'POST', text, raw=True)
        self.assertEqual(retry['notifications_created'], 0)
        self.assertEqual(retry['processed'], 0)
        self.assertEqual(retry['duplicates'] + retry['stale'], 24)
        self.assertEqual(self.view(), before)

    # In original arrival order the late VIP snapshot is stale; batch import instead sorts it.
    def test_supplied_assignment_original_arrival_order(self):
        _, events = self.supplied_events()
        self.rule()
        self.rule(kind='sla')
        outcomes = []
        for event in events:
            outcomes.append(self.request('/api/events', 'POST', event)['outcome'])
        self.assertEqual(outcomes.count('processed'), 23)
        self.assertEqual(outcomes.count('stale'), 1)
        view = self.view()
        self.assertEqual(len(view['notifications']), 2)
        self.assertEqual(view['active_alerts'], [])
        self.assertEqual(view['queues'][2]['snapshot']['event_id'], 'evt_01HXYZ093')


if __name__ == '__main__':
    if not BINARY.exists():
        raise SystemExit('Run ./build.sh first')
    unittest.main(verbosity=2)
