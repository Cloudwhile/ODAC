'use strict';
// Run with: node --test tests/settings_view_test.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../luci-app-device-access/htdocs/luci-static/resources/view/device-access/settings.js'), 'utf8');

function render(overrides = {}, job = async () => ({})) {
  const nodes = [], calls = [], errors = [], uploads = [], successes = [];
  function E(tag, attrs = {}, children = []) {
    const node = { tag, attrs, children: Array.isArray(children) ? children : [children], value: attrs.value || '', id: attrs.id, type: attrs.type,
      disabled: attrs.disabled === true,
      files: [], appendChild(child) { this.children.push(child); }, focus() { this.focused = true; } };
    if (tag === 'button' && attrs.click) node.activate = event => { if (!node.disabled) attrs.click(event || { currentTarget: node }); };
    nodes.push(node); return node;
  }
  let extended;
  const C = {
    el: E, header: (title, text) => E('header', {}, [title, text]), badge: x => x,
    field: (label, input) => E('label', {}, [label, input]),
    confirm: (title, text, label, danger, go) => go(),
    notify: e => errors.push(e), ok: message => successes.push(message),
    runJob: (method, params) => { calls.push({ method, params }); return job(method, params); },
    OdacError: function(code, message) { this.code = code; this.message = message; },
    act: (method, params) => calls.push({ method, params }),
    call: async (method, params) => { calls.push({ method, params }); return {}; }
  };
  const settings = { enabled: true, session_seconds: 3600, max_users: 200, max_devices: 500,
    default_devices: 3, max_sessions: 64, upload_kbps: 10000, download_kbps: 30000, max_audit: 500,
    wan_interval: 15, db_max_kib: 2048, interface: 'br-guest', gateway: '192.168.20.1',
    portal_hostname: 'portal.example.com', portal_https_port: 8443, portal_port: 2080,
    upstream: '', tls_cert: '/etc/odac/tls/cert.pem', ...overrides };
  const View = new Function('view', 'ui', 'fs', 'C', 'E', '_', 'window', 'location', source)(
    { extend: proto => { extended = proto; function ExtendedView() {} ExtendedView.prototype = proto; return ExtendedView; } },
    { uploadFile: async (target, file) => { uploads.push({ target, file }); } }, {}, C, E, x => x, { setTimeout: () => {} }, {});
  const view = new View();
  view.render({ data: settings });
  const button = text => nodes.find(n => n.tag === 'button' && n.children.includes(text));
  return { nodes, calls, errors, uploads, successes, button, input: id => nodes.find(n => n.id === id), view, extended };
}
const flush = () => new Promise(resolve => setImmediate(resolve));

test('normal save includes supported operational settings only', () => {
  const v = render();
  v.button('保存设置').attrs.click();
  assert.equal(v.calls[0].method, 'settings_update');
  assert.equal(v.calls[0].params.wan_interval, 15);
  assert.equal(v.calls[0].params.db_max_kib, 2048);
  assert.equal(v.calls[0].params.session_seconds, 3600);
  assert.equal('interface' in v.calls[0].params, false);
});

test('daemon-unavailable disables DB-backed fields but permits system setup', () => {
  const v = render({ daemon_available: false });
  assert.equal(typeof v.view.load, 'function');
  assert.equal(v.extended.handleSave, null);
  assert.equal(v.extended.handleSaveApply, null);
  assert.equal(v.extended.handleReset, null);
  const daemonInputs = v.nodes.filter(n => (n.tag === 'input' || n.tag === 'textarea') && n.type !== 'file' &&
    !(n.id || '').startsWith('odac-net-') && !n.attrs.placeholder);
  assert.ok(daemonInputs.length && daemonInputs.every(n => n.disabled));
  assert.equal(v.button('保存设置').disabled, true);
  assert.equal(v.button('保存门户文字').disabled, true);
  for (const text of ['应用门户设置', '下载备份', '恢复']) {
    assert.equal(v.button(text).disabled, true);
    v.button(text).activate();
  }
  assert.equal(v.button('保存网络设置').disabled, false);
  assert.equal(v.button('上传并保存证书').disabled, false);
  assert.equal(v.input('odac-net-interface').disabled, false);
  v.button('保存设置').activate();
  v.button('保存门户文字').activate();
  assert.equal(v.calls.length, 0);
});

test('active config prevents changing controlled interface in UI', () => {
  const v = render({ config_enabled: true });
  assert.equal(v.input('odac-net-interface').disabled, true);
  assert.equal(v.input('odac-net-gateway').disabled, false);
});

test('network save uses dedicated RPC and string values', async () => {
  const v = render();
  v.input('odac-net-portal_https_port').value = '9443';
  const b = v.button('保存网络设置');
  b.attrs.click({ currentTarget: b });
  await flush();
  assert.equal(v.errors.length, 0);
  assert.equal(v.calls[0].method, 'network_update');
  assert.equal(v.calls[0].params.interface, 'br-guest');
  assert.equal(v.calls[0].params.gateway, '192.168.20.1');
  assert.equal(v.calls[0].params.portal_https_port, '9443');
  assert.equal(v.calls[0].params.portal_port, '2080');
  assert.equal(v.calls[0].params.portal_hostname, 'portal.example.com');
  assert.equal(v.calls[0].params.upstream, '');
  assert.equal(b.disabled, false);
});

test('network save reports success only after the job completes', async () => {
  let finish;
  const v = render({}, () => new Promise(resolve => { finish = resolve; }));
  const b = v.button('保存网络设置');
  b.activate();
  await flush();
  assert.equal(b.disabled, true);
  assert.equal(v.successes.length, 0);
  finish({});
  await flush();
  assert.deepEqual(v.successes, ['网络设置已保存']);
  assert.equal(b.disabled, false);
});

test('failed transaction never reports a successful save', async () => {
  const v = render({}, async () => { throw { code: 'APPLY', message: 'Failed; rolled back' }; });
  const b = v.button('保存网络设置');
  b.activate();
  await flush();
  assert.equal(v.errors[0].code, 'APPLY');
  assert.equal(v.successes.length, 0);
  assert.equal(b.disabled, false);
});

for (const port of ['443', '2050', '8443.5', '65536', '']) test('network save rejects port ' + JSON.stringify(port), () => {
  const v = render();
  v.input('odac-net-portal_https_port').value = port;
  v.button('保存网络设置').attrs.click({ currentTarget: v.button('保存网络设置') });
  assert.equal(v.calls.length, 0);
  assert.equal(v.errors[0].code, 'INVALID');
});

test('TLS upload sends files through cgi-io and no key content in RPC', async () => {
  const v = render();
  v.input('odac-cert').files = [{ name: 'cert.pem', size: 1000 }];
  v.input('odac-key').files = [{ name: 'key.pem', size: 1000 }];
  const b = v.button('上传并保存证书');
  b.attrs.click({ currentTarget: b });
  await flush();
  assert.deepEqual(v.uploads.map(x => x.target), ['/tmp/odac-upload-cert.pem', '/tmp/odac-upload-key.pem']);
  assert.deepEqual(v.calls, [{ method: 'upload_cert', params: {} }]);
  assert.equal(b.disabled, false);
});
