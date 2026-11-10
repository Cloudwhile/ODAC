'use strict';
'require baseclass';
'require rpc';
'require ui';

/*
 * Shared helpers for ODAC LuCI views.
 * Every backend call goes through rpcd object "odac"; each method takes one
 * argument "params" (object) and returns {ok:true,data} or
 * {ok:false,error:{code,message}}. Backend values are always inserted as text
 * nodes (txt()), never as HTML.
 */

var METHODS = [ 'overview', 'users', 'registrations', 'devices', 'wan', 'audit', 'settings',
	'user_update', 'approve', 'reject', 'device_update', 'disconnect', 'settings_update',
	'backup', 'restore' ];

var raw = {};
METHODS.forEach(function(m) {
	raw[m] = rpc.declare({ object: 'odac', method: m, params: [ 'params' ] });
});

function OdacError(code, message) {
	this.code = code || 'UNKNOWN';
	this.message = message || _('后端未返回错误信息');
}
OdacError.prototype = Object.create(Error.prototype);

function call(method, params) {
	return raw[method](params || {}).then(function(res) {
		if (res && res.ok === true)
			return res.data;
		if (res && res.ok === false && res.error)
			throw new OdacError(res.error.code, res.error.message);
		throw new OdacError('BAD_RESPONSE', _('后端响应格式无效'));
	}, function(err) {
		throw new OdacError('RPC', (err && err.message) ? err.message : String(err));
	});
}

/* Safe wrapper for page load: resolves to {data} or {error}. */
function load(method, params) {
	return call(method, params).then(function(d) { return { data: d }; },
		function(e) { return { error: e }; });
}

function txt(v) {
	return document.createTextNode((v === null || v === undefined || v === '') ? '—' : String(v));
}

function el(tag, attrs, children) {
	var n = E(tag, attrs || {});
	(Array.isArray(children) ? children : (children == null ? [] : [ children ])).forEach(function(c) {
		if (c == null || c === false) return;
		n.appendChild((c instanceof Node) ? c : txt(c));
	});
	return n;
}

function fmtTime(ts) {
	if (ts === null || ts === undefined || ts === '' || ts === 0) return '—';
	var d = new Date(typeof ts === 'number' ? ts * 1000 : ts);
	if (isNaN(d.getTime())) return String(ts);
	function p(x) { return (x < 10 ? '0' : '') + x; }
	return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
		p(d.getHours()) + ':' + p(d.getMinutes());
}

function fmtBytes(b) {
	if (b === null || b === undefined) return '—';
	var u = [ 'B', 'KiB', 'MiB', 'GiB', 'TiB' ], i = 0;
	b = Number(b);
	while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
	return (i ? b.toFixed(1) : b) + ' ' + u[i];
}

/* kb/s -> label. null = inherit, 0 = unlimited. */
function fmtRate(k, inheritLabel) {
	if (k === null || k === undefined) return inheritLabel || _('继承');
	if (Number(k) === 0) return _('不限速');
	k = Number(k);
	return (k >= 1000 ? (k / 1000).toFixed(k % 1000 ? 1 : 0) + ' Mbps' : k + ' kb/s');
}

var STATUS = {
	ACTIVE: [ '正常', 'ok' ], PENDING: [ '待审批', 'warn' ], REJECTED: [ '已拒绝', 'muted' ],
	DISABLED: [ '已暂停', 'muted' ], BANNED: [ '已封禁', 'bad' ],
	ONLINE: [ '在线', 'ok' ], REAUTHENTICATING: [ '重新认证中', 'warn' ],
	AUTH_REQUIRED: [ '需要认证', 'bad' ], OFFLINE: [ '离线', 'bad' ], UNKNOWN: [ '未知', 'muted' ],
	WHITELIST: [ '白名单', 'ok' ], BLACKLIST: [ '黑名单', 'bad' ], NONE: [ '默认', 'muted' ]
};

function badge(v) {
	var s = STATUS[v] || [ v == null ? '—' : String(v), 'muted' ];
	return el('span', { 'class': 'odac-badge odac-' + s[1] }, s[0]);
}

function errorBox(e, what) {
	return el('div', { 'class': 'alert-message warning odac-error' }, [
		el('strong', {}, (what ? what + '：' : '') + _('加载失败')), ' ',
		el('code', {}, e.code), ' ', e.message
	]);
}

function notify(e, what) {
	ui.addNotification(null, el('p', {}, [ (what || _('操作失败')) + '：', el('code', {}, e.code), ' ', e.message ]), 'danger');
}

function ok(msg) {
	ui.addTimeLimitedNotification(null, el('p', {}, msg), 4000, 'info');
}

/* Run an admin action, report real backend result, then reload. */
function act(method, params, okMsg, after) {
	return call(method, params).then(function(d) {
		ok(okMsg || _('操作已完成'));
		return after ? after(d) : window.setTimeout(function() { location.reload(); }, 400);
	}, function(e) { notify(e); });
}

function confirmDialog(title, body, label, danger, fn) {
	ui.showModal(title, [
		el('div', {}, body),
		el('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('取消')), ' ',
			E('button', { 'class': 'btn ' + (danger ? 'cbi-button-negative' : 'cbi-button-positive'),
				'click': function() { ui.hideModal(); fn(); } }, label)
		])
	]);
}

