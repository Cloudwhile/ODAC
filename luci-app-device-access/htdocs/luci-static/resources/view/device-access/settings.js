'use strict';
'require view';
'require ui';
'require fs';
'require odac.common as C';

/**
 * 验证数字范围
 * @param {number} n - 要验证的数字
 * @param {number} min - 最小值（包含）
 * @param {number} max - 最大值（包含）
 * @param {string} label - 字段标签名（用于错误提示）
 * @returns {string|null} 错误信息或null（如果有效）
 */
function validateRange(n, min, max, label) {
	if (isNaN(n) || n < min || n > max) {
		return label + _(' 应在 ') + min + '–' + max + _(' 之间');
	}
	return null;
}

/**
 * 验证文本长度
 * @param {string} text - 要验证的文本
 * @param {number} maxLen - 最大长度
 * @param {string} label - 字段标签名（用于错误提示）
 * @returns {string|null} 错误信息或null（如果有效）
 */
function validateTextLength(text, maxLen, label) {
	if (text.length > maxLen) {
		return label + _(' 超过 ') + maxLen + _(' 个字符');
	}
	return null;
}

var EDIT = [
	[ 'session_seconds', _('默认会话有效期（分钟）'), 60, 1, 10080, _('用户登录后的会话时长，1 分钟–7 天') ],
	[ 'max_devices', _('最大设备记录数'), 1, 1, 5000, _('数据库中保留的设备记录总数上限') ],
	[ 'default_devices', _('新账号默认设备数上限'), 1, 1, 64, _('审批通过的账号未单独设置时可同时登录的设备数') ],
	[ 'max_users', _('最大注册账号数量'), 1, 1, 2000 ],
	[ 'max_sessions', _('最大并发认证会话'), 1, 1, 256 ],
	[ 'upload_kbps', _('默认上行限速（kb/s，0 为不限速）'), 1, 0, 1000000 ],
	[ 'download_kbps', _('默认下行限速（kb/s，0 为不限速）'), 1, 0, 1000000 ],
	[ 'max_audit', _('审计日志最大条数'), 1, 10, 10000, _('超出后自动清理最旧记录') ],
	[ 'wan_interval', _('WAN 检测周期（秒）'), 1, 5, 30, _('范围 5–30 秒') ],
	[ 'db_max_kib', _('数据库软上限（KiB）'), 1, 512, 65536, _('范围 512–65536 KiB') ]
];
/* key, label, max length, textarea. Empty restores the built-in portal text. */
var TEXTS = [
	[ 'portal_title', _('门户标题'), 64, false ],
	[ 'portal_subtitle', _('标题下方说明'), 128, false ],
	[ 'portal_register_hint', _('注册表单提示'), 256, true ],
	[ 'portal_footer', _('页脚说明'), 512, true ]
];
/* System configuration uses a separate RPC with preflight and rollback. */
var NET_EDIT = [
	[ 'interface', _('受控网络接口'), 'text', _('须与 ODAC 已配置 network/zone 对应的独立接口一致，如 br-guest；请先在网络设置中配置，ODAC 不会迁移接口或网段') ],
	[ 'gateway', _('门户网关地址'), 'text', _('受控接口现有的静态 IPv4 地址') ],
	[ 'portal_hostname', _('门户主机名（HTTPS）'), 'text', _('必须与证书匹配，如 portal.example.com') ],
	[ 'portal_https_port', _('门户 HTTPS 端口'), 'number', _('默认 8443，范围 1025–65535，不可使用 2050') ],
	[ 'portal_port', _('门户 HTTP 端口（仅跳转）'), 'number', _('默认 2080，范围 1025–65535，不可使用 2050') ],
	[ 'upstream', _('上游认证 IPv4 地址列表'), 'text', _('多个 IPv4 地址用空格分隔；可留空') ]
];

/* Must stay within the cgi-io upload allowance; the backup RPC itself is capped at 64 KiB. */
var MAX_RESTORE_BYTES = 8 * 1024 * 1024;

/**
 * 下载BLOB为文件
 * @param {string} name - 文件名
 * @param {Blob} blob - 要下载的数据
 */
