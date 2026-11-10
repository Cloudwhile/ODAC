'use strict';
'require view';
'require ui';
'require odac.common as C';

function editUser(u) {
	var maxd = E('input', { 'type': 'number', 'min': '0', 'max': '64', 'class': 'cbi-input-text', 'value': u.max_devices == null ? '' : String(u.max_devices), 'placeholder': _('留空=系统默认') });
	var exp = E('input', { 'type': 'datetime-local', 'class': 'cbi-input-text' });
	if (u.expires_at) { var d = new Date(u.expires_at * 1000); exp.value = new Date(d - d.getTimezoneOffset() * 60000).toISOString().slice(0, 16); }
	var up = C.rateField(_('上传限速'), u.upload_kbps), dn = C.rateField(_('下载限速'), u.download_kbps);
	ui.showModal(_('编辑账号：') + u.username, [
		C.field(_('最大同时在线设备数'), maxd, _('留空表示继承系统默认')),
		C.field(_('有效期'), exp, _('留空表示永久有效')), up.node, dn.node,
		C.el('p', { 'class': 'cbi-section-descr' }, _('限速优先级：设备自定义 > 账号默认 > 系统默认。')),
		E('div', { 'class': 'right' }, [ E('button', { 'class': 'btn', 'click': ui.hideModal }, _('取消')), ' ',
			E('button', { 'class': 'btn cbi-button-positive', 'click': function() {
				var p;
				try { p = { id: u.id, action: 'update', max_devices: maxd.value === '' ? null : parseInt(maxd.value, 10),
					expires_at: exp.value ? Math.floor(new Date(exp.value).getTime() / 1000) : null,
					upload_kbps: up.get(), download_kbps: dn.get() }; } catch (e) { return C.notify(e, _('输入无效')); }
				ui.hideModal(); C.act('user_update', p, _('账号已更新'));
			} }, _('保存')) ]) ]);
}

function resetPw(u) {
	var a = E('input', { 'type': 'password', 'class': 'cbi-input-text', 'autocomplete': 'new-password', 'minlength': '6' });
	var b = E('input', { 'type': 'password', 'class': 'cbi-input-text', 'autocomplete': 'new-password' });
	ui.showModal(_('重置密码：') + u.username, [ C.field(_('新密码（至少 6 位）'), a), C.field(_('确认新密码'), b),
		C.el('p', {}, _('重置后该账号所有在线设备会被断开，用户需用新密码重新登录。')),
		E('div', { 'class': 'right' }, [ E('button', { 'class': 'btn', 'click': ui.hideModal }, _('取消')), ' ',
			E('button', { 'class': 'btn cbi-button-negative', 'click': function() {
				if (a.value.length < 6) return C.notify(new C.OdacError('INVALID', _('密码至少 6 位')));
				if (a.value !== b.value) return C.notify(new C.OdacError('INVALID', _('两次输入不一致')));
				ui.hideModal(); C.act('user_update', { id: u.id, action: 'reset_password', password: a.value }, _('密码已重置'));
			} }, _('重置')) ]) ]);
}

function detail(u, devs) {
	var mine = devs.filter(function(d) { return d.username === u.username; });
	ui.showModal(_('账号详情：') + u.username, [
		C.el('dl', { 'class': 'odac-dl' }, [
			C.el('dt', {}, _('状态')), C.el('dd', {}, C.badge(u.status)),
			C.el('dt', {}, _('注册时间')), C.el('dd', {}, C.fmtTime(u.created_at)),
			C.el('dt', {}, _('最后登录')), C.el('dd', {}, C.fmtTime(u.last_login)),
			C.el('dt', {}, _('有效期')), C.el('dd', {}, u.expires_at ? C.fmtTime(u.expires_at) : _('永久')),
			C.el('dt', {}, _('最大设备')), C.el('dd', {}, u.max_devices == null ? _('系统默认') : u.max_devices),
			C.el('dt', {}, _('上传 / 下载')), C.el('dd', {}, C.fmtRate(u.upload_kbps) + ' / ' + C.fmtRate(u.download_kbps))
		]),
		C.el('h4', {}, _('关联设备')),
		C.table([ { key: 'mac', label: 'MAC' }, { key: 'hostname', label: _('名称'), render: function(d) { return d.alias || d.hostname; } },
			{ key: 'online', label: _('在线'), render: function(d) { return d.online ? _('是') : _('否'); } } ], mine, { empty: _('无关联设备') }).node,
		E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, _('关闭')))
	]);
}

