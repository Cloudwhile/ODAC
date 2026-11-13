'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../luci-app-device-access/htdocs/luci-static/resources/odac/common.js'), 'utf8');
const id = 'a'.repeat(32);
function harness(responses, tick = 2000) {
  let now = 0;
  const calls = [];
  const common = new Function('baseclass', 'rpc', 'ui', '_', 'window', 'Date', source)(
    { extend: x => x }, { declare: ({ method }) => async params => {
      calls.push({ method, params });
      const value = responses.shift();
      if (value instanceof Error) throw value;
      return value;
    } }, {}, x => x, { setTimeout: resolve => { now += tick; resolve(); } }, { now: () => now });
  return { common, calls };
}
const ok = data => ({ ok: true, data });
test('polls pending job and returns actual result without resubmitting', async () => {
  const h = harness([ok({ job: id }), ok({ done: false }), ok({ done: true, result: ok({ gateway: '192.168.20.1' }) })]);
  assert.deepEqual(await h.common.runJob('network_update', { portal_port: '2080' }), { gateway: '192.168.20.1' });
  assert.deepEqual(h.calls.map(x => x.method), ['network_update', 'network_job_status', 'network_job_status']);
  assert.deepEqual(h.calls[1].params, { job: id });
});
test('returns the final apply error', async () => {
  const h = harness([ok({ job: id }), ok({ done: true, result: { ok: false, error: { code: 'APPLY', message: 'Rolled back' } } })]);
  await assert.rejects(h.common.runJob('upload_cert', {}), e => e.code === 'APPLY');
});
test('temporary RPC disconnection retries status only', async () => {
  const h = harness([ok({ job: id }), new Error('Disconnected'), ok({ done: true, result: ok({}) })]);
  await h.common.runJob('network_update', {});
  assert.equal(h.calls.filter(x => x.method === 'network_update').length, 1);
});
test('polling deadline leaves transaction result explicitly uncertain', async () => {
  const h = harness([ok({ job: id }), ok({ done: false })], 600000);
  await assert.rejects(h.common.runJob('network_update', {}), e => e.code === 'TIMEOUT' && e.message.includes('后台可能仍在'));
  assert.equal(h.calls.length, 2);
});
test('rejects malformed job and status responses', async () => {
  await assert.rejects(harness([ok({})]).common.runJob('network_update', {}), e => e.code === 'BAD_RESPONSE');
  await assert.rejects(harness([ok({ job: id }), ok({ done: 'true' })]).common.runJob('network_update', {}), e => e.code === 'BAD_RESPONSE');
});
