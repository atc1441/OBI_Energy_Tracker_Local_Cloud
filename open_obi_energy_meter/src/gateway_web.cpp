// gateway_web.cpp — WiFi captive portal + web dashboard + MQTT.
// Runs on its own FreeRTOS task pinned to core 0 so the HTTP/MQTT work never stutters the
// LoRa gateway (which runs on the Arduino loop, core 1).
#include "gateway_web.h"
#include "reader.h"
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <WebServer.h>
#include <PubSubClient.h>         // knolleary/PubSubClient
#include <Preferences.h>

static WebServer    server(80);
static WiFiClient    net;
static PubSubClient  mqtt(net);
static Preferences   prefs;
static WiFiManager   wm;
static WiFiManagerParameter *pHost, *pPort, *pUser, *pPass, *pTopic;
static bool     g_serverUp = false;
static bool     g_wifiOk   = false;

static char     g_mqttHost[64] = "";
static uint16_t g_mqttPort = 1883;
static char     g_mqttUser[32] = "";
static char     g_mqttPass[32] = "";
static char     g_mqttTopic[48] = "obi/gateway";
static uint16_t g_pubEvery = 15;

static uint32_t g_mqttLastPubMs = 0;
static uint32_t g_mqttPubCount  = 0;
static int      g_mqttState     = -1;

// ---------------------------------------------------------------- helpers
static String hex(const uint8_t *p, size_t n, const char *sep = "") {
  static const char *H = "0123456789abcdef";
  String s; s.reserve(n * (2 + strlen(sep)));
  for (size_t i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; if (i + 1 < n) s += sep; }
  return s;
}
static String uuidStr(const uint8_t *u) {
  return hex(u, 4) + "-" + hex(u + 4, 2) + "-" + hex(u + 6, 2) + "-" + hex(u + 8, 2) + "-" + hex(u + 10, 6);
}
static const char *typeName(uint8_t t) { return t == 0x11 ? "outlet" : "meter"; }
static String jnum(uint32_t v) { return obi_na(v) ? String("null") : String((unsigned long)v); }
static const char *mqttStateText(int s) {
  switch (s) {
    case -4: return "timeout"; case -3: return "conn lost"; case -2: return "connect failed";
    case -1: return "disconnected"; case 0: return "connected"; case 1: return "bad protocol";
    case 2: return "bad client id"; case 3: return "unavailable"; case 4: return "bad credentials";
    case 5: return "unauthorized"; default: return "?";
  }
}
static String jstr(const char *s) { String o = "\""; for (; *s; s++) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; } return o + "\""; }

static String readersJson() {
  // Sort used readers by their 3-byte handle so the web UI always lists them in
  // the same, stable order (matches the shown "id").
  int order[MAX_READERS], n = 0;
  for (int i = 0; i < MAX_READERS; i++) if (readers[i].used) order[n++] = i;
  for (int a = 0; a < n - 1; a++)
    for (int b = 0; b < n - 1 - a; b++)
      if (memcmp(readers[order[b]].handle, readers[order[b + 1]].handle, 3) > 0) {
        int t = order[b]; order[b] = order[b + 1]; order[b + 1] = t;
      }

  String j = "[";
  bool first = true;
  for (int k = 0; k < n; k++) {
    Reader &r = readers[order[k]];
    if (!first) j += ",";
    first = false;
    uint32_t age = (millis() - r.lastSeenMs) / 1000;
    j += "{\"id\":\"" + hex(r.handle, 3) + "\"";
    j += ",\"uuid\":" + (r.haveUuid ? "\"" + uuidStr(r.uuid) + "\"" : "null");
    j += ",\"type\":\"" + String(typeName(r.devType)) + "\"";
    j += ",\"paired\":" + String(r.haveKey ? "true" : "false");
    j += ",\"legacy\":" + String(r.legacy ? "true" : "false");
    j += ",\"softver\":" + String(r.softver) + ",\"hardver\":" + String(r.hardver);
    j += ",\"battery_mV\":" + String(r.battery_mV);
    j += ",\"rssi\":" + String((int)r.lastRssi);
    j += ",\"infrared\":" + String((r.flags & 1) ? "true" : "false");
    j += ",\"import\":" + jnum(r.import_) + ",\"export\":" + jnum(r.export_) + ",\"power\":" + jnum(r.power);
    j += ",\"has_data\":" + String(r.haveData ? "true" : "false");
    j += ",\"bootloader\":" + String(r.inBootloader ? "true" : "false");
    j += ",\"age_s\":" + String(age);
    j += ",\"interval\":" + String(r.setInterval);
    j += "}";
  }
  return j + "]";
}

