#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <math.h>

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
</div>

<div class=card>
  <h2 style="margin-top:0">Scheduler</h2>
  <div class=hint>Build a simple timeline: choose a mode, set minutes, press <b>Add</b>. Drag to reorder. Press <b>Send to ESP32</b> to start looping.</div>
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
      <label>Duration (minutes)</label>
      <input type=number id=schMin min=1 max=180 value=5 style="width:100%">
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
    li.innerHTML = '<div class=handle></div>'
      + '<div class=mode-badge>'+ modeName(it.mode) +'</div>'
      + '<div class=small>'+ it.minutes +' min</div>'
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
  const m=+qs('schMode').value; const minutes=Math.max(1, Math.min(180, +qs('schMin').value||1));
  schedule.push({mode:m, minutes:minutes});
  renderSchedule(schedule);
});
window.removeItem=function(i){ schedule.splice(i,1); renderSchedule(schedule); };
qs('clearItems').addEventListener('click', ()=>{ schedule=[]; renderSchedule(schedule); });

qs('sendSched').addEventListener('click', ()=>{
  fetch('/schedule', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({items:schedule})});
});
</script>
</div></body></html>
)HTML";

void setupWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // <- corrected names
  Serial.begin(115200);
  Serial.print("WiFi…");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void setupWeb(){
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html",HTML); });
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

  // Receive schedule as JSON: {"items":[{"mode":0,"minutes":5}, ...]}
  server.on("/schedule", HTTP_POST, [](AsyncWebServerRequest* req){}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      // very small JSON parser: look for "mode" and "minutes" pairs
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

        int tPos = body.indexOf("\"minutes\"", stop); if(tPos<0) break;
        int tColon = body.indexOf(':', tPos); if(tColon<0) break;
        int tComma = body.indexOf(',', tColon); int tEnd = body.indexOf('}', tColon);
        int tStop = (tComma<0? tEnd : min(tComma,tEnd));
        uint32_t minutes = (uint32_t) body.substring(tColon+1, tStop).toInt();

        gSchedule[gScheduleCount++] = { mode, minutes*60UL*1000UL };
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