return view.extend({
	load: function() { return Promise.all([ C.load('users'), C.load('devices') ]); },
	render: function(r) {
		var body = [ C.header(_('账号管理'), _('批准、暂停、封禁账号，设置有效期、设备数与账号默认限速。')) ];
		if (r[0].error) { body.push(C.errorBox(r[0].error, _('账号列表'))); return E('div', {}, body); }
		var users = Array.isArray(r[0].data) ? r[0].data : [], devs = (r[1].data && Array.isArray(r[1].data)) ? r[1].data : [];
		var flt = E('select', { 'class': 'cbi-input-select', 'aria-label': _('状态筛选') }, [ E('option', { value: '' }, _('全部状态')) ].concat(
			[ 'PENDING', 'ACTIVE', 'DISABLED', 'BANNED', 'REJECTED' ].map(function(s) { return E('option', { value: s }, C.STATUS[s][0]); })));
		function btn(label, cls, fn) { return E('button', { 'class': 'btn cbi-button ' + (cls || ''), 'click': fn }, label); }
		function uact(u, action, msg, danger, text) {
			return function() { C.confirm(msg + '：' + u.username, text || _('确认执行此操作？'), msg, danger,
				function() { C.act('user_update', { id: u.id, action: action }, msg + _(' 已完成')); }); };
		}
		var t = C.table([
			{ key: 'username', label: _('用户名') },
			{ key: 'status', label: _('状态'), render: function(u) { return C.badge(u.status); } },
			{ key: 'device_count', label: _('设备数'), render: function(u) { return (u.device_count == null ? '—' : u.device_count) + ' / ' + (u.max_devices == null ? _('默认') : u.max_devices); } },
			{ key: 'last_login', label: _('最后登录'), render: function(u) { return C.fmtTime(u.last_login); } },
			{ key: 'expires_at', label: _('有效期'), render: function(u) { return u.expires_at ? C.fmtTime(u.expires_at) : _('永久'); } },
			{ key: 'upload_kbps', label: _('限速 上/下'), render: function(u) { return C.fmtRate(u.upload_kbps) + ' / ' + C.fmtRate(u.download_kbps); } },
			{ key: null, label: _('操作'), sortable: false, render: function(u) {
				var a = [ btn(_('详情'), '', function() { detail(u, devs); }) ];
				if (u.status === 'PENDING') {
					a.push(btn(_('批准'), 'cbi-button-positive', function() { C.act('approve', { ids: [ u.id ] }, _('已批准')); }));
					a.push(btn(_('拒绝'), 'cbi-button-negative', uact(u, 'reject', _('拒绝'), true)));
				} else {
					a.push(btn(_('编辑'), 'cbi-button-edit', function() { editUser(u); }));
					a.push(btn(_('重置密码'), '', function() { resetPw(u); }));
					if (u.status === 'ACTIVE') a.push(btn(_('暂停'), '', uact(u, 'disable', _('暂停'), true, _('暂停后该账号无法登录，并断开其所有在线设备。可随时恢复。'))));
					if (u.status === 'DISABLED') a.push(btn(_('恢复'), 'cbi-button-positive', uact(u, 'enable', _('恢复'))));
					if (u.status === 'BANNED') a.push(btn(_('解除封禁'), 'cbi-button-positive', uact(u, 'unban', _('解除封禁'))));
					else a.push(btn(_('封禁'), 'cbi-button-negative', uact(u, 'ban', _('封禁'), true, _('封禁会立即撤销该账号授予的全部设备会话，并阻止重新登录。'))));
					a.push(btn(_('断开设备'), '', uact(u, 'disconnect', _('断开全部设备'), true)));
				}
				a.push(btn(_('删除'), 'cbi-button-remove', uact(u, 'delete', _('删除账号'), true, _('删除不可撤销：账号与其设备绑定会被移除，在线会话立即断开。如需保留记录请改用“暂停”。'))));
				return C.el('div', { 'class': 'odac-actions' }, a);
			} }
		], users, { sortKey: 'username', empty: _('暂无注册账号'), filter: function(u) { return !flt.value || u.status === flt.value; },
			search: function(u) { return u.username; } });
		flt.addEventListener('change', t.redraw);
		body.push(C.el('div', { 'class': 'odac-toolbar' }, [ C.searchBox(_('搜索用户名'), t), flt ]), t.node);
		if (r[1].error) body.push(C.errorBox(r[1].error, _('设备列表（账号详情中的关联设备不可用）')));
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
