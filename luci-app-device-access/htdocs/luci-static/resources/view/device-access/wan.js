'use strict';
'require view';
'require ui';
'require odac.common as C';

'require poll';

return view.extend({
	load: function() { return C.load('wan'); },
	box: null,
	draw: function(r) {
		var b = this.box; while (b.firstChild) b.removeChild(b.firstChild);
		if (r.error) { b.appendChild(C.errorBox(r.error, _('WAN 状态'))); return; }
		var w = r.data || {};
		function row(k, v) { return [ C.el('dt', {}, k), C.el('dd', {}, v) ]; }
		var fw = w.forwarding == null ? '—' : (w.forwarding ? C.el('span', { 'class': 'odac-badge odac-ok' }, _('已放行（已授权设备）')) : C.el('span', { 'class': 'odac-badge odac-bad' }, _('已暂停')));
		b.appendChild(C.el('dl', { 'class': 'odac-dl' }, [].concat(
			row(_('WAN 认证状态'), C.badge(w.state || 'UNKNOWN')), row(_('上游认证地址'), w.upstream),
			row(_('最近检测'), C.fmtTime(w.checked_at)), row(_('最近成功'), C.fmtTime(w.last_success)), row(_('最近失败'), C.fmtTime(w.last_failure)),
			row(_('自动登录脚本'), w.script_running == null ? '—' : (w.script_running ? _('进程运行中（仅诊断，不代表已认证）') : _('未检测到进程'))),
			row(_('最近错误'), w.error), row(_('自动重试'), w.retry == null ? '—' : String(w.retry)), row(_('下游转发'), fw))));
	},
	render: function(r) {
		this.box = E('div', { 'class': 'cbi-section', 'aria-live': 'polite' });
		this.draw(r);
		poll.add(L.bind(function() { return C.load('wan').then(L.bind(this.draw, this)); }, this), 15);
		return E('div', {}, [ C.header(_('上游认证'), _('只读适配器：上游认证由管理员自己的脚本负责，ODAC 只读取脚本写入的状态文件，不显示上游密码、Cookie 或 Token。每 15 秒刷新。')), this.box,
			C.el('div', { 'class': 'alert-message notice' }, _('上游认证失效时，普通下游设备的 WAN 转发会被暂停；路由器自身仍可访问上游门户以完成自动登录。管理员脚本每次尝试后应立即写入状态（/usr/libexec/odac-wan-state <状态>），以缩短失效到生效之间的窗口；期间个别请求仍可能收到上游重定向。')) ]);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