static String statusJson() {
  bool con = mqtt.connected();
  long lastPub = g_mqttLastPubMs ? (long)((millis() - g_mqttLastPubMs) / 1000) : -1;
  String j = "{";
  j += "\"gwid\":\"" + hex(GWID, 6) + "\"";
  j += ",\"gwid_ascii\":" + jstr(String((const char *)GWID).substring(0, 6).c_str());
  j += ",\"uptime_s\":" + String(gw_uptime_s());
  j += ",\"wifi\":" + String(g_wifiOk ? "true" : "false");
  j += ",\"ip\":\"" + (g_wifiOk ? WiFi.localIP().toString() : String("-")) + "\"";
  j += ",\"wifi_rssi\":" + String(g_wifiOk ? WiFi.RSSI() : 0);
  j += ",\"freq_mhz\":869.5,\"bw_khz\":500,\"sf\":7";
  j += ",\"mqtt\":{\"host\":" + jstr(g_mqttHost) + ",\"port\":" + String(g_mqttPort);
  j += ",\"user\":" + jstr(g_mqttUser) + ",\"topic\":" + jstr(g_mqttTopic);
  j += ",\"enabled\":" + String(g_mqttHost[0] ? "true" : "false");
  j += ",\"connected\":" + String(con ? "true" : "false");
  j += ",\"state\":" + jstr(mqttStateText(g_mqttState));
  j += ",\"pub_count\":" + String(g_mqttPubCount);
  j += ",\"last_pub_s\":" + String(lastPub) + "}";
  // reader OTA status
  uint8_t ot[3] = {0}; gw_ota_target(ot);
  j += ",\"ota\":{\"active\":" + String(gw_ota_active() ? "true" : "false");
  j += ",\"target\":\"" + hex(ot, 3) + "\",\"size\":" + String(gw_ota_size());
  j += ",\"served\":" + String(gw_ota_progress()) + "}}";
  return j;
}