function saveBlob(name, blob) {
	var a = E('a', { 'href': URL.createObjectURL(blob), 'download': name });
	document.body.appendChild(a); a.click(); a.remove();
	URL.revokeObjectURL(a.href);
}

/**
 * 生成数据备份并下载
 * @returns {Promise} 备份操作的Promise
 * @throws {C.OdacError} 如果备份失败
 */
function doBackup() {
	return C.call('backup', {}).then(function(d) {
		if (!d || !d.path) throw new C.OdacError('BAD_RESPONSE', _('后端未返回备份文件路径'));
		return fs.read_direct(d.path, 'blob').then(function(blob) {
			saveBlob('odac-backup-' + new Date((d.created_at || Date.now() / 1000) * 1000).toISOString().slice(0, 10) + '.json', blob);
			C.ok(_('备份已生成并下载（') + (d.size == null ? '?' : d.size) + _(' 字节）'));
		});
	});
}

return view.extend({
	load: function() { return C.load('settings'); },
	render: function(r) {
		var body = [ C.header(_('系统设置'), _('修改运行参数、门户文字、网络设置与 TLS 证书。网络和证书变更会经过预检，应用失败时回滚；已启用时会暂时中断受控客户端连接。')) ];
		if (r.error) { body.push(C.errorBox(r.error, _('设置'))); return E('div', {}, body); }
		var s = r.data || {}, inputs = {};
		var daemonAvailable = s.daemon_available !== false;
		body.push(C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('服务')),
			C.el('p', {}, [ _('认证服务状态：'), C.badge(s.enabled ? 'ACTIVE' : 'DISABLED') ]),
			C.el('p', { 'class': 'cbi-section-descr' }, _('启用或停用会改动网络与防火墙，只能在路由器上运行 /usr/libexec/odac-setup --enable（带预检与回滚）或 /usr/libexec/odac-remove --retain-guard。停用后受控网络保持拒绝（fail-closed），不会放通。')) ]));
		var portalOn = s.portal_enabled !== 0;
		var portalSel = E('select', { 'class': 'cbi-input-select', 'disabled': !daemonAvailable, 'aria-label': _('认证门户') }, [
			E('option', { 'value': '1' }, _('启用认证（设备需登录或加入白名单）')),
			E('option', { 'value': '0' }, _('停用认证（放行所有未进黑名单的设备）'))
		]);
		portalSel.value = portalOn ? '1' : '0';
		body.push(C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('认证门户')),
			C.el('p', { 'class': 'cbi-section-descr' }, _('停用认证后，受控网络内除黑名单设备外的所有设备均可直接访问互联网，客户端无需登录。此设置只影响准入策略，不改动网络与防火墙配置。')),
			C.field(_('认证门户'), portalSel),
			E('button', { 'class': 'btn cbi-button cbi-button-save', 'disabled': !daemonAvailable, 'click': function() {
				var want = portalSel.value === '1';
				var go = function() { C.act('settings_update', { portal_enabled: want ? 1 : 0 }, want ? _('认证门户已启用') : _('认证门户已停用')); };
				if (want === portalOn) return go();
				C.confirm(want ? _('启用认证门户') : _('停用认证门户'),
					want ? _('启用后，设备需要登录账号或加入白名单才能访问互联网。是否继续？')
						: _('停用后，受控网络内除黑名单设备外的所有设备都可直接访问互联网，无需登录。是否继续？'),
					want ? _('启用') : _('停用'), !want, go);
			} }, _('应用门户设置')) ]));
		var sec = C.el('div', { 'class': 'cbi-section' }, C.el('h3', {}, _('限制与周期')));
		EDIT.forEach(function(f) {
			var v = s[f[0]]; if (v != null && f[0] === 'session_seconds') v = Math.round(v / 60);
			var i = E('input', { 'type': 'number', 'class': 'cbi-input-text', 'min': String(f[3]), 'max': String(f[4]), 'value': v == null ? '' : String(v) });
			i.disabled = !daemonAvailable;
			inputs[f[0]] = [ i, f ]; sec.appendChild(C.field(f[1], i, f[5]));
		});
		body.push(sec, E('button', { 'class': 'btn cbi-button cbi-button-save', 'disabled': !daemonAvailable, 'click': function() {
			var p = {};
			for (var k in inputs) {
				var i = inputs[k][0], f = inputs[k][1], n = parseInt(i.value, 10);
				if (i.value === '') continue;
				var err = validateRange(n, f[3], f[4], f[1]);
				if (err) return C.notify(new C.OdacError('INVALID', err));
				p[k] = k === 'session_seconds' ? n * 60 : n;
			}
			C.act('settings_update', p, _('设置已保存'));
		} }, _('保存设置')));
		var texts = {}, tsec = C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('门户页面文字')),
			C.el('p', { 'class': 'cbi-section-descr' }, _('修改客户端登录页上显示的文字。留空则使用默认文字，不支持换行与 HTML。')) ]);
		TEXTS.forEach(function(f) {
			var i = E(f[3] ? 'textarea' : 'input', { 'class': 'cbi-input-text', 'maxlength': String(f[2]), 'rows': '3' });
			i.disabled = !daemonAvailable;
			i.value = s[f[0]] == null ? '' : String(s[f[0]]);
			texts[f[0]] = [ i, f ]; tsec.appendChild(C.field(f[1], i, _('最多 ') + f[2] + _(' 个字符')));
		});
		body.push(tsec, E('button', { 'class': 'btn cbi-button cbi-button-save', 'disabled': !daemonAvailable, 'click': function() {
			var p = {};
			for (var k in texts) {
				var v = texts[k][0].value.replace(/[\r\n\t]+/g, ' ').trim(), f = texts[k][1];
				var err = validateTextLength(v, f[2], f[1]);
				if (err) return C.notify(new C.OdacError('INVALID', err));
				p[k] = v;
			}
			C.act('settings_update', p, _('门户文字已保存'));
		} }, _('保存门户文字')));

		var net = {}, nsec = C.el('div', { 'class': 'cbi-section' }, C.el('h3', {}, _('网络与门户')));
		if (s.config_enabled) body.push(C.el('p', { 'class': 'alert-message warning' }, _('受控网络已启用时不可切换受控接口；请先停用准入，再修改接口并重新执行启用预检。')));
		NET_EDIT.forEach(function(f) {
			var i = E('input', { 'id': 'odac-net-' + f[0], 'type': f[2], 'class': 'cbi-input-text', 'value': s[f[0]] == null ? '' : String(s[f[0]]) });
			if (f[2] === 'number') { i.min = '1025'; i.max = '65535'; i.step = '1'; }
			if (f[0] === 'interface' && s.config_enabled) i.disabled = true;
			net[f[0]] = [ i, f ];
			nsec.appendChild(C.el('div', { 'class': 'odac-field' }, [ E('label', { 'for': i.id }, f[1]), i, C.el('small', {}, f[3]) ]));
		});
		body.push(nsec, E('button', { 'class': 'btn cbi-button cbi-button-save', 'click': function(ev) {
			var p = {}, button = ev.currentTarget;
			for (var k in net) {
				var i = net[k][0], f = net[k][1], v = i.value.trim();
				if (f[2] === 'number') {
					var n = Number(v), err = validateRange(n, 1025, 65535, f[1]);
					if (err || !Number.isInteger(n) || n === 2050) {
						i.focus(); return C.notify(new C.OdacError('INVALID', err || _('端口必须是整数且不可使用 2050')));
					}
				}
				if (!v && k !== 'upstream') { i.focus(); return C.notify(new C.OdacError('INVALID', f[1] + _(' 不能为空'))); }
				p[k] = v;
			}
			if (p.portal_port === p.portal_https_port) return C.notify(new C.OdacError('INVALID', _('HTTP 与 HTTPS 端口不可相同')));
			C.confirm(_('应用网络设置'), _('保存并应用这些设置？已启用时会断开受控客户端会话。失败时自动恢复原配置。'), _('应用'), true, function() {
				button.disabled = true;
				C.runJob('network_update', p).then(function() {
					C.ok(_('网络设置已保存')); window.setTimeout(function() { location.reload(); }, 800);
				}).catch(function(e) { C.notify(e, _('网络设置失败')); }).then(function() { button.disabled = false; });
			});
		} }, _('保存网络设置')));

		var certFile = E('input', { 'id': 'odac-cert', 'type': 'file', 'accept': '.pem,.cer,.crt' });
		var keyFile = E('input', { 'id': 'odac-key', 'type': 'file', 'accept': '.pem,.key' });
		body.push(C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('TLS 证书')),
			C.el('p', {}, _('上传 PEM 证书与私钥。服务器将验证有效期、主机名及密钥匹配关系，应用失败时恢复原证书。')),
			C.el('p', {}, [ _('当前证书：'), C.el('code', {}, s.tls_cert) ]),
			E('label', { 'for': 'odac-cert' }, _('证书文件')), certFile,
			E('label', { 'for': 'odac-key' }, _('私钥文件')), keyFile,
			E('button', { 'class': 'btn cbi-button cbi-button-save', 'click': function(ev) {
				var cf = certFile.files[0], kf = keyFile.files[0], button = ev.currentTarget;
				if (!cf || !kf) return C.notify(new C.OdacError('INVALID', _('请同时选择证书与私钥')));
				if (!cf.size || !kf.size || cf.size > 1048576 || kf.size > 1048576) return C.notify(new C.OdacError('INVALID', _('文件须非空且每个不超过 1 MiB')));
				C.confirm(_('更新 TLS 证书'), _('上传并应用新证书？已启用时会暂时断开受控客户端连接。'), _('上传并应用'), true, function() {
					button.disabled = true;
					ui.uploadFile('/tmp/odac-upload-cert.pem', cf)
						.then(function() { return ui.uploadFile('/tmp/odac-upload-key.pem', kf); })
						.then(function() { return C.runJob('upload_cert', {}); })
						.then(function() { C.ok(_('证书已保存')); window.setTimeout(function() { location.reload(); }, 800); })
						.catch(function(e) { C.notify(e, _('证书更新失败')); })
						.then(function() { button.disabled = false; });
				});
			} }, _('上传并保存证书')) ]));

		var file = E('input', { 'type': 'file', 'accept': '.json,application/json', 'aria-label': _('选择备份文件') });
		var conf = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'placeholder': _('输入“恢复”以确认') });
		body.push(C.el('div', { 'class': 'cbi-section' }, [ C.el('h3', {}, _('数据备份与恢复')),
			C.el('p', {}, _('备份包含账号（仅密码哈希）、设备策略与设置，请妥善保管。')),
			E('button', { 'class': 'btn cbi-button', 'disabled': !daemonAvailable, 'click': function() { doBackup().catch(function(e) { C.notify(e, _('备份失败')); }); } }, _('下载备份')),
			C.el('h4', {}, _('从备份恢复')),
			C.el('p', { 'class': 'alert-message warning' }, _('恢复会替换当前全部账号、设备策略与设置，并断开所有在线会话。执行前会先自动下载一份当前数据备份，若恢复结果不符合预期可用该文件再次恢复。')),
			file, conf,
			E('button', { 'class': 'btn cbi-button cbi-button-negative', 'disabled': !daemonAvailable, 'click': function() {
				var f = file.files[0];
				if (!f) return C.notify(new C.OdacError('INVALID', _('请选择备份文件')));
				if (f.size > MAX_RESTORE_BYTES) return C.notify(new C.OdacError('TOO_LARGE', _('备份文件超过 ') + (MAX_RESTORE_BYTES / 1048576) + ' MiB'));
				if (conf.value.trim() !== '恢复') return C.notify(new C.OdacError('CONFIRM', _('请输入“恢复”以确认')));
				f.text().then(function(t) {
					try { JSON.parse(t); } catch (e) { throw new C.OdacError('INVALID', _('文件不是有效的 JSON 备份')); }
					return doBackup().then(function() { return ui.uploadFile('/tmp/odac-restore.json', f); });
				}).then(function() { return C.call('restore', { path: '/tmp/odac-restore.json' }); })
					.then(function() { C.ok(_('恢复完成')); window.setTimeout(function() { location.reload(); }, 800); },
						function(e) { C.notify(e, _('恢复失败，请检查后端日志并核对当前数据')); });
			} }, _('恢复')) ]));
		return E('div', {}, body);
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