/* Rate editor: select inherit / unlimited / custom + Mbps number. */
function rateField(label, value) {
	var mode = (value === null || value === undefined) ? 'inherit' : (Number(value) === 0 ? 'unlimited' : 'custom');
	var num = E('input', { 'type': 'number', 'min': '0.001', 'max': '1000', 'step': 'any', 'class': 'cbi-input-text odac-num',
		'value': mode === 'custom' ? String(Number(value) / 1000) : '', 'aria-label': label + ' Mbps' });
	var sel = E('select', { 'class': 'cbi-input-select', 'aria-label': label }, [
		E('option', { 'value': 'inherit' }, _('继承上级')),
		E('option', { 'value': 'unlimited' }, _('不限速')),
		E('option', { 'value': 'custom' }, _('自定义 (Mbps)'))
	]);
	sel.value = mode;
	function sync() { num.disabled = sel.value !== 'custom'; }
	sel.addEventListener('change', sync); sync();
	return {
		node: el('div', { 'class': 'odac-field' }, [ el('label', {}, label), el('div', { 'class': 'odac-inline' }, [ sel, num ]) ]),
		get: function() {
			if (sel.value === 'inherit') return null;
			if (sel.value === 'unlimited') return 0;
			var m = parseFloat(num.value);
			if (!(m >= 0.001) || m > 1000) throw new OdacError('INVALID', label + _('：请输入 0.001–1000 Mbps'));
			return Math.round(m * 1000);
		}
	};
}

function field(label, input, hint) {
	return el('div', { 'class': 'odac-field' }, [ el('label', {}, label), input, hint ? el('small', {}, hint) : null ]);
}

/*
 * Sortable/searchable table. cols: [{key,label,render?(row),sort?(row)}]
 * opts: {rows, empty, filter?(row)->bool, search?(row)->string, onRow?}
 */
function table(cols, rows, opts) {
	opts = opts || {};
	var state = { key: opts.sortKey || null, dir: 1, q: '' };
	var tbody = E('tbody');
	var wrap = el('div', { 'class': 'odac-table-wrap' });
	var thead = E('tr');
	var t = el('table', { 'class': 'table odac-table' }, [ el('thead', {}, thead), tbody ]);

	cols.forEach(function(c) {
		var th = el('th', { 'scope': 'col' }, c.label);
		if (c.key && c.sortable !== false) {
			th.classList.add('odac-sortable');
			th.tabIndex = 0;
			th.setAttribute('aria-sort', 'none');
			var go = function() {
				state.dir = state.key === c.key ? -state.dir : 1;
				state.key = c.key;
				thead.querySelectorAll('th').forEach(function(x) { x.setAttribute('aria-sort', 'none'); });
				th.setAttribute('aria-sort', state.dir > 0 ? 'ascending' : 'descending');
				draw();
			};
			th.addEventListener('click', go);
			th.addEventListener('keydown', function(ev) { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); go(); } });
		}
		thead.appendChild(th);
	});

	function val(r, c) { return c.sort ? c.sort(r) : r[c.key]; }

	function draw() {
		var list = rows.filter(function(r) {
			if (opts.filter && !opts.filter(r)) return false;
			if (!state.q) return true;
			var s = opts.search ? opts.search(r) : cols.map(function(c) { return r[c.key]; }).join(' ');
			return String(s).toLowerCase().indexOf(state.q) >= 0;
		});
		if (state.key) {
			var c = cols.filter(function(x) { return x.key === state.key; })[0];
			list.sort(function(a, b) {
				var x = val(a, c), y = val(b, c);
				if (x == null) return 1; if (y == null) return -1;
				return (x > y ? 1 : x < y ? -1 : 0) * state.dir;
			});
		}
		while (tbody.firstChild) tbody.removeChild(tbody.firstChild);
		if (!list.length) {
			tbody.appendChild(el('tr', {}, el('td', { 'colspan': String(cols.length), 'class': 'odac-empty' },
				rows.length ? _('没有匹配的记录') : (opts.empty || _('暂无数据')))));
		}
		list.forEach(function(r) {
			var tr = E('tr');
			cols.forEach(function(c) {
				var v = c.render ? c.render(r) : r[c.key];
				tr.appendChild(el('td', { 'data-title': c.label }, v));
			});
			tbody.appendChild(tr);
		});
		if (opts.onDraw) opts.onDraw(list);
	}

	wrap.appendChild(t);
	draw();
	return {
		node: wrap,
		redraw: draw,
		setQuery: function(q) { state.q = String(q || '').trim().toLowerCase(); draw(); }
	};
}

function searchBox(placeholder, tbl) {
	var i = E('input', { 'type': 'search', 'class': 'cbi-input-text odac-search', 'placeholder': placeholder, 'aria-label': placeholder });
	i.addEventListener('input', function() { tbl.setQuery(i.value); });
	return i;
}

function css() {
	if (document.getElementById('odac-css')) return;
	var l = E('link', { 'id': 'odac-css', 'rel': 'stylesheet', 'href': L.resource('odac/odac.css') });
	document.head.appendChild(l);
}

function header(title, sub) {
	css();
	return el('div', { 'class': 'odac-head' }, [ el('h2', {}, title), sub ? el('p', { 'class': 'cbi-section-descr' }, sub) : null ]);
}

function macWarning() {
	return el('div', { 'class': 'alert-message warning odac-note', 'role': 'note' }, [
		el('strong', {}, _('安全提示：')),
		_('MAC 白名单/黑名单只是便捷的准入手段，并不能证明设备身份。MAC 地址可以被伪造；手机等设备默认启用的“随机/私有 MAC”会让同一设备在不同网络或重连后显示为新设备，导致白名单失效或黑名单被绕过。重要设备请改用账号登录。')
	]);
}

return baseclass.extend({
	call: call, load: load, txt: txt, el: el, fmtTime: fmtTime, fmtBytes: fmtBytes, fmtRate: fmtRate,
	badge: badge, errorBox: errorBox, notify: notify, ok: ok, act: act, confirm: confirmDialog,
	rateField: rateField, field: field, table: table, searchBox: searchBox, header: header,
	macWarning: macWarning, OdacError: OdacError, STATUS: STATUS
});
