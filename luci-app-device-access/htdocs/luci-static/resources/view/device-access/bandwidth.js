'use strict';
'require view';
'require ui';
'require odac.common as C';

return view.extend({
	load: function() { return Promise.all([ C.load('settings'), C.load('users'), C.load('devices') ]); },
	render: function(r) {
		var body = [ C.header(_('带宽管理'), null),
			C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('规则覆盖关系')), C.el('ol', { 'class': 'odac-rule' }, [
				C.el('li', {}, _('设备自定义限速（含白名单设备）优先级最高；')),
				C.el('li', {}, _('设备为“继承”时使用关联账号的默认限速；')),
				C.el('li', {}, _('账号也为“继承”或设备无账号时使用系统默认限速。')),
				C.el('li', {}, _('“不限速”是明确设置的值（0），不会再向上继承。')) ]),
				C.el('p', { 'class': 'cbi-section-descr' }, _('限速由 openNDS 按 kb/s 执行（1 Mbps = 1000 kb/s），属于平均速率控制，不保证瞬时带宽上限。')) ]) ];
		var s = r[0].data || {}, users = Array.isArray(r[1].data) ? r[1].data : [], devs = Array.isArray(r[2].data) ? r[2].data : [];
		var umap = {}; users.forEach(function(u) { umap[u.username] = u; });
		if (r[0].error) body.push(C.errorBox(r[0].error, _('系统默认')));
		else {
			var up = C.rateField(_('默认上传'), s.upload_kbps), dn = C.rateField(_('默认下载'), s.download_kbps);
			body.push(C.el('h3', {}, _('全局默认限速')), up.node, dn.node, E('button', { 'class': 'btn cbi-button cbi-button-save', 'click': function() {
				var p; try { p = { upload_kbps: up.get(), download_kbps: dn.get() }; } catch (e) { return C.notify(e, _('输入无效')); }
				if (p.upload_kbps === null || p.download_kbps === null) return C.notify(new C.OdacError('INVALID', _('系统默认没有上级可继承，请选择不限速或自定义')));
				C.act('settings_update', p, _('默认限速已保存'));
			} }, _('保存默认限速')));
		}
		function eff(dev, k) {
			if (dev[k] != null) return [ C.fmtRate(dev[k]), _('设备') ];
			var u = dev.username && umap[dev.username];
			if (u && u[k] != null) return [ C.fmtRate(u[k]), _('账号') ];
			if (s[k] != null) return [ C.fmtRate(s[k]), _('系统') ];
			return [ '—', _('未知') ];
		}
		body.push(C.el('h3', {}, _('账号级限速')));
		if (r[1].error) body.push(C.errorBox(r[1].error));
		else body.push(C.table([ { key: 'username', label: _('账号') }, { key: 'upload_kbps', label: _('上传'), render: function(u) { return C.fmtRate(u.upload_kbps, _('继承系统')); } },
			{ key: 'download_kbps', label: _('下载'), render: function(u) { return C.fmtRate(u.download_kbps, _('继承系统')); } } ], users, { empty: _('暂无账号'), sortKey: 'username' }).node,
			C.el('small', {}, _('在“账号管理”中编辑账号限速。')));
		body.push(C.el('h3', {}, _('设备级限速与当前生效策略')));
		if (r[2].error) body.push(C.errorBox(r[2].error));
		else {
			var t = C.table([ { key: 'mac', label: 'MAC' }, { key: 'alias', label: _('名称'), render: function(d) { return d.alias || d.hostname; } },
				{ key: 'policy', label: _('策略'), render: function(d) { return C.badge(d.policy || 'NONE'); } }, { key: 'username', label: _('账号') },
				{ key: 'upload_kbps', label: _('设备设置 上/下'), render: function(d) { return C.fmtRate(d.upload_kbps) + ' / ' + C.fmtRate(d.download_kbps); } },
				{ key: null, label: _('生效上传（来源）'), sortable: false, render: function(d) { var e = eff(d, 'upload_kbps'); return e[0] + '（' + e[1] + '）'; } },
				{ key: null, label: _('生效下载（来源）'), sortable: false, render: function(d) { var e = eff(d, 'download_kbps'); return e[0] + '（' + e[1] + '）'; } } ],
				devs, { empty: _('尚未发现设备') });
			body.push(C.el('div', { 'class': 'odac-toolbar' }, [ C.searchBox(_('搜索设备'), t) ]), t.node,
				C.el('small', {}, _('“生效”列由界面按覆盖规则推算，仅供参考；在“设备管理”抽屉中修改设备限速。')));
		}
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
