"""Optional browser acceptance checks. Requires Playwright and Chromium."""
from pathlib import Path
import os
import socket
import subprocess
import tempfile
import time
import urllib.request

from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as folder:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        base = f'http://127.0.0.1:{port}'
        with open(Path(folder) / 'server.log', 'w') as log:
            server = subprocess.Popen([
                str(ROOT / 'build/intraday-server'), '--state', str(Path(folder) / 'state.json'),
                '--port', str(port), '--web', str(ROOT / 'web'),
            ], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    try:
                        urllib.request.urlopen(base + '/api/health', timeout=1).close()
                        break
                    except OSError:
                        time.sleep(.03)
                else:
                    raise RuntimeError('Server failed to start')
                with sync_playwright() as playwright:
                    browser = playwright.chromium.launch(
                        executable_path=os.environ.get('CHROMIUM', '/usr/bin/chromium'),
                        headless=True, args=['--no-sandbox'],
                    )
                    page = browser.new_page(viewport={'width': 1440, 'height': 1000})
                    errors = []
                    page.on('pageerror', lambda error: errors.append(str(error)))
                    page.goto(base)
                    expect(page.locator('#connection')).to_have_text('Backend connected')
                    expect(page.locator('#queue-rows .queue-metric-row')).to_have_count(3)

                    # Configure both rules through the actual form.
                    page.get_by_role('link', name='Configure rules').click()
                    page.locator('#threshold').fill('20')
                    page.get_by_role('button', name='Save rule', exact=True).click()
                    expect(page.locator('.rule-card')).to_have_count(1)
                    page.locator('#rule-type').select_option('sla')
                    expect(page.locator('#threshold-field')).to_be_hidden()
                    page.get_by_role('button', name='Save rule', exact=True).click()
                    expect(page.locator('.rule-card')).to_have_count(2)
                    page.get_by_role('link', name='Back to overview').click()

                    # Advance the explicit synthetic feed through each episode.
                    for expected_cursor in range(1, 6):
                        page.get_by_role('button', name='Next demo event').click()
                        page.wait_for_function('(cursor) => window.fetch("/api/state").then(r=>r.json()).then(s=>s.demo_cursor===cursor)', arg=expected_cursor)
                        expect(page.get_by_role('button', name='Next demo event')).to_be_enabled()
                    expect(page.locator('#attention-count')).to_have_text('2')
                    expect(page.locator('.alert-item').first).to_contain_text('SLA breach')
                    expect(page.locator('#history .history-row')).to_have_count(2)
                    expect(page.locator('.toast')).to_have_count(2)
                    # Queue metrics and active alerts show current context, while
                    # toasts and history retain the original triggering snapshot.
                    billing_context = page.locator('.queue-context').first
                    expect(billing_context).to_contain_text('Available agents4')
                    expect(billing_context).to_contain_text('Agents on calls12')
                    expect(billing_context).to_contain_text('Volume · last 15m78')
                    expect(billing_context).to_contain_text('Forecast · next 15m50')
                    expect(page.locator('.alert-item').first).to_contain_text('Available agents: 4')
                    expect(page.locator('#history')).to_contain_text('Forecast (next 15m): 50')
                    expect(page.locator('.toast').last).to_contain_text('Agents on calls: 12')

                    # Capture the dashboard at desktop and mobile dimensions.
                    artifact_dir = Path(os.environ.get('ARTIFACT_DIR', '/tmp/intraday-artifacts'))
                    artifact_dir.mkdir(parents=True, exist_ok=True)
                    page.screenshot(path=str(artifact_dir / 'dashboard-desktop.png'), full_page=True)
                    page.set_viewport_size({'width': 390, 'height': 844})
                    expect(page.locator('#queue-rows')).to_be_visible()
                    assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth'), 'Mobile layout overflows'
                    page.screenshot(path=str(artifact_dir / 'dashboard-mobile.png'), full_page=True)
                    page.set_viewport_size({'width': 1440, 'height': 1000})

                    # Repeated violation stays quiet; independent recoveries clear the panel.
                    page.get_by_role('button', name='Next demo event').click()
                    expect(page.get_by_role('button', name='Next demo event')).to_be_enabled()
                    expect(page.locator('#history .history-row')).to_have_count(2)
                    page.get_by_role('button', name='Next demo event').click()
                    expect(page.locator('#attention-count')).to_have_text('1')
                    page.get_by_role('button', name='Next demo event').click()
                    expect(page.locator('#attention-count')).to_have_text('0')
                    page.get_by_role('button', name='Next demo event').click()
                    expect(page.locator('#attention-count')).to_have_text('2')
                    expect(page.locator('#history .history-row')).to_have_count(4)

                    # Raise backlog threshold: clear only that alert, with no extra notification.
                    page.get_by_role('link', name='Configure rules').click()
                    page.locator('.rule-card').first.get_by_role('button', name='Edit', exact=True).click()
                    page.locator('#threshold').fill('30')
                    page.get_by_role('button', name='Save changes', exact=True).click()
                    expect(page.locator('.rule-card').first).to_contain_text('More than 30')
                    page.get_by_role('link', name='Back to overview').click()
                    expect(page.locator('#attention-count')).to_have_text('1')
                    expect(page.locator('#history .history-row')).to_have_count(4)
                    expect(page.locator('#history')).to_contain_text('Cleared by rule change')

                    # Refresh retains backend data and does not toast the old history again.
                    page.reload()
                    expect(page.locator('#connection')).to_have_text('Backend connected')
                    expect(page.locator('#history .history-row')).to_have_count(4)
                    expect(page.locator('.toast')).to_have_count(0)
                    assert not errors, errors
                    browser.close()
                    print('PASS: browser rule forms, priority, toasts, episodes, editing, reload, desktop/mobile layout')
                    print(f'Screenshots: {artifact_dir}')
            finally:
                server.kill()
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
