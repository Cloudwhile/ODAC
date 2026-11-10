'use strict';
'require view';
'require ui';
'require odac.common as C';

function drawer(d) {
	var bg = E('div', { 'class': 'odac-drawer-bg' });
	var alias = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'maxlength': '64', 'value': d.alias || '' });
	var pol = E('select', { 'class': 'cbi-input-select' }, [ E('option', { value: 'NONE' }, _('默认（需登录）')), E('option', { value: 'WHITELIST' }, _('白名单（免登录放行）')), E('option', { value: 'BLACKLIST' }, _('黑名单（拒绝）')) ]);
	pol.value = d.policy || 'NONE';
	var exp = E('input', { 'type': 'datetime-local', 'class': 'cbi-input-text' });
	if (d.expires_at) { var t = new Date(d.expires_at * 1000); exp.value = new Date(t - t.getTimezoneOffset() * 60000).toISOString().slice(0, 16); }
	var up = C.rateField(_('上传限速'), d.upload_kbps), dn = C.rateField(_('下载限速'), d.download_kbps);
	function close() { bg.remove(); dr.remove(); document.removeEventListener('keydown', esc); }
	function esc(e) { if (e.key === 'Escape') close(); }
	function row(k, v) { return [ C.el('dt', {}, k), C.el('dd', {}, v) ]; }
	var dr = C.el('aside', { 'class': 'odac-drawer', 'role': 'dialog', 'aria-modal': 'true', 'aria-label': _('设备详情') }, [
		C.el('div', { 'class': 'odac-toolbar' }, [ C.el('h3', { 'style': 'flex:1;margin:0' }, d.alias || d.hostname || d.mac), E('button', { 'class': 'btn', 'click': close }, _('关闭')) ]),
		C.el('dl', { 'class': 'odac-dl' }, [].concat(row('MAC', d.mac), row('IPv4', d.ipv4), row('IPv6', d.ipv6), row(_('主机名'), d.hostname),
			row(_('接入'), [ d.access_type, d.ssid, d.interface ].filter(Boolean).join(' · ') || null), row(_('关联账号'), d.username),
			row(_('在线'), d.online == null ? null : (d.online ? _('是') : _('否'))), row(_('认证状态'), d.auth_state),
			row(_('首次发现'), C.fmtTime(d.first_seen)), row(_('最近活动'), C.fmtTime(d.last_seen)),
			row(_('流量 上/下'), C.fmtBytes(d.upload_bytes) + ' / ' + C.fmtBytes(d.download_bytes)))),
		C.el('h4', {}, _('策略')), C.macWarning(),
		C.field(_('备注'), alias), C.field(_('访问策略'), pol), C.field(_('策略有效期'), exp, _('留空=永久；到期后恢复为默认策略')),
		up.node, dn.node, C.el('small', {}, _('“继承上级”= 使用关联账号默认，否则系统默认。')),
		C.el('div', { 'class': 'odac-toolbar' }, [
			E('button', { 'class': 'btn cbi-button cbi-button-positive', 'click': function() {
				var p; try { p = { mac: d.mac, alias: alias.value.trim() || null, policy: pol.value,
					expires_at: exp.value ? Math.floor(new Date(exp.value).getTime() / 1000) : null, upload_kbps: up.get(), download_kbps: dn.get() }; }
				catch (e) { return C.notify(e, _('输入无效')); }
				var go = function() { C.act('device_update', p, _('设备策略已更新')); };
				if (p.policy === 'BLACKLIST' && d.policy !== 'BLACKLIST') C.confirm(_('加入黑名单'), _('该设备将立即被断开，且无论使用哪个账号都无法上网。'), _('加入黑名单'), true, go); else go();
			} }, _('保存')),
			d.online ? E('button', { 'class': 'btn cbi-button cbi-button-negative', 'click': function() {
				C.confirm(_('强制下线'), _('立即撤销该设备的当前会话？'), _('强制下线'), true, function() { C.act('disconnect', { mac: d.mac }, _('已下线')); }); } }, _('强制下线')) : null ])
	]);
	document.body.appendChild(bg); document.body.appendChild(dr);
	bg.addEventListener('click', close); document.addEventListener('keydown', esc); alias.focus();
}

