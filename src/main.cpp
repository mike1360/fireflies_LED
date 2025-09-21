#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <math.h>
#include <Preferences.h>

// --------- USER CONFIG ----------
#define LED_PIN       5
#define NUM_LEDS      500
#define LED_TYPE      WS2811
#define COLOR_ORDER   GRB
#define MAX_MA        2000

const char* WIFI_SSID = "Kamaji";
const char* WIFI_PASS = "princesselizabeth";
// --------------------------------

CRGB leds[NUM_LEDS];
AsyncWebServer server(80);

// ---- Wi-Fi state / storage ----
Preferences prefs;
String savedSsid = "";
String savedPass = "";
bool inApMode = false;
const char* AP_SSID = "Fireflies-Setup";
const char* AP_PASS = ""; // open AP

// Multiple saved networks
struct WifiCred { String ssid; String pass; };
#define MAX_WIFI_NETS 10
WifiCred gNets[MAX_WIFI_NETS];
int gNetCount = 0;

void loadWifiList(){
  gNetCount = 0;
  prefs.begin("wifi", false);
  int n = prefs.getInt("n", 0);
  for (int i=0; i<n && i<MAX_WIFI_NETS; i++){
    String s = prefs.getString(("s"+String(i)).c_str(), "");
    String p = prefs.getString(("p"+String(i)).c_str(), "");
    if (s.length()>0) gNets[gNetCount++] = {s,p};
  }
  // Backward compat: migrate single ssid/pass if present
  if (gNetCount==0){
    String s = prefs.getString("ssid", "");
    String p = prefs.getString("pass", "");
    if (s.length()>0){
      gNets[gNetCount++] = {s,p};
      prefs.putInt("n", gNetCount);
      prefs.putString("s0", s);
      prefs.putString("p0", p);
    }
  }
  prefs.end();
}
void saveWifiList(){
  prefs.begin("wifi", false);
  prefs.putInt("n", gNetCount);
  for (int i=0;i<gNetCount;i++){
    prefs.putString(("s"+String(i)).c_str(), gNets[i].ssid);
    prefs.putString(("p"+String(i)).c_str(), gNets[i].pass);
  }
  prefs.end();
}
bool addWifi(const String& s, const String& p){
  if (s.length()==0 || gNetCount>=MAX_WIFI_NETS) return false;
  for (int i=0;i<gNetCount;i++){ if (gNets[i].ssid==s){ gNets[i].pass=p; saveWifiList(); return true; } }
  gNets[gNetCount++] = {s,p};
  saveWifiList();
  return true;
}
bool delWifi(int idx){
  if (idx<0 || idx>=gNetCount) return false;
  for (int i=idx;i<gNetCount-1;i++) gNets[i]=gNets[i+1];
  gNetCount--;
  saveWifiList();
  return true;
}

// runtime params
uint8_t gBrightness = 80;
uint8_t gMode = 0;      // 0=Fireflies 1=Sync 2=Wave 3=Twinkle 4=Swarm 5=Ripples
uint8_t gDensity = 35;  // meaning varies by mode
uint8_t gSpeed = 50;
uint8_t gHueBase = 45;
uint8_t gSaturation = 200;
bool    gAutoHueDrift = true;

// NEW: lifespan + background fade
uint8_t gLifespan = 50; // 1–100; lower = shorter life (faster on/off)
uint8_t gFade     = 240; // 200–250; lower = faster fade-to-black

uint32_t lastFrame = 0;
uint32_t tMs = 0;

// ----- Simple scheduler -----
struct SchedItem { uint8_t mode; uint32_t duration_ms; };
#define MAX_SCHEDULE_ITEMS 20
SchedItem gSchedule[MAX_SCHEDULE_ITEMS];
uint8_t gScheduleCount = 0;
uint8_t gScheduleIndex = 0;
uint32_t gSchedStart = 0;
bool gScheduleEnabled = false;

// ---------- FIREFLIES -----------
struct Firefly { uint16_t idx; float phase, rise, fall, hold; uint8_t hue; bool active; };
#define MAX_FIREFLIES 128
Firefly flies[MAX_FIREFLIES];

float frand(float a,float b){ return a+(b-a)*(float)esp_random()/(float)UINT32_MAX; }
uint16_t urand16(uint16_t a,uint16_t b){ return a+(esp_random()%(uint32_t)(b-a+1)); }

