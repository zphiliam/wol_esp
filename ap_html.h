#pragma once
#include <pgmspace.h>

// ─────────────────────────────────────────────────────────────────────────────
// SoftAP 配置网页（PROGMEM，节省 ESP8266 堆内存）
// ─────────────────────────────────────────────────────────────────────────────
static const char CONFIG_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WoL 设备配置</title>
<style>
:root{--c:#3b82f6;--ch:#2563eb;--ce:#ef4444;--bg:#f1f5f9;--card:#fff;--txt:#1e293b;--sub:#64748b;--bd:#e2e8f0}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);min-height:100vh;display:flex;align-items:center;justify-content:center;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:var(--txt);padding:16px}
.card{background:var(--card);border-radius:12px;box-shadow:0 4px 24px rgba(0,0,0,.09);padding:28px 32px;width:100%;max-width:440px}
h2{font-size:1.2rem;font-weight:700;margin-bottom:4px}
.sub{color:var(--sub);font-size:.82rem;margin-bottom:22px}
.sec{font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:var(--sub);margin:18px 0 10px;padding-bottom:6px;border-bottom:1px solid var(--bd)}
.f{margin-bottom:14px}
label{display:block;font-size:.85rem;font-weight:500;margin-bottom:5px}
input{width:100%;padding:9px 11px;border:1.5px solid var(--bd);border-radius:6px;font-size:.875rem;color:var(--txt);background:#fff;outline:none;transition:border-color .15s}
input:focus{border-color:var(--c)}
input::placeholder{color:#94a3b8}
button{width:100%;padding:11px;background:var(--c);color:#fff;border:none;border-radius:7px;font-size:.95rem;font-weight:600;cursor:pointer;margin-top:10px;transition:background .15s}
button:hover{background:var(--ch)}
.msg{padding:9px 13px;border-radius:6px;font-size:.85rem;margin-bottom:14px}
.err{background:#fef2f2;color:var(--ce);border:1px solid #fecaca}
.ok{background:#f0fdf4;color:#16a34a;border:1px solid #bbf7d0}
footer{text-align:center;color:var(--sub);font-size:.74rem;margin-top:18px}
.pw-wrap{position:relative}.pw-eye{position:absolute!important;right:7px;top:50%;transform:translateY(-50%);width:auto!important;padding:3px!important;background:none!important;border:none!important;margin:0!important;cursor:pointer;color:var(--sub);display:flex;align-items:center;border-radius:4px;line-height:0}.pw-eye:hover{background:#f1f5f9!important;color:var(--txt)}
</style>
</head>
<body>
<div class="card">
<h2>WoL 设备配置</h2>
<p class="sub">连接到此 AP 后在浏览器中打开 192.168.4.1</p>
%STATUS%
<form method="POST" action="/save">
<div class="sec">WiFi</div>
<div class="f"><label>SSID <span style="color:var(--ce)">*</span><button type="button" id="sb" onclick="doScan()" style="float:right;width:auto;padding:3px 10px;font-size:.78rem;margin:0;border-radius:5px">扫描</button></label><div style="position:relative"><input name="ssid" id="si" type="text" maxlength="32" value="%SSID%" required placeholder="WiFi 名称" autocomplete="off"><div id="sl" style="display:none;position:absolute;top:100%;left:0;right:0;background:#fff;border:1.5px solid var(--c);border-radius:6px;margin-top:4px;max-height:200px;overflow-y:auto;z-index:99;box-shadow:0 4px 12px rgba(0,0,0,.12)"></div></div></div>
<div class="f"><label>密码</label><div class="pw-wrap"><input name="wpass" id="pw0" type="password" maxlength="63" value="%WPASS%" placeholder="留空表示无密码" autocomplete="current-password" style="padding-right:38px"><button type="button" class="pw-eye" onclick="tpw('pw0',this)"><svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg></button></div></div>
<div class="sec">WoL 目标</div>
<div class="f"><label>目标 MAC 地址 <span style="color:var(--ce)">*</span></label><input name="wolmac" type="text" maxlength="12" pattern="[0-9A-Fa-f]{12}" value="%WOLMAC%" required placeholder="AABBCCDDEEFF（12 位十六进制，无分隔符）"></div>
%FULLBLOCK%
<button type="submit">保存并重启</button>
</form>
<footer>固件版本 %VERSION%</footer>
</div>
<script>
function doScan(){var b=document.getElementById('sb');b.textContent='扫描中…';b.disabled=true;fetch('/scan').then(function(r){return r.json()}).then(function(list){var d=document.getElementById('sl');d.innerHTML='';list.forEach(function(s){var item=document.createElement('div');item.textContent=s;item.style.cssText='padding:11px 14px;cursor:pointer;font-size:.9rem;border-bottom:1px solid #f1f5f9';item.addEventListener('touchstart',function(){item.style.background='#eff6ff'});item.addEventListener('touchend',function(){document.getElementById('si').value=s;d.style.display='none';item.style.background=''});item.addEventListener('mousedown',function(){document.getElementById('si').value=s;d.style.display='none'});d.appendChild(item)});if(list.length>0)d.style.display='block';b.textContent='重新扫描('+list.length+')';b.disabled=false}).catch(function(){b.textContent='重试';b.disabled=false})}
document.addEventListener('click',function(e){var d=document.getElementById('sl');var si=document.getElementById('si');if(d&&e.target===si){if(d.children.length>0)d.style.display='block';}else if(d){d.style.display='none';}});
window.addEventListener('load',doScan);
var EYE_ON='<svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>';
var EYE_OFF='<svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';
function tpw(id,btn){var inp=document.getElementById(id);var show=inp.type==='password';inp.type=show?'text':'password';btn.innerHTML=show?EYE_OFF:EYE_ON}
</script>
</body>
</html>
)=====";

// MQTT 字段块（ap_full_config=true 时插入 %FULLBLOCK%）
static const char MQTT_FIELDS_HTML[] PROGMEM = R"=====(
<div class="sec">MQTT</div>
<div class="f"><label>服务器地址 <span style="color:var(--ce)">*</span></label><input name="mqttsvr" type="text" maxlength="63" value="%MQTTSVR%" required placeholder="broker.example.com"></div>
<div class="f"><label>端口</label><input name="mqttport" type="number" min="1" max="65535" value="%MQTTPORT%"></div>
<div class="f"><label>用户名</label><input name="mqttuser" type="text" maxlength="63" value="%MQTTUSER%" placeholder="（可选）"></div>
<div class="f"><label>密码</label><div class="pw-wrap"><input name="mqttpass" id="pw1" type="password" maxlength="63" value="%MQTTPASS%" placeholder="（可选）" autocomplete="current-password" style="padding-right:38px"><button type="button" class="pw-eye" onclick="tpw('pw1',this)"><svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg></button></div></div>
<div class="f"><label>Client ID <span style="color:var(--ce)">*</span></label><input name="mqttid" type="text" maxlength="31" value="%MQTTID%" required placeholder="wol-device-1"></div>
)=====";