return view.extend({
	load: function() { return C.load('devices'); },
	render: function(r) {
		var body = [ C.header(_('设备管理'), _('点击设备行的“详情”打开抽屉，可修改备注、白/黑名单、限速或强制下线。')), C.macWarning() ];
		if (r.error) { body.push(C.errorBox(r.error, _('设备列表'))); return E('div', {}, body); }
		var rows = Array.isArray(r.data) ? r.data : [];
		var f = E('select', { 'class': 'cbi-input-select', 'aria-label': _('筛选') }, [ [ '', _('全部设备') ], [ 'online', _('在线') ], [ 'offline', _('离线') ],
			[ 'WHITELIST', _('白名单') ], [ 'BLACKLIST', _('黑名单') ], [ 'NONE', _('默认策略') ] ].map(function(o) { return E('option', { value: o[0] }, o[1]); }));
		function q(fn, label, cls) { return E('button', { 'class': 'btn cbi-button ' + (cls || ''), 'click': fn }, label); }
		var t = C.table([
			{ key: 'alias', label: _('设备名称'), sort: function(d) { return d.alias || d.hostname || ''; }, render: function(d) { return d.alias || d.hostname; } },
			{ key: 'mac', label: 'MAC' }, { key: 'ipv4', label: 'IP' }, { key: 'username', label: _('账号') },
			{ key: 'online', label: _('在线'), render: function(d) { return d.online == null ? '—' : C.el('span', { 'class': 'odac-badge ' + (d.online ? 'odac-ok' : 'odac-muted') }, d.online ? _('在线') : _('离线')); } },
			{ key: 'auth_state', label: _('认证') }, { key: 'policy', label: _('策略'), render: function(d) { return C.badge(d.policy || 'NONE'); } },
			{ key: 'access_type', label: _('接入') },
			{ key: 'download_bytes', label: _('流量 上/下'), render: function(d) { return C.fmtBytes(d.upload_bytes) + ' / ' + C.fmtBytes(d.download_bytes); } },
			{ key: 'upload_kbps', label: _('限速 上/下'), render: function(d) { return C.fmtRate(d.upload_kbps) + ' / ' + C.fmtRate(d.download_kbps); } },
			{ key: 'first_seen', label: _('首次连接'), render: function(d) { return C.fmtTime(d.first_seen); } },
			{ key: 'last_seen', label: _('最近活动'), render: function(d) { return C.fmtTime(d.last_seen); } },
			{ key: null, label: _('操作'), sortable: false, render: function(d) {
				var base = { mac: d.mac, alias: d.alias, expires_at: d.expires_at, upload_kbps: d.upload_kbps, download_kbps: d.download_kbps };
				function setp(p, msg) { return function() { var o = Object.assign({}, base, { policy: p }); C.act('device_update', o, msg); }; }
				return C.el('div', { 'class': 'odac-actions' }, [ q(function() { drawer(d); }, _('详情')),
					d.policy === 'WHITELIST' ? q(setp('NONE', _('已移出白名单')), _('移出白名单')) : (d.policy !== 'BLACKLIST' ? q(setp('WHITELIST', _('已加入白名单')), _('白名单'), 'cbi-button-positive') : null),
					d.policy === 'BLACKLIST' ? q(setp('NONE', _('已移出黑名单')), _('移出黑名单')) : q(function() {
						C.confirm(_('加入黑名单'), _('设备 ') + d.mac + _(' 将立即被断开且任何账号均无法放行。'), _('加入黑名单'), true, setp('BLACKLIST', _('已加入黑名单'))); }, _('黑名单'), 'cbi-button-negative') ]);
			} }
		], rows, { sortKey: 'last_seen', empty: _('尚未发现设备'), search: function(d) { return [ d.mac, d.ipv4, d.ipv6, d.hostname, d.alias, d.username ].join(' '); },
			filter: function(d) { var v = f.value; if (!v) return true; if (v === 'online') return !!d.online; if (v === 'offline') return d.online === false; return (d.policy || 'NONE') === v; } });
		f.addEventListener('change', t.redraw);
		body.push(C.el('div', { 'class': 'odac-toolbar' }, [ C.searchBox(_('搜索 MAC / IP / 名称 / 账号'), t), f ]), t.node);
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
