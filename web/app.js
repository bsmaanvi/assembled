'use strict';

const $ = id => document.getElementById(id);
let names = {};
let catalogSignature = "";
const statusLabels = { firing: 'Active violation', clear: 'Clear', disabled: 'Disabled', waiting_for_data: 'Waiting for data' };
let currentState = null;
let lastNotification = null;
let refreshVersion = 0;
let busy = false;

function element(tag, className = '', text = '') {
  const node = document.createElement(tag);
  node.className = className;
  node.textContent = text;
  return node;
}
function badge(text, style = 'neutral') { return element('span', `badge ${style}`, text); }
function duration(seconds) {
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60), remainder = seconds % 60;
  return `${minutes}m${remainder ? ` ${remainder}s` : ''}`;
}
function metric(value) { return value == null ? 'Not reported' : String(value); }
function staffingText(snapshot) {
  return `Available agents: ${metric(snapshot?.agents_available)} · Agents on calls: ${metric(snapshot?.agents_on_call)}`;
}
function volumeText(snapshot) {
  return `Volume (last 15m): ${metric(snapshot?.volume_last_15m)} · Forecast (next 15m): ${metric(snapshot?.volume_forecast_next_15m)}`;
}
function timeLabel(seconds) {
  const date = new Date(seconds * 1000);
  return `${date.toISOString().slice(0, 10)} · ${date.toISOString().slice(11, 19)} UTC`;
}
function showFeedback(message, error = false) {
  $('feedback').textContent = message;
  $('feedback').classList.toggle('error', error);
  $('feedback').hidden = false;
}
async function api(path, method = 'GET', data = undefined, raw = false) {
  const options = { method, headers: { 'X-Intraday-Client': 'dashboard' } };
  if (data !== undefined) {
    options.headers['Content-Type'] = raw ? 'application/x-ndjson' : 'application/json';
    options.body = raw ? data : JSON.stringify(data);
  }
  const response = await fetch(path, { ...options, signal: AbortSignal.timeout(10000) });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || `Request failed (${response.status})`);
  return body;
}
function emptyState(title, detail) {
  const node = element('div', 'empty-state');
  node.append(element('span', 'empty-icon', '◇'), element('strong', '', title), element('span', '', detail));
  return node;
}
function ruleText(rule) {
  return rule.type === 'backlog' ? `More than ${rule.threshold} tickets waiting` : 'Oldest wait exceeds the queue SLA target';
}
function renderCatalog(state) {
  names = Object.fromEntries(state.queues.map(queue => [queue.id, queue.name]));
  const signature = JSON.stringify(names);
  $('queue-count').textContent = `${state.queues.length} queues`;
  if (signature === catalogSignature) return;
  const selected = $('queue-select').value;
  $('queue-select').replaceChildren();
  for (const queue of state.queues) {
    const option = element('option', '', queue.name);
    option.value = queue.id;
    $('queue-select').append(option);
  }
  if (state.queues.some(queue => queue.id === selected)) $('queue-select').value = selected;
  catalogSignature = signature;
}
function renderQueues(state) {
  $('queue-rows').replaceChildren();
  const observed = state.queues.filter(queue => queue.snapshot);
  $('stat-waiting').textContent = observed.length ? observed.reduce((sum, queue) => sum + queue.snapshot.tickets_waiting, 0) : '—';
  $('stat-sla').textContent = observed.length ? observed.filter(queue => queue.snapshot.longest_wait_sec > queue.snapshot.sla_target_sec).length : '—';
  $('stat-alerts').textContent = state.active_alerts.length;
  const critical = state.active_alerts.filter(alert => alert.severity === 'critical').length;
  $('stat-alerts-detail').textContent = state.active_alerts.length ? `${critical} critical · ${state.active_alerts.length - critical} warning` : 'No active violations';
  $('stat-rules').textContent = state.rules.filter(rule => rule.enabled).length;
  for (const queue of state.queues) {
    const row = element('tr', 'queue-metric-row'), title = element('td'), name = element('div', 'queue-name');
    name.append(element('span', 'queue-icon', queue.name.slice(0, 1).toUpperCase()), document.createTextNode(queue.name));
    title.append(name, element('small', 'observation', queue.snapshot ? timeLabel(queue.snapshot.observed_at_sec) : 'Waiting for first observation'));
    row.append(title);
    const snapshot = queue.snapshot;
    row.append(element('td', '', snapshot ? String(snapshot.tickets_waiting) : '—'));
    row.append(element('td', '', snapshot ? duration(snapshot.longest_wait_sec) : '—'));
    row.append(element('td', '', snapshot ? duration(snapshot.sla_target_sec) : '—'));
    const status = element('td');
    status.append(!snapshot ? badge('No data') : snapshot.longest_wait_sec > snapshot.sla_target_sec ? badge('SLA breached', 'critical') : badge('Within SLA', 'clear'));
    row.append(status);
    $('queue-rows').append(row);
    const contextRow = element('tr', 'queue-context-row'), cell = element('td');
    cell.colSpan = 5;
    const context = element('div', 'queue-context');
    for (const [label, value] of [
      ['Available agents', snapshot?.agents_available], ['Agents on calls', snapshot?.agents_on_call],
      ['Volume · last 15m', snapshot?.volume_last_15m], ['Forecast · next 15m', snapshot?.volume_forecast_next_15m],
    ]) {
      const field = element('div');
      field.append(element('small', '', label), element('strong', '', metric(value)));
      context.append(field);
    }
    cell.append(context); contextRow.append(cell); $('queue-rows').append(contextRow);
  }
}
function renderAlerts(state) {
  $('attention-count').textContent = state.active_alerts.length;
  $('active-alerts').replaceChildren();
  if (!state.active_alerts.length) {
    $('active-alerts').append(emptyState('Nothing needs attention', state.rules.some(rule => rule.enabled) ? 'No observed violations of your enabled rules.' : 'Configure a rule to start monitoring.'));
    return;
  }
  const groups = new Map();
  for (const alert of state.active_alerts) {
    if (!groups.has(alert.queue_id)) groups.set(alert.queue_id, []);
    groups.get(alert.queue_id).push(alert);
  }
  for (const [queue, alerts] of groups) {
    const group = element('article', 'alert-group'), heading = element('div', 'alert-group-title');
    heading.append(element('span', '', names[queue]), badge(alerts[0].severity === 'critical' ? 'Critical' : 'Warning', alerts[0].severity));
    group.append(heading);
    for (const alert of alerts) {
      const item = element('div', 'alert-item');
      item.append(badge(alert.type === 'sla' ? 'SLA breach' : 'Backlog', alert.severity));
      item.append(element('p', '', alert.type === 'sla' ? `Oldest wait ${duration(alert.observed_value)} / target ${duration(alert.threshold)}` : `${alert.observed_value} waiting / threshold ${alert.threshold}`));
      item.append(element('p', 'notification-context', staffingText(alert.context)));
      item.append(element('small', '', `Rule #${alert.id} · ${timeLabel(alert.observed_at_sec)}`));
      group.append(item);
    }
    $('active-alerts').append(group);
  }
}
function renderHistory(state) {
  $('history').replaceChildren();
  $('history-count').textContent = `${state.notifications.length} notifications`;
  if (!state.notifications.length) {
    $('history').append(emptyState('Your notification history starts here', 'New violations will appear here, even after they resolve.'));
    return;
  }
  const reason = { recovered: 'Recovered', rule_changed: 'Cleared by rule change', rule_disabled: 'Rule disabled', rule_removed: 'Rule removed' };
  for (const notification of state.notifications.slice(-30).reverse()) {
    const row = element('article', 'history-row'), body = element('div', 'history-body'), meta = element('div', 'history-meta');
    body.append(element('strong', '', `${names[notification.queue_id]} · ${notification.severity === 'critical' ? 'SLA breach' : 'Queue backlog'}`));
    body.append(element('p', '', notification.explanation));
    body.append(element('p', 'notification-context', staffingText(notification.context)));
    body.append(element('p', 'notification-context', volumeText(notification.context)));
    body.append(element('small', '', `Rule #${notification.rule_id} · ${timeLabel(notification.observed_at_sec)} · ${notification.trigger === 'rule_change' ? 'Triggered on rule save' : 'Triggered by event'} · ${notification.event_id}`));
    meta.append(badge(notification.close_reason ? reason[notification.close_reason] || notification.close_reason : 'Active', notification.close_reason ? 'neutral' : notification.severity));
    meta.append(element('small', '', `Notification #${notification.id}`));
    row.append(element('span', 'history-symbol', notification.severity === 'critical' ? '!' : '+'), body, meta);
    $('history').append(row);
  }
  if (state.notifications.length > 30) $('history').append(element('div', 'panel-foot', 'Showing the latest 30 notifications. Other retained history is available from the backend; explicit cleanup may remove older entries.'));
}
function renderRules(state) {
  $('rule-list').replaceChildren();
  $('rules-count').textContent = `${state.rules.length} rules`;
  $('nav-rule-count').textContent = state.rules.length;
  if (!state.rules.length) {
    $('rule-list').append(emptyState('A quieter day starts with a rule', 'Choose a queue and a condition in the form.'));
    return;
  }
  for (const rule of state.rules) {
    const card = element('article', 'rule-card'), heading = element('div', 'rule-card-heading'), footer = element('div', 'rule-card-footer'), actions = element('div', 'rule-card-actions');
    heading.append(element('span', '', `${names[rule.queue_id]} · ${rule.type === 'sla' ? 'SLA breach' : 'Backlog'}`));
    heading.append(badge(statusLabels[rule.status], rule.status === 'firing' ? rule.type === 'sla' ? 'critical' : 'warning' : rule.status === 'clear' ? 'clear' : 'neutral'));
    card.append(heading, element('p', '', ruleText(rule)));
    const edit = element('button', 'button secondary', 'Edit');
    edit.type = 'button'; edit.dataset.editId = rule.id;
    const toggle = element('button', 'button secondary', rule.enabled ? 'Disable' : 'Enable');
    toggle.type = 'button'; toggle.dataset.toggleId = rule.id;
    actions.append(edit, toggle);
    footer.append(element('small', '', `Rule #${rule.id} · ${rule.type === 'sla' ? 'Critical' : 'Warning'}`), actions);
    card.append(footer); $('rule-list').append(card);
  }
}
function toast(notification) {
  const node = element('div', `toast ${notification.severity}`), close = element('button', '', '×');
  close.type = 'button'; close.setAttribute('aria-label', 'Dismiss notification toast');
  close.addEventListener('click', () => node.remove());
  node.append(element('strong', '', `${names[notification.queue_id]} · ${notification.severity === 'critical' ? 'Critical' : 'Warning'}`), element('p', '', notification.explanation), close);
  node.append(element('p', 'notification-context', staffingText(notification.context)));
  $('toasts').append(node);
  while ($('toasts').children.length > 3) $('toasts').firstElementChild.remove();
  setTimeout(() => node.remove(), 9000);
}
async function refresh() {
  const version = ++refreshVersion;
  try {
    const state = await api('/api/state');
    if (version !== refreshVersion) return;
    currentState = state;
    renderCatalog(state); renderQueues(state); renderAlerts(state); renderHistory(state); renderRules(state); updatePreview();
    const maximum = state.notifications.at(-1)?.id || 0;
    // Initial load does not replay old notifications as new toasts.
    if (lastNotification !== null && maximum >= lastNotification) {
      for (const notification of state.notifications.filter(item => item.id > lastNotification).slice(-3)) toast(notification);
    }
    lastNotification = maximum;
    $('connection').textContent = 'Backend connected'; $('connection').className = 'connection online';
  } catch (error) {
    $('connection').textContent = 'Disconnected · retrying'; $('connection').className = 'connection offline';
    if (!currentState) showFeedback(`Cannot reach the backend: ${error.message}`, true);
  }
}
function updatePreview() {
  const sla = $('rule-type').value === 'sla', threshold = $('threshold').value;
  $('threshold-field').hidden = sla; $('threshold').required = !sla; $('sla-context').hidden = !sla;
  const snapshot = currentState?.queues.find(queue => queue.id === $('queue-select').value)?.snapshot;
  $('sla-context').textContent = snapshot ? `Latest queue SLA target: ${duration(snapshot.sla_target_sec)}. This rule follows the feed's target.` : 'The SLA target comes from queue observations. Waiting for the first snapshot.';
  const queueName = names[$('queue-select').value] || 'the selected queue';
  $('rule-preview').textContent = sla ? `Notify me when ${queueName}'s oldest ticket wait exceeds its SLA target.` : `Notify me when ${queueName} has more than ${threshold || '…'} tickets waiting.`;
}
function resetForm() {
  $('rule-form').reset(); $('rule-id').value = ''; $('form-title').textContent = 'Create a rule';
  $('save-rule').textContent = 'Save rule'; $('cancel-edit').hidden = true; updatePreview();
}
async function runAction(action) {
  if (busy || !currentState) return;
  busy = true;
  document.querySelectorAll('button').forEach(button => { button.disabled = true; });
  try { await action(); await refresh(); }
  catch (error) { showFeedback(error.message, true); }
  finally {
    busy = false;
    document.querySelectorAll('button').forEach(button => { button.disabled = false; });
    $('import-file').value = '';
  }
}
function route() {
  const rules = location.hash === '#rules';
  $('dashboard-page').hidden = rules; $('rules-page').hidden = !rules;
  $('nav-dashboard').classList.toggle('active', !rules); $('nav-rules').classList.toggle('active', rules);
  $('nav-dashboard').setAttribute('aria-current', rules ? 'false' : 'page');
  $('nav-rules').setAttribute('aria-current', rules ? 'page' : 'false');
  $('breadcrumb').textContent = rules ? 'Notification rules' : 'Overview';
}
$('queue-form').addEventListener('submit', event => {
  event.preventDefault();
  const body = { id: $('new-queue-id').value.trim(), name: $('new-queue-name').value.trim() };
  runAction(async () => {
    await api('/api/queues', 'POST', body);
    await refresh();
    $('queue-select').value = body.id;
    $('queue-form').reset(); updatePreview();
    showFeedback(`Queue ${body.name} added. It will wait for its first observation.`);
  });
});
$('retention-form').addEventListener('submit', event => {
  event.preventDefault();
  if (!window.confirm('Remove older unprotected history and event IDs? Active alerts and their evidence will be retained.')) return;
  runAction(async () => {
    const result = await api('/api/maintenance/compact', 'POST', {
      history_limit: Number($('history-limit').value), event_limit: Number($('event-limit').value),
    });
    showFeedback(`Cleanup saved. ${result.retained_notifications} notifications retained, including all active alerts.`);
  });
});
$('rule-form').addEventListener('submit', event => {
  event.preventDefault();
  const body = { queue_id: $('queue-select').value, type: $('rule-type').value, enabled: $('rule-enabled').checked };
  if (body.type === 'backlog') body.threshold = Number($('threshold').value);
  const id = $('rule-id').value;
  runAction(async () => {
    await api(id ? `/api/rules/${id}` : '/api/rules', id ? 'PATCH' : 'POST', body);
    showFeedback(id ? 'Rule updated. Active alerts were checked against the new settings.' : 'Rule saved. We checked the latest queue observation.');
    resetForm();
  });
});
$('rule-list').addEventListener('click', event => {
  const button = event.target.closest('button');
  if (!button || busy) return;
  if (button.dataset.editId) {
    const rule = currentState.rules.find(item => item.id === Number(button.dataset.editId));
    $('rule-id').value = rule.id; $('queue-select').value = rule.queue_id;
    $('rule-type').value = rule.type; $('threshold').value = rule.threshold ?? 20;
    $('rule-enabled').checked = rule.enabled; $('form-title').textContent = `Edit rule #${rule.id}`;
    $('save-rule').textContent = 'Save changes'; $('cancel-edit').hidden = false; updatePreview();
    $('queue-select').focus();
  } else if (button.dataset.toggleId) {
    const rule = currentState.rules.find(item => item.id === Number(button.dataset.toggleId));
    runAction(async () => {
      await api(`/api/rules/${rule.id}`, 'PATCH', { enabled: !rule.enabled });
      showFeedback(`Rule #${rule.id} ${rule.enabled ? 'disabled' : 'enabled'}.`);
    });
  }
});
$('cancel-edit').addEventListener('click', resetForm);
for (const id of ['queue-select', 'rule-type', 'threshold']) $(id).addEventListener('input', updatePreview);
$('next-demo').addEventListener('click', () => runAction(async () => {
  const result = await api('/api/demo/next', 'POST', {});
  $('demo-label').textContent = `Synthetic demo: ${result.label}`;
  showFeedback(`${result.label}. ${result.new_notifications.length} new notification(s). Results depend on your enabled rules.`);
}));
$('import-button').addEventListener('click', () => { if (!busy) $('import-file').click(); });
$('import-file').addEventListener('change', event => {
  const file = event.target.files[0];
  if (!file) return;
  if (file.size > 2 * 1024 * 1024) { showFeedback('Please choose a JSONL file smaller than 2 MiB.', true); return; }
  runAction(async () => {
    const result = await api('/api/replay', 'POST', await file.text(), true);
    $('demo-label').textContent = `Imported ${file.name}`;
    showFeedback(`Import: ${result.processed} processed, ${result.duplicates} duplicate, ${result.stale} older, ${result.skipped_agent_events} agent events skipped. ${result.notifications_created} notifications created. Import is applied to the current workspace.`);
  });
});
window.addEventListener('hashchange', route);
route(); updatePreview(); refresh();
setInterval(refresh, 1500);
