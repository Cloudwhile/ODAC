'use strict';
// Run with: node --test tests/portal_request_test.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../device-access/files/www/odac/portal.js'), 'utf8');
// Expose the real request implementation without starting the portal DOM.
const requestSource = source.slice(0, source.indexOf('  function set(')) + 'globalThis.request = req; })();';
function client(fetch) {
  let deadline;
  const timers = new Set();
  const context = {
    AbortController, fetch,
    setTimeout(fn, delay) { assert.equal(delay, 30000); deadline = fn; timers.add(fn); return fn; },
    clearTimeout(fn) { timers.delete(fn); }
  };
  vm.runInNewContext(requestSource, context);
  return { request: context.request, timers, expire() { assert.ok(timers.has(deadline)); deadline(); } };
}
const aborted = () => Object.assign(new Error('aborted'), { name: 'AbortError' });

test('deadline cancels a request waiting for response headers', async () => {
  const c = client((url, options) => new Promise((resolve, reject) => {
    options.signal.addEventListener('abort', () => reject(aborted()));
  }));
  const pending = c.request('status');
  c.expire();
  await assert.rejects(pending, e => e.code === 'TIMEOUT');
  assert.equal(c.timers.size, 0);
});

test('deadline remains active while the response body stalls', async () => {
  let bodyStarted;
  const started = new Promise(resolve => { bodyStarted = resolve; });
  const c = client(async (url, options) => ({
    status: 200,
    json: () => new Promise((resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(aborted()));
      bodyStarted();
    })
  }));
  const pending = c.request('status');
  await started;
  c.expire();
  await assert.rejects(pending, e => e.code === 'TIMEOUT');
  assert.equal(c.timers.size, 0);
});

test('GET status request uses same-origin credentials and no-store cache', async () => {
  const c = client(async (url, options) => {
    assert.equal(url, '/cgi-bin/odac?action=status');
    assert.equal(options.method, 'GET');
    assert.equal(options.credentials, 'same-origin');
    assert.equal(options.cache, 'no-store');
    assert.equal(options.headers.Accept, 'application/json');
    assert.equal(Object.keys(options.headers).length, 1);
    assert.equal('body' in options, false);
    assert.equal(typeof options.signal.aborted, 'boolean');
    assert.equal(typeof options.signal.addEventListener, 'function');
    return { status: 200, json: async () => ({ ok: true, data: { csrf: 'fresh' } }) };
  });
  assert.deepEqual(await c.request('status'), { csrf: 'fresh' });
  assert.equal(c.timers.size, 0);
});

test('success preserves POST options and clears deadline', async () => {
  const c = client(async (url, options) => {
    assert.equal(url, '/cgi-bin/odac?action=login');
    assert.equal(options.method, 'POST');
    assert.equal(options.credentials, 'same-origin');
    assert.equal(JSON.parse(options.body).csrf, 'test-token');
    return { json: async () => ({ ok: true, data: { authenticated: true } }) };
  });
  assert.equal((await c.request('login', { csrf: 'test-token' })).authenticated, true);
  assert.equal(c.timers.size, 0);
});

for (const [label, fetch, code] of [
  ['network failure', async () => { throw new TypeError('offline'); }, 'NETWORK'],
  ['invalid JSON', async () => ({ status: 502, json: async () => { throw new SyntaxError('invalid JSON'); } }), 'BAD_RESPONSE'],
  ['backend error', async () => ({ json: async () => ({ ok: false, error: { code: 'CSRF' } }) }), 'CSRF']
]) test(label + ' preserves classification and clears deadline', async () => {
  const c = client(fetch);
  await assert.rejects(c.request('status'), e => e.code === code);
  assert.equal(c.timers.size, 0);
});
