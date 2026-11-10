'use strict';
'require view';
'require ui';
'require odac.common as C';

return view.extend({
	load: function() { return C.load('registrations'); },
	render: function(r) {
		var body = [ C.header(_('注册审批'), _('审批前请核对用户名、申请时间与申请设备。设备名称由系统自动识别，所有审批操作由后端写入审计日志。')) ];
		if (r.error) { body.push(C.errorBox(r.error, _('待审核列表'))); return E('div', {}, body); }
		var rows = Array.isArray(r.data) ? r.data : [], sel = {};
		var cnt = C.el('span', {}, _('已选 0 项'));
		function upd() { var n = Object.keys(sel).filter(function(k) { return sel[k]; }).length; cnt.textContent = _('已选 ') + n + _(' 项'); return n; }
		function ids() { return Object.keys(sel).filter(function(k) { return sel[k]; }).map(function(k) { return isNaN(+k) ? k : +k; }); }
		function bulk(m, label, danger) {
			return E('button', { 'class': 'btn cbi-button ' + (danger ? 'cbi-button-negative' : 'cbi-button-positive'), 'click': function() {
				var l = ids(); if (!l.length) return C.notify(new C.OdacError('NONE', _('请先勾选申请')));
				C.confirm(label, _('将对 ') + l.length + _(' 个申请执行“') + label + _('”。'), label, danger, function() { C.act(m, { ids: l }, label + _('：已提交 ') + l.length + _(' 项')); });
			} }, label);
		}
		var t = C.table([
			{ key: null, label: _('选择'), sortable: false, render: function(x) {
				var c = E('input', { 'type': 'checkbox', 'aria-label': _('选择 ') + x.username });
				c.checked = !!sel[x.id]; c.addEventListener('change', function() { sel[x.id] = c.checked; upd(); }); return c; } },
			{ key: 'username', label: _('用户名') },
			{ key: 'created_at', label: _('申请时间'), render: function(x) { return C.fmtTime(x.created_at); } },
			{ key: 'mac', label: _('申请设备'), render: function(x) { return [ x.mac, x.ipv4, x.hostname ].filter(Boolean).join(' · ') || '—'; } },
			{ key: null, label: _('操作'), sortable: false, render: function(x) { return C.el('div', { 'class': 'odac-actions' }, [
				E('button', { 'class': 'btn cbi-button cbi-button-positive', 'click': function() { C.act('approve', { ids: [ x.id ] }, _('已批准 ') + x.username); } }, _('批准')),
				E('button', { 'class': 'btn cbi-button cbi-button-negative', 'click': function() {
					C.confirm(_('拒绝申请'), _('拒绝 ') + x.username + _(' 的注册申请？'), _('拒绝'), true, function() { C.act('reject', { ids: [ x.id ] }, _('已拒绝')); }); } }, _('拒绝')) ]); } }
		], rows, { sortKey: 'created_at', empty: _('当前没有待审核的注册申请'), search: function(x) { return [ x.username, x.mac, x.ipv4, x.hostname ].join(' '); } });
		body.push(C.el('div', { 'class': 'odac-toolbar' }, [ C.searchBox(_('搜索用户名 / MAC / IP / 设备名'), t), cnt, bulk('approve', _('批量批准')), bulk('reject', _('批量拒绝'), true) ]), t.node);
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