void spawnFly(Firefly &f){
  f.idx = urand16(0, NUM_LEDS-1);
  f.rise = frand(0.15,0.35); f.hold = frand(0.05,0.25); f.fall = frand(0.25,0.55);
  f.phase=0; f.hue = gAutoHueDrift ? (gHueBase + random8(12)) : gHueBase; f.active=true;
}
void setupFireflies(){ for(int i=0;i<MAX_FIREFLIES;i++) flies[i].active=false; }

void stepFireflies(float dt){
  // spawn to target density
  uint8_t target = map(gDensity, 0, 100, 0, MAX_FIREFLIES);
  int active=0; for(auto &f:flies) if(f.active) active++;
  for(int s=0;s<4 && active<target;s++){ for(auto &f:flies) if(!f.active){ spawnFly(f); active++; break; } }

  // Stronger background fade so residuals actually reach black
  for (int i=0;i<NUM_LEDS;i++){
    leds[i].nscale8_video(gFade);
    if (leds[i].r < 3 && leds[i].g < 3 && leds[i].b < 3) leds[i] = CRGB::Black; // snap tiny embers off
  }

  // Lifespan scaler: lower gLifespan => faster time progression
  float lifeScale = 0.6f + 1.6f * (1.0f - (gLifespan / 100.0f)); // 100 → slow/long, 1 → fast/short

  for (auto &f : flies){
    if (!f.active) continue;

    float speedScalar = (0.08f + 0.6f*(gSpeed/100.0f)) * lifeScale;
    f.phase += dt * speedScalar;

    float total = f.rise + f.hold + f.fall;
    if (f.phase >= total){ f.active=false; continue; }

    float x=f.phase, a=0.0f;
    if (x < f.rise)               a = x / f.rise;
    else if (x < f.rise + f.hold) a = 1.0f;
    else                          a = 1.0f - ((x - (f.rise + f.hold)) / f.fall);

    uint8_t v = (uint8_t)((a*a) * 255);
    leds[f.idx] += CHSV(f.hue, gSaturation, v);
  }

  if (gAutoHueDrift && (millis() & 1023) < 16) gHueBase++;
}

// ---------- SYNC / WAVE ----------
void stepSync(float){ uint8_t beat = beatsin8(10+(gSpeed/2), 10, 255); fill_solid(leds, NUM_LEDS, CHSV(gHueBase,gSaturation,beat)); }
void stepWave(float t){
  for(int i=0;i<NUM_LEDS;i++){
    uint8_t b1=sin8((i*2)+(t*(2+gSpeed/2))); uint8_t b2=sin8((i*3)-(t*(1+gSpeed/3)));
    leds[i]=CHSV(gHueBase,gSaturation,qadd8(b1/2,b2/2));
  }
}

// ---------- TWINKLE --------------
void stepTwinkle(float){
  fadeToBlackBy(leds, NUM_LEDS, 12);
  if(random8() < gDensity){ int i = random16(NUM_LEDS); leds[i] += CHSV(gHueBase + random8(18), gSaturation, random8(160,255)); }
}

// ---------- SWARM (Perlin) -------
void stepSwarm(float t){
  for(int i=0;i<NUM_LEDS;i++){
    uint8_t n = inoise8(i*4, (uint32_t)(t* (10+gSpeed)) );
    uint8_t v = scale8(n, 220);
    leds[i] = CHSV(gHueBase + scale8(n,20), gSaturation, v);
  }
}

// ---------- RIPPLES --------------
#define MAX_RIPPLES 6
struct Ripple { int center; float age; float speed; bool on; };
Ripple rip[MAX_RIPPLES];
void triggerRipple(){ for(auto &r:rip) if(!r.on){ r.center=random16(NUM_LEDS); r.age=0; r.speed=0.9f+(gSpeed/140.0f); r.on=true; break; } }
void stepRipples(float dt){
  fadeToBlackBy(leds, NUM_LEDS, 18);
  for(auto &r:rip){
    if(!r.on) continue;
    r.age += dt*r.speed;
    float radius = r.age * (8 + gDensity/2.0f);
    if(radius>NUM_LEDS){ r.on=false; continue; }
    for(int i=0;i<NUM_LEDS;i++){
      float d=fabsf(i - r.center);
      if(d<radius && d>radius-2){
        uint8_t v = (uint8_t)(255.0f * (1.0f - (radius/NUM_LEDS)));
        leds[i] += CHSV(gHueBase, gSaturation, v);
      }
    }
  }
}
// Ripple overlay that can run on top of any base mode
void stepRipplesOverlay(float dt){
  stepRipples(dt);
}