// ---------------------------------------------------------------- dashboard
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>OBI LoRa Gateway</title>
<style>
:root{--bg:#0a0e14;--panel:#141a23;--panel2:#1b2430;--line:#232e3c;--txt:#eaf0f7;--dim:#7d8da0;
--accent:#31d07a;--accent2:#12a35a;--amber:#e3b341;--red:#f0616d;--blue:#5aa9ff;
--mono:ui-monospace,SFMono-Regular,Menlo,monospace}
*{box-sizing:border-box}
body{margin:0;background:
 radial-gradient(1200px 500px at 80% -10%,#12351f33,transparent),
 radial-gradient(900px 400px at -10% 0%,#0e2a4a33,transparent),var(--bg);
 color:var(--txt);font:14.5px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;min-height:100vh}
header{padding:14px 20px;display:flex;align-items:center;gap:13px;position:sticky;top:0;z-index:5;
 background:#0a0e14cc;backdrop-filter:blur(10px);border-bottom:1px solid var(--line)}
.logo{width:36px;height:36px;border-radius:10px;display:grid;place-items:center;font-size:19px;
 background:linear-gradient(135deg,var(--accent),var(--accent2));box-shadow:0 0 18px #31d07a55}
h1{font-size:16px;margin:0;font-weight:650}.sub{color:var(--dim);font-size:12px}
.spacer{flex:1}
.seg{display:flex;background:var(--panel2);border:1px solid var(--line);border-radius:9px;overflow:hidden}
.seg button{background:transparent;border:0;color:var(--dim);padding:7px 12px;cursor:pointer;font-size:13px;font-weight:600}
.seg button.act{background:var(--accent);color:#04140a}
.icon{background:var(--panel2);border:1px solid var(--line);color:var(--dim);border-radius:9px;padding:7px 11px;cursor:pointer;font-size:15px}
.wrap{max-width:860px;margin:0 auto;padding:20px}
.bar{display:flex;flex-wrap:wrap;gap:9px;margin-bottom:16px}
.pill{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:8px 13px;font-size:12.5px}
.pill .k{color:var(--dim);margin-right:7px;font-weight:600}.pill b{font-family:var(--mono)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.on{background:var(--accent);box-shadow:0 0 8px var(--accent)}.off{background:var(--red)}.idle{background:var(--dim)}
.list{display:flex;flex-direction:column;gap:12px}
.card{background:linear-gradient(180deg,#161d27,#131922);border:1px solid var(--line);border-radius:15px;
 padding:14px 16px;transition:.15s;position:relative;overflow:hidden}
.card::before{content:"";position:absolute;left:0;top:0;bottom:0;width:3px;background:linear-gradient(var(--accent),var(--accent2))}
.card:hover{border-color:#31527a;transform:translateY(-1px)}
.hd{display:flex;align-items:center;gap:9px;flex-wrap:wrap}
.hd .id{font-family:var(--mono);font-size:17px;font-weight:700;letter-spacing:1.5px}
.tag{font-size:10px;padding:3px 8px;border-radius:20px;border:1px solid var(--line);color:var(--dim);
 text-transform:uppercase;letter-spacing:.8px;font-weight:700}
.tag.meter{color:var(--blue);border-color:#26496f;background:#1a2c4022}
.tag.outlet{color:var(--amber);border-color:#5a4713;background:#3a2e0a22}
.meta{color:var(--dim);font-size:12px;font-family:var(--mono)}
.uuid{font-family:var(--mono);font-size:11.5px;color:var(--dim);word-break:break-all;margin:7px 0 13px;
 padding:6px 9px;background:#0d131b;border-radius:8px;border:1px solid #1a222d}
.uuid b{color:#9fb0c4;letter-spacing:.5px}
.mx{display:grid;grid-template-columns:repeat(auto-fit,minmax(92px,1fr));gap:7px}
.mc{background:#0f151d;border:1px solid #1a222d;border-radius:9px;padding:8px 10px}
.mc .l{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:.7px;font-weight:600;margin-bottom:4px}
.mc .v{font-family:var(--mono);font-size:15px;font-weight:600}
.mc .v small{font-size:11px;color:var(--dim);font-weight:400}
.v.na{color:#3d4a5a}
.batt{height:5px;background:#0a0e14;border-radius:3px;overflow:hidden;margin-top:6px}
.batt>i{display:block;height:100%;border-radius:3px;transition:.3s}
.ctrl{display:flex;gap:8px;margin-top:12px;align-items:center;flex-wrap:wrap}
input{background:var(--panel2);border:1px solid var(--line);color:var(--txt);border-radius:8px;padding:8px 10px;font-family:var(--mono);font-size:13px}
.ctrl input{width:92px}
button.b{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#04140a;border:0;border-radius:8px;padding:8px 14px;font-weight:700;cursor:pointer}
button.g{background:transparent;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px 13px;cursor:pointer;font-size:13px}
.hint{color:var(--amber);font-size:12px;margin-top:9px;display:flex;gap:6px;align-items:center}
.boot{background:linear-gradient(90deg,#3a2a05,#2a1f08);border:1px solid #7a5a12;color:#f5c451;
 font-size:12px;font-weight:600;padding:7px 11px;border-radius:9px;margin:0 0 12px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:16px;display:none}
.panel.open{display:block}
.panel h2{margin:0 0 3px;font-size:15px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px 14px;margin:12px 0}
.grid2 label{font-size:11.5px;color:var(--dim);display:block;margin-bottom:5px}
.grid2 input{width:100%}
footer{color:var(--dim);font-size:12px;text-align:center;padding:26px}
footer b{font-family:var(--mono);color:var(--txt)}
</style></head><body>
<header><div class="logo">⚡</div><div><h1>OBI LoRa Gateway</h1><div class="sub" id="sub"></div></div>
<div class="spacer"></div>
<div class="seg"><button id="lde" onclick="setLang('de')">DE</button><button id="len" onclick="setLang('en')">EN</button></div>
<button class="icon" onclick="tog('mq')" title="MQTT">⚙</button></header>
<div class="wrap">
 <div class="panel" id="mq">
  <h2 id="mqtt_h"></h2><div class="sub" id="mqtt_stat"></div>
  <div class="grid2">
   <div><label id="l_host"></label><input id="c_host" placeholder="192.168.1.10"></div>
   <div><label id="l_port"></label><input id="c_port" type="number"></div>
   <div><label id="l_user"></label><input id="c_user"></div>
   <div><label id="l_pass"></label><input id="c_pass" type="password" placeholder="••••"></div>
   <div style="grid-column:1/3"><label id="l_topic"></label><input id="c_topic" style="width:100%"></div>
  </div>
  <button class="b" onclick="saveMqtt()" id="b_save"></button>
  <div class="sub" id="mqtt_cmd" style="margin-top:10px"></div>
 </div>
 <div class="bar" id="bar"></div>
 <div class="list" id="list"></div>
 <footer><span id="ft"></span> · <b id="gw"></b></footer>
</div>
<script>
const $=s=>document.querySelector(s);
const T={
 de:{sub:'869,5 MHz · SF7 · liest deine Zähler direkt',wifi:'WLAN',mqtt:'MQTT',radio:'Funk',readers:'Reader',
  offline:'offline',fw:'Firmware',batt:'Batterie',opt:'Sensor',active:'aktiv',nosig:'kein Signal',
  imp:'Bezug',exp:'Einspeisung',pow:'Leistung',seen:'Zuletzt',before:'vor',setiv:'Intervall',sec:'Sek.',
  none:'Noch keine Reader',waiting:'Warte auf einen Reader — Reader-Taste ~10 s halten (echtes Gateway aus).',
  uuidwait:'UUID noch unbekannt — Reader per Taste neu verbinden',
  irhint:'Noch keine Messwerte — Reader-Taste einmal kurz drücken, um die Infrarot-Lesung zu starten.',
  uptime:'Laufzeit',mqcfg:'MQTT-Einstellungen',host:'Server',port:'Port',user:'Benutzer',pass:'Passwort',
  topic:'Basis-Topic',save:'Speichern',con:'verbunden',dis:'getrennt',lastpub:'zuletzt gesendet',
  msgs:'Nachr.',never:'noch nichts',disabled:'deaktiviert',ago:'her',
  cmdhint:'Intervall per MQTT setzen (Sekunden als Payload):',
  boot:'Bootloader-Modus — bereit zum Flashen',
  flash:'Firmware flashen',pick:'Erst eine .bin auswählen',uploading:'lädt hoch…',armed:'bereit — Reader startet neu & zieht',
  otarun:'OTA läuft',otawarn:'⚠ Reader-Firmware über LoRa flashen — nur mit gültiger .bin. Der Reader validiert vor dem Schreiben.',
  vnote:'Die Firmware-Version MUSS im Dateinamen stehen, z.B. reader_v55.bin.',
  novers:'Keine Version im Dateinamen gefunden.\nBitte die richtige Firmware-Version in den Dateinamen schreiben, z.B. reader_v55.bin',
  vmiss:'Version fehlt im Dateinamen',
  samever:'Der Reader läuft bereits auf v%v — er akzeptiert dieselbe Version nicht (kein Reflash).\n\nTrotzdem versuchen?'},
 en:{sub:'869.5 MHz · SF7 · reading your meters directly',wifi:'WiFi',mqtt:'MQTT',radio:'Radio',readers:'Readers',
  offline:'offline',fw:'Firmware',batt:'Battery',opt:'Sensor',active:'active',nosig:'no signal',
  imp:'Import',exp:'Export',pow:'Power',seen:'Last seen',before:'',setiv:'Interval',sec:'sec',
  none:'No readers yet',waiting:'Waiting for a reader — hold its button ~10 s (real gateway off).',
  uuidwait:'UUID unknown yet — reconnect the reader with its button',
  irhint:'No readings yet — tap the reader button once to start its infrared readout.',
  uptime:'uptime',mqcfg:'MQTT settings',host:'Server',port:'Port',user:'User',pass:'Password',
  topic:'Base topic',save:'Save',con:'connected',dis:'disconnected',lastpub:'last publish',
  msgs:'msgs',never:'nothing yet',disabled:'disabled',ago:'ago',
  cmdhint:'Set interval via MQTT (seconds as payload):',
  boot:'Bootloader mode — ready to flash',
  flash:'Flash firmware',pick:'Pick a .bin first',uploading:'uploading…',armed:'armed — reader will reset & pull',
  otarun:'OTA in progress',otawarn:'⚠ Flash reader firmware over LoRa — use a valid .bin only. The reader validates before writing.',
  vnote:'The firmware version MUST be in the filename, e.g. reader_v55.bin.',
  novers:'No version found in the filename.\nPut the correct firmware version in the filename, e.g. reader_v55.bin',
  vmiss:'version missing in filename',
  samever:'The reader is already on v%v — it will not accept the same version (no reflash).\n\nTry anyway?'}};
let lang=localStorage.getItem('lang')||'de',L=T[lang];
function setLang(x){lang=x;L=T[x];localStorage.setItem('lang',x);applyLang();tick();}
function applyLang(){$('#lde').className=lang=='de'?'act':'';$('#len').className=lang=='en'?'act':'';
 $('#sub').textContent=L.sub;$('#mqtt_h').textContent=L.mqcfg;$('#l_host').textContent=L.host;
 $('#l_port').textContent=L.port;$('#l_user').textContent=L.user;$('#l_pass').textContent=L.pass;
 $('#l_topic').textContent=L.topic;$('#b_save').textContent=L.save;}
function tog(id){$('#'+id).classList.toggle('open')}
const nf=n=>n.toLocaleString(lang=='de'?'de-DE':'en-US');
function val(v,unit){return v===null?`<span class="v na">–</span>`:`<span class="v">${nf(v)}${unit?' <small>'+unit+'</small>':''}</span>`}
// import/export come as raw Wh -> show kWh (raw/1000) with 3 decimals
function valE(v){return v===null?`<span class="v na">–</span>`:`<span class="v">${(v/1000).toLocaleString(lang=='de'?'de-DE':'en-US',{minimumFractionDigits:3,maximumFractionDigits:3})} <small>kWh</small></span>`}
function bcol(p){return p>50?'var(--accent)':p>20?'var(--amber)':'var(--red)'}
function fmt(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+(h?h+'h ':'')+m+'m'}
let cfgLoaded=false;
async function tick(){try{
 const st=await (await fetch('/api/status')).json(), rs=await (await fetch('/api/readers')).json();
 $('#gw').textContent=st.gwid_ascii+' · '+st.gwid;$('#ft').textContent=L.uptime+' '+fmt(st.uptime_s);
 const q=st.mqtt;
 const mqp=!q.enabled?`<span class="dot idle"></span>${L.disabled}`
  :q.connected?`<span class="dot on"></span>${q.host} · ${q.pub_count} ${L.msgs} · ${q.last_pub_s<0?L.never:L.lastpub+' '+q.last_pub_s+'s '+L.ago}`
  :`<span class="dot off"></span>${q.host} · ${q.state}`;
 $('#bar').innerHTML=[
  [L.wifi,st.wifi?`<span class="dot on"></span>${st.ip} · ${st.wifi_rssi} dBm`:`<span class="dot off"></span>${L.offline}`],
  [L.mqtt,mqp],
  [L.radio,`<span class="dot on"></span>${st.freq_mhz} MHz · SF${st.sf} · BW${st.bw_khz}`],
  [L.readers,`<b>${rs.length}</b>`]
 ].map(p=>`<div class="pill"><span class="k">${p[0]}</span>${p[1]}</div>`).join('');
 $('#mqtt_stat').innerHTML=q.enabled?(q.connected?`<span class="dot on"></span>${L.con}`:`<span class="dot off"></span>${L.dis} — ${q.state}`):`<span class="dot idle"></span>${L.disabled}`;
 $('#mqtt_cmd').innerHTML=`${L.cmdhint}<br><code>${q.topic}/&lt;id&gt;/set_interval</code> ← <code>30</code>`;
 if(!cfgLoaded){cfgLoaded=true;$('#c_host').value=q.host;$('#c_port').value=q.port;$('#c_user').value=q.user;$('#c_topic').value=q.topic;}
 // don't blow away a reader card while its interval/file input is focused (you'd never finish typing)
 const ae=document.activeElement, editing=ae&&/^(iv_|fw_)/.test(ae.id||'');
 if(!editing && !uploading) $('#list').innerHTML=rs.length?rs.map(card).join(''):`<div class="card"><div class="hd"><span class="id">—</span></div><div class="uuid" style="border:0;background:0">${L.waiting}</div></div>`;
 if(st.ota&&st.ota.active){const el=$('#op_'+st.ota.target);if(el){const p=st.ota.size?Math.min(100,Math.max(0,Math.round(st.ota.served/st.ota.size*100))):0;el.textContent=L.otarun+' '+p+'%';}}
}catch(e){}}
function card(r){
 const p=Math.max(0,Math.min(100,Math.round((r.battery_mV-2400)/8)));
 const sens=r.infrared?`<span class="dot on"></span>${L.active}`:`<span class="dot idle"></span>${L.nosig}`;
 return `<div class="card">
  <div class="hd"><span class="id">${r.id.toUpperCase()}</span>
   <span class="tag ${r.type}">${r.type}</span>
   <span class="meta">v${r.softver}.${r.hardver}${r.legacy?' · legacy':''} · ${r.rssi} dBm${r.paired?' · 🔒':''}</span></div>
  <div class="uuid">${r.uuid?('<b>UUID</b> '+r.uuid):L.uuidwait}</div>
  ${r.bootloader?`<div class="boot">⚙ ${L.boot}</div>`:''}
  <div class="mx">
   <div class="mc"><div class="l">${L.imp}</div>${valE(r.import)}</div>
   <div class="mc"><div class="l">${L.exp}</div>${valE(r.export)}</div>
   <div class="mc"><div class="l">${L.pow}</div>${val(r.power,'W')}</div>
   <div class="mc"><div class="l">${L.batt}</div><span class="v">${(r.battery_mV/1000).toFixed(2)} <small>V</small></span>
    <div class="batt"><i style="width:${p}%;background:${bcol(p)}"></i></div></div>
   <div class="mc"><div class="l">${L.opt}</div><span class="v" style="font-size:13px">${sens}</span></div>
   <div class="mc"><div class="l">${L.seen}</div><span class="v">${L.before} ${r.age_s}s</span></div>
  </div>
  <div class="ctrl"><input type="number" id="iv_${r.id}" placeholder="${L.sec}" min="5" value="${r.interval||''}">
   <button class="g" onclick="setIv('${r.id}')">${L.setiv}</button>
   <input type="file" id="fw_${r.id}" accept=".bin" style="flex:1 1 200px;min-width:170px;padding:6px 8px;font-size:12px">
   <button class="g" onclick="doOta('${r.id}',${r.softver||0})">${L.flash}</button>
   <span class="meta" id="op_${r.id}"></span></div>
  ${!r.has_data&&!r.bootloader?`<div class="hint">⚠ ${L.irhint}</div>`:''}
 </div>`;
}
async function setIv(id){const v=$('#iv_'+id).value;if(!v)return;await fetch('/api/interval?id='+id+'&seconds='+v,{method:'POST'});tick();}
let uploading=false;
async function doOta(id,cur){
 const f=$('#fw_'+id).files[0]; if(!f){$('#op_'+id).textContent=L.pick;return;}
 // version comes from the filename (…v55.bin); the reader needs it to decide whether to reflash.
 const vm=f.name.match(/v(\d+)/);
 if(!vm){ alert(L.novers); $('#op_'+id).textContent=L.vmiss; return; }
 const ver=parseInt(vm[1])&255;
 const dl=lang=='de'?'Datei':'File';
 if(!confirm(L.otawarn+'\n\n'+L.vnote+'\n\nReader: '+id.toUpperCase()+'\n'+dl+': '+f.name+'\nVersion: v'+ver)) return;
 // the reader refuses an OTA to the version it is already running -> warn but let the user force it.
 if(cur&&ver===cur){ if(!confirm(L.samever.replace('%v',ver))) return; }
 const buf=await f.arrayBuffer(); const fd=new FormData(); fd.append('fw',new Blob([buf]),f.name);
 uploading=true; $('#op_'+id).textContent=L.uploading;
 try{ const r=await (await fetch('/api/ota?id='+id+'&size='+buf.byteLength+'&ver='+ver,{method:'POST',body:fd})).json();
   $('#op_'+id).textContent=r.ok?L.armed:'error'; }
 catch(e){ $('#op_'+id).textContent='error'; }
 uploading=false;
}
async function saveMqtt(){const b=new URLSearchParams();b.set('host',$('#c_host').value);b.set('port',$('#c_port').value);
 b.set('user',$('#c_user').value);b.set('topic',$('#c_topic').value);if($('#c_pass').value)b.set('pass',$('#c_pass').value);
 await fetch('/api/mqtt',{method:'POST',headers:{'content-type':'application/x-www-form-urlencoded'},body:b});$('#c_pass').value='';tick();}
applyLang();tick();setInterval(tick,2000);
</script></body></html>
)HTML";

// ---------------------------------------------------------------- routes
static void handleReaders() { server.send(200, "application/json", readersJson()); }
static void handleStatus()  { server.send(200, "application/json", statusJson()); }
static void handleIndex()   { server.send_P(200, "text/html", INDEX_HTML); }

static void handleInterval() {
  String id = server.arg("id"); long secs = server.arg("seconds").toInt();
  if (id.length() == 6 && secs >= 1 && secs <= 65535) {
    uint8_t h[3];
    for (int i = 0; i < 3; i++) h[i] = (uint8_t)strtol(id.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    gw_request_interval(h, (uint16_t)secs);
    server.send(200, "application/json", "{\"ok\":true}");
  } else server.send(400, "application/json", "{\"ok\":false}");
}

static void handleMqttCfg() {
  if (server.hasArg("host")) strlcpy(g_mqttHost, server.arg("host").c_str(), sizeof g_mqttHost);
  if (server.hasArg("port")) g_mqttPort = server.arg("port").toInt();
  if (server.hasArg("user")) strlcpy(g_mqttUser, server.arg("user").c_str(), sizeof g_mqttUser);
  if (server.hasArg("pass")) strlcpy(g_mqttPass, server.arg("pass").c_str(), sizeof g_mqttPass);
  if (server.hasArg("topic")) strlcpy(g_mqttTopic, server.arg("topic").c_str(), sizeof g_mqttTopic);
  prefs.begin("obigw", false);
  prefs.putString("h", g_mqttHost); prefs.putUShort("p", g_mqttPort);
  prefs.putString("u", g_mqttUser); prefs.putString("pw", g_mqttPass); prefs.putString("t", g_mqttTopic);
  prefs.end();
  mqtt.disconnect(); mqtt.setServer(g_mqttHost, g_mqttPort);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---- reader firmware OTA upload (multipart, .bin) --------------------------
static bool s_otaOk = false;
static void handleOtaUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String id = server.arg("id");
    uint32_t size = server.arg("size").toInt();
    uint8_t ver = (uint8_t)server.arg("ver").toInt();
    s_otaOk = false;
    if (id.length() == 6 && size) {
      uint8_t h[3];
      for (int i = 0; i < 3; i++) h[i] = (uint8_t)strtol(id.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
      s_otaOk = gw_ota_begin(h, size, ver);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_otaOk) gw_ota_write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_otaOk) gw_ota_arm();
  }
}
static void handleOtaDone()   { server.send(200, "application/json", gw_ota_active() ? "{\"ok\":true}" : "{\"ok\":false}"); }
static void mqttCallback(char *topic, byte *payload, unsigned int len);   // fwd: <base>/<id>/set_interval
static void handleOtaCancel() { gw_ota_cancel(); server.send(200, "application/json", "{\"ok\":true}"); }

// ---------------------------------------------------------------- services
static void startServices() {
  if (g_serverUp) return;
  strlcpy(g_mqttHost, pHost->getValue(), sizeof g_mqttHost);
  g_mqttPort = atoi(pPort->getValue());
  strlcpy(g_mqttUser, pUser->getValue(), sizeof g_mqttUser);
  strlcpy(g_mqttPass, pPass->getValue(), sizeof g_mqttPass);
  strlcpy(g_mqttTopic, pTopic->getValue(), sizeof g_mqttTopic);
  prefs.begin("obigw", false);
  prefs.putString("h", g_mqttHost); prefs.putUShort("p", g_mqttPort);
  prefs.putString("u", g_mqttUser); prefs.putString("pw", g_mqttPass); prefs.putString("t", g_mqttTopic);
  prefs.end();

  server.on("/", handleIndex);
  server.on("/api/readers", handleReaders);
  server.on("/api/status", handleStatus);
  server.on("/api/interval", HTTP_POST, handleInterval);
  server.on("/api/mqtt", HTTP_POST, handleMqttCfg);
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);   // multipart .bin upload
  server.on("/api/ota_cancel", HTTP_POST, handleOtaCancel);
  server.begin();
  mqtt.setServer(g_mqttHost, g_mqttPort);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);          // <base>/<id>/set_interval command topic
  g_serverUp = true; g_wifiOk = true;
  Serial.printf("[web] WiFi ok, dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---- MQTT command RX: publish "<seconds>" to <base>/<id>/set_interval ------
// e.g.  mosquitto_pub -t obi/gateway/238d4e/set_interval -m 30
static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic);
  String base = String(g_mqttTopic) + "/";
  if (!t.startsWith(base)) return;
  String rest = t.substring(base.length());              // "<id>/set_interval"
  int slash = rest.indexOf('/');
  if (slash != 6) return;                                // id must be exactly 6 hex chars
  String id = rest.substring(0, slash), cmd = rest.substring(slash + 1);
  char buf[16]; unsigned n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n); buf[n] = 0;
  long secs = atol(buf);
  if (cmd == "set_interval" && secs >= 1 && secs <= 65535) {
    uint8_t h[3];
    for (int i = 0; i < 3; i++) h[i] = (uint8_t)strtol(id.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    gw_request_interval(h, (uint16_t)secs);
    Serial.printf("[mqtt] rx set_interval %s -> %ld s\n", id.c_str(), secs);
  } else {
    Serial.printf("[mqtt] rx ignored topic=%s payload=%s\n", topic, buf);
  }
}

static void mqttService() {
  static uint32_t lastTry = 0, lastPub = 0;
  uint32_t now = millis();
  g_mqttState = mqtt.state();
  if (g_mqttHost[0] && !mqtt.connected() && now - lastTry > 8000) {
    lastTry = now;
    String cid = "obi-gw-" + hex(GWID, 3);
    if (mqtt.connect(cid.c_str(), g_mqttUser[0] ? g_mqttUser : nullptr, g_mqttPass[0] ? g_mqttPass : nullptr)) {
      String sub = String(g_mqttTopic) + "/+/set_interval";     // one wildcard level = reader id
      mqtt.subscribe(sub.c_str());
      Serial.printf("[mqtt] connected, subscribed %s\n", sub.c_str());
    }
    g_mqttState = mqtt.state();
  }
  if (mqtt.connected()) {
    mqtt.loop();
    if (now - lastPub > (uint32_t)g_pubEvery * 1000) {
      lastPub = now;
      for (int i = 0; i < MAX_READERS; i++) {
        Reader &r = readers[i];
        if (!r.used || !r.haveData) continue;
        String topic = String(g_mqttTopic) + "/" + hex(r.handle, 3);
        String p = "{\"id\":\"" + hex(r.handle, 3) + "\",\"uuid\":" +
                   (r.haveUuid ? "\"" + uuidStr(r.uuid) + "\"" : "null") +
                   ",\"type\":\"" + typeName(r.devType) + "\",\"battery_mV\":" + r.battery_mV +
                   ",\"rssi\":" + (int)r.lastRssi + ",\"infrared\":" + ((r.flags & 1) ? "true" : "false") +
                   ",\"import\":" + jnum(r.import_) + ",\"export\":" + jnum(r.export_) +
                   ",\"power\":" + jnum(r.power) + "}";
        if (mqtt.publish(topic.c_str(), p.c_str())) g_mqttPubCount++;
      }
      g_mqttLastPubMs = now;
    }
  }
}

// ---------------------------------------------------------------- task
static void webTask(void *) {
  prefs.begin("obigw", true);
  prefs.getString("h", "").toCharArray(g_mqttHost, sizeof g_mqttHost);
  g_mqttPort = prefs.getUShort("p", 1883);
  prefs.getString("u", "").toCharArray(g_mqttUser, sizeof g_mqttUser);
  prefs.getString("pw", "").toCharArray(g_mqttPass, sizeof g_mqttPass);
  { String t = prefs.getString("t", "obi/gateway"); t.toCharArray(g_mqttTopic, sizeof g_mqttTopic); }
  prefs.end();

  static char portBuf[8]; snprintf(portBuf, sizeof portBuf, "%u", g_mqttPort);
  pHost  = new WiFiManagerParameter("host", "MQTT host (blank = off)", g_mqttHost, sizeof g_mqttHost - 1);
  pPort  = new WiFiManagerParameter("port", "MQTT port", portBuf, 6);
  pUser  = new WiFiManagerParameter("user", "MQTT user", g_mqttUser, sizeof g_mqttUser - 1);
  pPass  = new WiFiManagerParameter("pass", "MQTT pass", g_mqttPass, sizeof g_mqttPass - 1);
  pTopic = new WiFiManagerParameter("topic", "MQTT base topic", g_mqttTopic, sizeof g_mqttTopic - 1);
  wm.addParameter(pHost); wm.addParameter(pPort); wm.addParameter(pUser);
  wm.addParameter(pPass); wm.addParameter(pTopic);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);
  Serial.println("[web] WiFi: joining saved network or opening portal 'OBI-Gateway-Setup' (192.168.4.1)");
  if (wm.autoConnect("OBI-Gateway-Setup")) startServices();

  for (;;) {
    wm.process();
    if (!g_serverUp && WiFi.status() == WL_CONNECTED) startServices();
    if (g_serverUp) { server.handleClient(); mqttService(); }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void web_setup() { xTaskCreatePinnedToCore(webTask, "web", 12288, nullptr, 1, nullptr, 0); }
void web_loop() {}
