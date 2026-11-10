'use strict';
'require view';
'require ui';
'require odac.common as C';

return view.extend({
	load: function() { return Promise.all([ C.load('overview'), C.load('audit', { limit: 10 }) ]); },
	render: function(r) {
		var ov = r[0], au = r[1], body = [ C.header(_('设备准入控制 · 总览'), _('数据实时取自 odacd 后端；未知值显示为 —。')) ];
		if (ov.error) body.push(C.errorBox(ov.error, _('总览')));
		else {
			var d = ov.data || {}, w = d.wan || {};
			function card(k, v) { return C.el('div', { 'class': 'odac-card' }, [ C.el('div', { 'class': 'k' }, k), C.el('div', { 'class': 'v' }, v) ]); }
			body.push(C.el('div', { 'class': 'odac-grid' }, [
				card(_('WAN 认证'), C.badge(w.state || 'UNKNOWN')),
				card(_('在线设备'), d.devices_online), card(_('已授权设备'), d.devices_authorized),
				card(_('待审核账号'), d.registrations_pending), card(_('注册用户'), d.users),
				card(_('白名单设备'), d.whitelist), card(_('黑名单设备'), d.blacklist),
				card(_('当前上传'), d.upload_kbps == null ? '—' : C.fmtRate(d.upload_kbps)),
				card(_('当前下载'), d.download_kbps == null ? '—' : C.fmtRate(d.download_kbps))
			]));
			var ev = Array.isArray(d.recent_events) ? d.recent_events : [];
			body.push(C.el('h3', {}, _('最近登录事件')));
			body.push(C.table([ { key: 'time', label: _('时间'), render: function(x) { return C.fmtTime(x.time); } },
				{ key: 'type', label: _('事件') }, { key: 'username', label: _('账号') }, { key: 'mac', label: 'MAC' } ],
				ev, { empty: _('暂无事件') }).node);
		}
		body.push(C.el('h3', {}, _('最近管理操作')));
		if (au.error) body.push(C.errorBox(au.error, _('审计日志')));
		else body.push(C.table([ { key: 'time', label: _('时间'), render: function(x) { return C.fmtTime(x.time); } },
			{ key: 'actor', label: _('操作者') }, { key: 'action', label: _('操作') }, { key: 'target', label: _('对象') },
			{ key: 'detail', label: _('详情') } ], Array.isArray(au.data) ? au.data : [], { empty: _('暂无操作记录') }).node);
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