// ------------- WEB UI -------------
const char* HTML = R"HTML(
<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>
<title>Fireflies</title>
<style>
:root{--bg:#0b0d12;--card:#121725;--text:#e6ecf2;--muted:#95a2b0;--acc:#5ee06c;--rail:#2b3246;--handle:#2a9241;}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial}
.wrap{max-width:880px;margin:32px auto;padding:0 18px}
.card{background:var(--card);border-radius:14px;padding:18px;margin:14px 0;box-shadow:0 6px 18px rgba(0,0,0,.28)}
h2{margin:8px 0 14px}
label{display:flex;justify-content:space-between;align-items:center;margin:12px 0 6px}
.val{opacity:.85;font-variant-numeric:tabular-nums}
select,input[type=range],button{width:100%}
input[type=range]{accent-color:var(--acc)}
.hint{color:var(--muted);font-size:.9em;margin:-2px 0 10px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media (max-width:740px){.row{grid-template-columns:1fr}}
.btn{margin-top:10px;padding:12px 14px;border:0;border-radius:10px;background:var(--acc);color:#06210b;font-weight:700}
.btn.secondary{background:#2f3548;color:#d9e0ea}
.bar{height:10px;border-radius:8px;background:var(--rail);overflow:hidden}
.footer{color:var(--muted);font-size:.85em;margin-top:10px}
.sched-list{list-style:none;margin:0;padding:0}
.sched-item{display:flex;align-items:center;gap:8px;background:#0f1320;border:1px solid #242a3a;border-radius:10px;padding:10px;margin:8px 0;cursor:grab}
.handle{width:14px;height:14px;border-radius:4px;background:var(--handle)}
.mode-badge{font-weight:600}
.small{font-size:.9em;color:var(--muted)}
</style></head><body><div class=wrap>
<h2>Fireflies Controller</h2>
<div id="apBanner" class="hint" style="display:none">AP mode: connect your phone to <b>Fireflies-Setup</b> and open <b>http://192.168.4.1/wifi</b> to join a network.</div>
<div class=card>
  <label>Mode
    <select id=mode>
      <option value=0>Fireflies</option>
      <option value=1>Sync Pulse</option>
      <option value=2>Wave</option>
      <option value=3>Twinkle</option>
      <option value=4>Swarm</option>
      <option value=5>Ripples (standalone)</option>
    </select>
  </label>
  <div class=hint>Choose the base animation. The Ripple button below now works on <em>any</em> mode as an overlay.</div>

  <div class=row>
    <div>
      <label>Brightness <span class=val id=vbright></span></label>
      <input type=range id=bright min=1 max=255 value=80>
      <div class=hint>Overall output level. Lower if your PSU gets warm.</div>
    </div>
    <div>
      <label>Density / Intensity <span class=val id=vdensity></span></label>
      <input type=range id=density min=0 max=100 value=35>
      <div class=hint>How many lights are active at once (mode-dependent).</div>
    </div>
  </div>

  <div class=row>
    <div>
      <label>Speed <span class=val id=vspeed></span></label>
      <input type=range id=speed min=1 max=100 value=50>
      <div class=hint>Animation tempo.</div>
    </div>
    <div>
      <label>Hue <span class=val id=vhue></span></label>
      <input type=range id=hue min=0 max=255 value=45>
      <div class=hint>Base color; enable drift for subtle wandering.</div>
    </div>
  </div>

  <div class=row>
    <div>
      <label>Saturation <span class=val id=vsat></span></label>
      <input type=range id=sat min=0 max=255 value=200>
      <div class=hint>Color purity. Lower for pastel fireflies.</div>
    </div>
    <div>
      <label>Lifespan <span class=val id=vlife></span></label>
      <input type=range id=life min=1 max=100 value=50>
      <div class=hint>How long each firefly lives. Lower = quicker blink.</div>
    </div>
  </div>

  <label>Background Fade <span class=val id=vfade></span></label>
  <input type=range id=fade min=200 max=250 value=240>
  <div class=hint>How fast the whole scene clears to black. Lower = faster.</div>

  <div style="display:flex;align-items:center;gap:10px;margin-top:8px">
    <input type=checkbox id=drift checked>
    <label for=drift style="margin:0">Auto hue drift</label>
  </div>

  <button class=btn id=rippleBtn>Create Ripple</button>
  <div class=hint>Triggers an expanding wave overlay on top of the current mode.</div>

  <div class=card style="margin-top:16px">
    <h3 style="margin:0 0 8px">Wi-Fi</h3>
    <div class=hint>Save the networks you use. The device will try them at boot, in the order listed below.</div>

    <ul id="wifiList" class="sched-list" style="margin-top:8px"></ul>

    <div class=row>
      <div>
        <label>SSID</label>
        <input id="nwSsid" placeholder="Network name">
      </div>
      <div>
        <label>Password</label>
        <input id="nwPass" type="password" placeholder="Password">
      </div>
    </div>
    <div class=row>
      <button class="btn secondary" id="addNet">Add network</button>
      <a class="btn secondary" href="/wifi_setup">Start Setup Hotspot</a>
    </div>
    <div class=row>
      <a class="btn secondary" href="/wifi">Open Wi-Fi Setup</a>
      <button class="btn" id="tryNets">Try saved networks now</button>
    </div>
    <div class="small" id="wifiMsg" style="margin-top:6px;opacity:.85"></div>
  </div>
</div>

<div class=card>
  <h2 style="margin-top:0">Scheduler</h2>
  <div class=hint>Build a simple timeline: choose a mode, set duration, press <b>Add</b>. Drag to reorder. Press <b>Send to ESP32</b> to start looping.</div>
  <div class=row>
    <div>
      <label>Mode</label>
      <select id=schMode>
        <option value=0>Fireflies</option>
        <option value=3>Twinkle</option>
        <option value=4>Swarm</option>
        <option value=2>Wave</option>
        <option value=1>Sync Pulse</option>
        <option value=5>Ripples (standalone)</option>
      </select>
    </div>
    <div>
      <label>Duration</label>
      <div class=row>
        <input type=number id=schMin min=0 max=180 value=0 style="width:100%" placeholder="min">
        <input type=number id=schSec min=0 max=59 value=10 style="width:100%" placeholder="sec">
      </div>
      <div class="small" style="opacity:.8">Minutes and seconds (0–180 min, 0–59 sec).</div>
    </div>
  </div>
  <div class=row>
    <button class="btn" id=addItem>Add</button>
    <button class="btn secondary" id=clearItems>Clear</button>
  </div>
  <ul class=sched-list id=schedList></ul>
  <button class="btn" id=sendSched>Send to ESP32</button>
  <div class="footer small">The schedule loops continuously on the device until you clear or send a new one.</div>
</div>

<script>
// ----- Helpers -----
const qs=id=>document.getElementById(id);
const state={};

fetch('/whoami').then(r=>r.json()).then(j=>{
  const b=document.getElementById('apBanner'); if(!b) return;
  if(j && j.ap){ b.style.display='block'; }
}).catch(()=>{});

function upd(){
  qs('vbright').textContent=qs('bright').value;
  qs('vdensity').textContent=qs('density').value;
  qs('vspeed').textContent=qs('speed').value;
  qs('vhue').textContent=qs('hue').value;
  qs('vsat').textContent=qs('sat').value;
  qs('vlife').textContent=qs('life').value;
  qs('vfade').textContent=qs('fade').value;
}

function sendParams(){
  const p=new URLSearchParams({
    mode:qs('mode').value, bright:qs('bright').value, density:qs('density').value,
    speed:qs('speed').value, hue:qs('hue').value, sat:qs('sat').value,
    life:qs('life').value, fade:qs('fade').value, drift:qs('drift').checked?1:0
  });
  fetch('/set?'+p);
}

['mode','bright','density','speed','hue','sat','life','fade','drift'].forEach(id=>{
  qs(id).addEventListener(id==='mode'?'change':'input', ()=>{ upd(); sendParams(); });
});

// Ripple overlay works on any mode now
qs('rippleBtn').addEventListener('click', ()=>{ fetch('/ripple'); });

upd();

// ----- Scheduler UI (drag & drop) -----
const list=qs('schedList');
function renderSchedule(items){
  list.innerHTML='';
  items.forEach((it,idx)=>{
    const li=document.createElement('li');
    li.className='sched-item';
    li.draggable=true;
    li.dataset.idx=idx;
    const mm = Math.floor(it.seconds/60), ss = it.seconds%60;
    li.innerHTML = '<div class=handle></div>'
      + '<div class=mode-badge>'+ modeName(it.mode) +'</div>'
      + '<div class=small>'+ (mm>0?(mm+'m '):'') + ss + 's</div>'
      + '<button class="btn secondary" style="width:auto" onclick="removeItem('+idx+')">Remove</button>';
    li.addEventListener('dragstart', ev=>{ ev.dataTransfer.setData('text/plain', idx); });
    li.addEventListener('dragover', ev=>ev.preventDefault());
    li.addEventListener('drop', ev=>{
      ev.preventDefault();
      const from=+ev.dataTransfer.getData('text/plain'); const to=idx;
      if(from===to) return;
      const a=schedule[from]; schedule.splice(from,1); schedule.splice(to,0,a);
      renderSchedule(schedule);
    });
    list.appendChild(li);
  });
}
function modeName(m){ return ['Fireflies','Sync','Wave','Twinkle','Swarm','Ripples'][m] || ('Mode '+m); }
let schedule=[];

qs('addItem').addEventListener('click', ()=>{
  const m = +qs('schMode').value;
  const min = Math.max(0, Math.min(180, +qs('schMin').value||0));
  const sec = Math.max(0, Math.min(59,  +qs('schSec').value||0));
  let total = (min*60)+sec;
  if (total < 1) total = 1; // minimum 1s
  schedule.push({mode:m, seconds: total});
  renderSchedule(schedule);
});
window.removeItem=function(i){ schedule.splice(i,1); renderSchedule(schedule); };
qs('clearItems').addEventListener('click', ()=>{ schedule=[]; renderSchedule(schedule); });

qs('sendSched').addEventListener('click', ()=>{
  fetch('/schedule', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({items:schedule})});
});

// ----- Wi-Fi list management -----
function refreshWifiList(){
  fetch('/wifi_list').then(r=>r.json()).then(arr=>{
    const ul=document.getElementById('wifiList'); ul.innerHTML='';
    arr.forEach((item,i)=>{
      const li=document.createElement('li');
      li.className='sched-item';
      li.innerHTML = '<div class="handle"></div>'
        + '<div class="mode-badge">'+ item.ssid +'</div>'
        + '<button class="btn secondary" style="width:auto" onclick="delNet('+i+')">Remove</button>';
      ul.appendChild(li);
    });
  }).catch(()=>{});
}
function delNet(i){ fetch('/wifi_del?i='+i).then(()=>refreshWifiList()); }
document.getElementById('addNet').addEventListener('click', ()=>{
  const s=document.getElementById('nwSsid').value.trim();
  const p=document.getElementById('nwPass').value;
  if(!s){ document.getElementById('wifiMsg').textContent='Enter an SSID.'; return; }
  fetch('/wifi_add', {method:'POST', body:new URLSearchParams({ssid:s, pass:p})})
    .then(()=>{ document.getElementById('nwPass').value=''; refreshWifiList(); });
});
document.getElementById('tryNets').addEventListener('click', ()=>{
  const msg=document.getElementById('wifiMsg'); msg.textContent='Trying saved networks…';
  fetch('/wifi_try').then(r=>r.json()).then(j=>{
    msg.textContent = j.connected ? 'Connected! Check the Serial Monitor for IP.' : 'No saved networks worked. AP may be active.';
  }).catch(()=>{ msg.textContent='Error contacting device.'; });
});
refreshWifiList();
</script>
</div></body></html>
)HTML";

void startAP(){
  inApMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.println();
  Serial.print("AP mode. Connect to "); Serial.print(AP_SSID);
  Serial.print(" then visit http://"); Serial.println(ip);
}

bool tryConnectSTA(const char* ssid, const char* pass, uint32_t timeoutMs){
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("WiFi connect to '"); Serial.print(ssid); Serial.print("' ");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    inApMode = false;
    return true;
  }
  Serial.println();
  Serial.println("WiFi connect timeout.");
  return false;
}

void setupWiFi(){
  Serial.begin(115200);
  delay(50);

  loadWifiList();  // new: list of networks

  bool ok = false;
  for (int i=0; i<gNetCount && !ok; i++){
    ok = tryConnectSTA(gNets[i].ssid.c_str(), gNets[i].pass.c_str(), 10000);
  }

  if (!ok && String(WIFI_SSID).length()>0){
    ok = tryConnectSTA(WIFI_SSID, WIFI_PASS, 10000);
    if (ok){ addWifi(WIFI_SSID, WIFI_PASS); }
  }

  if (!ok) startAP();
}

void setupWeb(){
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html; charset=utf-8",HTML); });

  server.on("/set", HTTP_GET, [](AsyncWebServerRequest* req){
    if(req->hasParam("mode"))    gMode = req->getParam("mode")->value().toInt();
    if(req->hasParam("bright"))  gBrightness = req->getParam("bright")->value().toInt();
    if(req->hasParam("density")) gDensity = req->getParam("density")->value().toInt();
    if(req->hasParam("speed"))   gSpeed = req->getParam("speed")->value().toInt();
    if(req->hasParam("hue"))     gHueBase = req->getParam("hue")->value().toInt();
    if(req->hasParam("sat"))     gSaturation = req->getParam("sat")->value().toInt();
    if(req->hasParam("life"))    gLifespan = req->getParam("life")->value().toInt();
    if(req->hasParam("fade"))    gFade     = req->getParam("fade")->value().toInt();
    if(req->hasParam("drift"))   gAutoHueDrift = (req->getParam("drift")->value().toInt()!=0);
    req->send(200,"text/plain","ok");
  });

  server.on("/ripple", HTTP_GET, [](AsyncWebServerRequest* r){ triggerRipple(); r->send(200,"text/plain","rip"); });

  // UI can detect AP mode
  server.on("/whoami", HTTP_GET, [](AsyncWebServerRequest* r){
    String j = String("{\"ap\":") + (inApMode ? "true" : "false") + "}";
    r->send(200, "application/json", j);
  });

  // Simple Wi-Fi config page (single entry)
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* r){
    String page = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Wi-Fi Setup</title><style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;padding:18px}input,button{font-size:16px;padding:10px;margin:6px 0;width:100%}label{display:block;margin-top:10px}</style></head><body>");
    page += F("<h2>Fireflies Wi-Fi Setup</h2>");
    page += F("<form method='POST' action='/wifi_save'>");
    page += F("<label>SSID</label><input name='ssid' placeholder='Network name' value='");
    page += savedSsid; page += F("'>");
    page += F("<label>Password</label><input name='pass' type='password' placeholder='Password' value='");
    page += savedPass; page += F("'>");
    page += F("<button type='submit'>Save &amp; Reboot</button></form>");
    page += F("<p style='opacity:.7'>Device will reboot and try to join the network you entered.</p>");
    page += F("</body></html>");
    r->send(200, "text/html; charset=utf-8", page);
  });
  server.on("/wifi_save", HTTP_POST, [](AsyncWebServerRequest* req){
    String ssid = req->getParam("ssid", true)->value();
    String pass = req->getParam("pass", true)->value();
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    savedSsid = ssid; savedPass = pass;
    req->send(200, "text/html", "<meta http-equiv='refresh' content='2;url=/' /><h3>Saved. Rebooting…</h3>");
    delay(500);
    ESP.restart();
  });

  // Saved networks management
  server.on("/wifi_list", HTTP_GET, [](AsyncWebServerRequest* r){
    String out="[";
    for (int i=0;i<gNetCount;i++){ if(i) out+=","; out+="{\"ssid\":\""+gNets[i].ssid+"\"}"; }
    out+="]";
    r->send(200,"application/json",out);
  });
  server.on("/wifi_add", HTTP_POST, [](AsyncWebServerRequest* req){
    String ssid = req->getParam("ssid", true)->value();
    String pass = req->getParam("pass", true)->value();
    bool ok = addWifi(ssid, pass);
    req->send(200,"application/json", String("{\"ok\":")+(ok?"true":"false")+"}");
  });
  server.on("/wifi_del", HTTP_GET, [](AsyncWebServerRequest* req){
    int idx = req->hasParam("i") ? req->getParam("i")->value().toInt() : -1;
    bool ok = delWifi(idx);
    req->send(200,"application/json", String("{\"ok\":")+(ok?"true":"false")+"}");
  });
  server.on("/wifi_try", HTTP_GET, [](AsyncWebServerRequest* r){
    bool ok=false;
    for (int i=0;i<gNetCount && !ok;i++){
      ok = tryConnectSTA(gNets[i].ssid.c_str(), gNets[i].pass.c_str(), 7000);
    }
    if(!ok) startAP();
    r->send(200,"application/json", String("{\"connected\":")+(ok?"true":"false")+"}");
  });

  // Receive schedule as JSON: {"items":[{"mode":0,"seconds":10}, ...]}
  server.on("/schedule", HTTP_POST, [](AsyncWebServerRequest* req){}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      gScheduleCount = 0;
      String body; body.reserve(len+1);
      for(size_t i=0;i<len;i++) body += (char)data[i];
      int pos = 0;
      while (gScheduleCount < MAX_SCHEDULE_ITEMS){
        int mPos = body.indexOf("\"mode\"", pos); if(mPos<0) break;
        int colon = body.indexOf(':', mPos); if(colon<0) break;
        int comma = body.indexOf(',', colon); int endBrace = body.indexOf('}', colon);
        int stop = (comma<0? endBrace : min(comma,endBrace));
        uint8_t mode = (uint8_t) body.substring(colon+1, stop).toInt();

        // prefer "seconds", fallback to "minutes"
        int tPos = body.indexOf("\"seconds\"", stop);
        bool usedSeconds = true;
        if (tPos < 0) { tPos = body.indexOf("\"minutes\"", stop); usedSeconds = false; }
        if (tPos<0) break;
        int tColon = body.indexOf(':', tPos); if(tColon<0) break;
        int tComma = body.indexOf(',', tColon); int tEnd = body.indexOf('}', tColon);
        int tStop = (tComma<0? tEnd : min(tComma,tEnd));
        uint32_t amount = (uint32_t) body.substring(tColon+1, tStop).toInt();
        uint32_t durMs = usedSeconds ? (amount*1000UL) : (amount*60UL*1000UL);

        gSchedule[gScheduleCount++] = { mode, durMs };
        pos = tStop;
      }
      gScheduleIndex = 0;
      gSchedStart = millis();
      gScheduleEnabled = (gScheduleCount>0);
      req->send(200, "application/json", String("{\"count\":") + gScheduleCount + "}");
    }
  );

  server.begin();
}

