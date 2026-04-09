#pragma once
#include <pgmspace.h>

// ─────────────────────────────────────────────────────────────────────────────
// SoftAP 配置网页（PROGMEM）
// 动态数据由前端 fetch GET /config 填充；表单提交 fetch POST /save，响应 JSON。
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
html,body{overscroll-behavior-y:none}
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
#mq{display:none}
</style>
</head>
<body>
<div class="card">
<h2>WoL 设备配置</h2>
<p class="sub">连接到此 AP 后在浏览器中打开 192.168.4.1</p>
<div id="msg"></div>
<form id="frm">
<div class="sec">WiFi</div>
<div class="f"><label>SSID <span style="color:var(--ce)">*</span><button type="button" id="sb" onclick="doScan()" style="float:right;width:auto;padding:3px 10px;font-size:.78rem;margin:0;border-radius:5px">扫描</button></label><div style="position:relative"><input name="ssid" id="si" type="text" maxlength="32" required placeholder="WiFi 名称" autocomplete="off"><div id="sl" style="display:none;position:absolute;top:100%;left:0;right:0;background:#fff;border:1.5px solid var(--c);border-radius:6px;margin-top:4px;max-height:200px;overflow-y:auto;z-index:99;box-shadow:0 4px 12px rgba(0,0,0,.12);overscroll-behavior:contain"></div></div></div>
<div class="f"><label>密码</label><div class="pw-wrap"><input name="wpass" id="pw0" type="password" maxlength="63" placeholder="留空表示无密码" autocomplete="current-password" style="padding-right:38px"><button type="button" class="pw-eye" id="peb0" onclick="tpw('pw0','peb0')"></button></div></div>
<div class="sec">WoL 目标</div>
<div class="f"><label>目标 MAC 地址 <span style="color:var(--ce)">*</span></label><input name="wolmac" type="text" maxlength="12" pattern="[0-9A-Fa-f]{12}" required placeholder="AABBCCDDEEFF（12 位十六进制，无分隔符）"></div>
<div id="mq">
<div class="sec">MQTT</div>
<div class="f"><label>服务器地址 <span style="color:var(--ce)">*</span></label><input name="mqttsvr" type="text" maxlength="63" placeholder="broker.example.com"></div>
<div class="f"><label>端口</label><input name="mqttport" type="number" min="1" max="65535" value="8883"></div>
<div class="f"><label>用户名</label><input name="mqttuser" type="text" maxlength="63" placeholder="（可选）"></div>
<div class="f"><label>密码</label><div class="pw-wrap"><input name="mqttpass" id="pw1" type="password" maxlength="63" placeholder="（可选）" autocomplete="current-password" style="padding-right:38px"><button type="button" class="pw-eye" id="peb1" onclick="tpw('pw1','peb1')"></button></div></div>
<div class="f"><label>Client ID <span style="color:var(--ce)">*</span></label><input name="mqttid" type="text" maxlength="31" placeholder="wol-device-1"></div>
</div>
<button type="submit" id="btn">保存并重启</button>
</form>
<footer id="ver"></footer>
</div>
<script>
var EON='<svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>';
var EOFF='<svg xmlns="http://www.w3.org/2000/svg" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';
document.getElementById('peb0').innerHTML=EON;
document.getElementById('peb1').innerHTML=EON;
function tpw(id,bid){var i=document.getElementById(id),b=document.getElementById(bid),s=i.type==='password';i.type=s?'text':'password';b.innerHTML=s?EOFF:EON}
function q(n){return document.querySelector('[name='+n+']')}
function showMsg(t,ok){document.getElementById('msg').innerHTML='<div class="msg '+(ok?'ok':'err')+'">'+t+'</div>'}
fetch('/config').then(function(r){return r.json()}).then(function(d){
  q('ssid').value=d.ssid||'';
  q('wpass').value=d.wpass||'';
  q('wolmac').value=d.wolmac||'';
  if(d.ap_full_config){
    document.getElementById('mq').style.display='block';
    q('mqttsvr').value=d.mqtt_server||'';
    q('mqttport').value=d.mqtt_port||8883;
    q('mqttuser').value=d.mqtt_user||'';
    q('mqttpass').value=d.mqtt_pass||'';
    q('mqttid').value=d.mqtt_id||'';
  }
  document.getElementById('ver').textContent='固件版本 '+(d.version||'');
}).catch(function(){showMsg('加载配置失败，请刷新页面。',false)});
document.getElementById('frm').addEventListener('submit',function(e){
  e.preventDefault();
  var btn=document.getElementById('btn');
  btn.disabled=true;btn.textContent='保存中…';
  fetch('/save',{method:'POST',body:new URLSearchParams(new FormData(this)),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
  .then(function(r){return r.json()})
  .then(function(d){
    if(d.ok){showMsg('配置已写入，设备正在重启，请稍候…',true);btn.style.display='none';}
    else{showMsg(d.error||'保存失败',false);btn.disabled=false;btn.textContent='保存并重启';}
  }).catch(function(){showMsg('请求失败，请重试。',false);btn.disabled=false;btn.textContent='保存并重启'});
});
function sigBars(r){var l=r>-55?4:r>-66?3:r>-77?2:1,s='<svg width="22" height="14" viewBox="0 0 22 14" style="vertical-align:middle;flex-shrink:0">';for(var i=1;i<=4;i++){var h=i*3+1;s+='<rect x="'+(i-1)*6+'" y="'+(14-h)+'" width="4" height="'+h+'" fill="'+(i<=l?'#3b82f6':'#cbd5e1')+'" rx="1"/>';}return s+'</svg>'}
function doScan(){var b=document.getElementById('sb');b.textContent='扫描中…';b.disabled=true;fetch('/scan').then(function(r){return r.json()}).then(function(list){var seen={},deduped=[];list.forEach(function(n){if(seen[n.ssid]===undefined||n.rssi>seen[n.ssid])seen[n.ssid]=n.rssi});list.forEach(function(n){if(seen[n.ssid]===n.rssi&&!deduped.some(function(x){return x.ssid===n.ssid}))deduped.push(n)});var d=document.getElementById('sl');d.innerHTML='';deduped.forEach(function(net){var item=document.createElement('div');item.style.cssText='padding:9px 12px;cursor:pointer;font-size:.88rem;border-bottom:1px solid #f1f5f9;display:flex;align-items:center;justify-content:space-between;gap:8px';var ns=document.createElement('span');ns.textContent=net.ssid;ns.style.cssText='flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';var ss=document.createElement('span');ss.innerHTML=sigBars(net.rssi);ss.title=net.rssi+' dBm';item.appendChild(ns);item.appendChild(ss);item.addEventListener('mouseover',function(){item.style.background='#f8fafc'});item.addEventListener('mouseout',function(){item.style.background=''});var scrolled=false,startY=0;item.addEventListener('touchstart',function(e){startY=e.touches[0].clientY;scrolled=false});item.addEventListener('touchmove',function(e){if(Math.abs(e.touches[0].clientY-startY)>10)scrolled=true});item.addEventListener('touchend',function(){if(!scrolled){document.getElementById('si').value=net.ssid;d.style.display='none'}});item.addEventListener('mousedown',function(){document.getElementById('si').value=net.ssid;d.style.display='none'});d.appendChild(item)});if(deduped.length>0)d.style.display='block';b.textContent='重新扫描('+deduped.length+')';b.disabled=false}).catch(function(){b.textContent='重试';b.disabled=false})}
document.addEventListener('click',function(e){var d=document.getElementById('sl'),si=document.getElementById('si');if(d&&e.target===si){if(d.children.length>0)d.style.display='block';}else if(d){d.style.display='none';}});
window.addEventListener('load',doScan);
</script>
</body>
</html>
)=====";