void setup(){
  delay(200);
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MA);
  FastLED.setBrightness(gBrightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();

  setupFireflies();
  for(auto &r:rip) r.on=false;

  setupWiFi();
  setupWeb();
  lastFrame = millis();

  gScheduleEnabled = false;
  gScheduleCount = 0;
  gScheduleIndex = 0;
  gSchedStart = millis();
}

void loop(){
  tMs = millis();
  float dt = (tMs - lastFrame) / 1000.0f; lastFrame = tMs;

  // Scheduler: advance mode when duration expires
  if (gScheduleEnabled && gScheduleCount > 0){
    if (tMs - gSchedStart >= gSchedule[gScheduleIndex].duration_ms){
      gScheduleIndex = (gScheduleIndex + 1) % gScheduleCount;
      gMode = gSchedule[gScheduleIndex].mode;
      gSchedStart = tMs;
    }
  }

  // Base mode render
  switch(gMode){
    case 0: stepFireflies(dt); break;
    case 1: stepSync(tMs/1000.0f); break;
    case 2: stepWave(tMs/1000.0f); break;
    case 3: stepTwinkle(dt); break;
    case 4: stepSwarm(tMs/1000.0f); break;
    case 5: stepRipples(dt); break; // standalone ripple mode
  }

  // Ripple overlay (works on any base mode)
  stepRipplesOverlay(dt);

  FastLED.setBrightness(gBrightness);
  FastLED.show();
}