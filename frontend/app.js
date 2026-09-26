"use strict";
const $ = id => document.getElementById(id);
const scenes = [
  {name: "Aurora", description: "Slow ribbons of light shaped by ambient temperature."},
  {name: "Constellation", description: "Drifting points that gather into a living field."},
  {name: "Contours", description: "A landscape of flowing lines and gentle oscillations."}
];
let state = null, socket, reconnectTimer, lastPacket = "", lastAction = "";
let connected = false, displayedTemp = 22;
const stage = $("stage"), canvas = $("visual"), ctx = canvas.getContext("2d");
const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
let width = 1, height = 1;

function log(message) {
  const list = $("events");
  if (list.firstElementChild?.querySelector("time")?.textContent === "—") list.replaceChildren();
  const li = document.createElement("li"), text = document.createElement("span"), time = document.createElement("time");
  text.textContent = message; time.textContent = new Date().toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
  li.append(text, time); list.prepend(li);
  while (list.children.length > 4) list.lastElementChild.remove();
}
function setConnection(value) {
  connected = value;
  $("connection").textContent = value ? "Backend connected" : "Backend disconnected · retrying";
  $("connection").classList.toggle("online", value);
  $("next").disabled = $("blackout").disabled = !value;
  if (!value) {
    $("temperature").textContent = "—";
    $("sensor-status").textContent = "Live readings unavailable";
    $("preview-state").textContent = "OFFLINE · LAST SCENE";
    $("button-state").textContent = "Physical button unavailable";
    $("button-dot").classList.remove("pressed");
  }
}
function update(next) {
  if (next.type !== "state" || !Number.isInteger(next.scene_index) || !scenes[next.scene_index]) return;
  state = next;
  const scene = scenes[state.scene_index], d = state.device;
  const fresh = state.device_online;
  const temperature = fresh && d?.temperature_available && Number.isFinite(d.temperature_c) ? d.temperature_c : null;
  $("temperature").textContent = temperature === null ? "—" : temperature.toFixed(1);
  $("mode").textContent = state.mode === "demo" ? "DEMO · SIMULATED SENSOR" : "LIVE HARDWARE";
  $("sensor-status").textContent = state.mode === "demo" ? "Simulated temperature · no hardware input" :
    !state.serial_connected ? "Nano disconnected · reconnecting" : !fresh ? "No recent telemetry" :
    temperature === null ? "Temperature sensor unavailable" : "TMP102 · updated every second";
  $("scene-name").textContent = scene.name;
  $("scene-description").textContent = scene.description;
  $("scene-label").textContent = `0${state.scene_index + 1} / ${scene.name}`;
  $("blackout").textContent = state.blackout ? "Resume projection" : "Blackout";
  $("preview-state").textContent = state.blackout ? "BLACKOUT" : fresh ? "RENDERING" : "SENSOR OFFLINE";
  const buttonReady = fresh && d?.button_available;
  $("button-state").textContent = !buttonReady ? "Physical button unavailable" : d.button_pressed ? "Physical button held" : "Physical button ready";
  $("button-dot").classList.toggle("pressed", Boolean(buttonReady && d.button_pressed));
  const packet = d ? `${d.seq}:${d.uptime_ms}` : "";
  if (packet && packet !== lastPacket && !["telemetry", "status", "button_release"].includes(d.event)) {
    const labels = {short_press: "Short press → next scene", long_press: "Long hold → toggle blackout", ready: "Nano firmware ready"};
    log(labels[d.event] || d.event); lastPacket = packet;
  }
  if (["next_scene", "toggle_blackout"].includes(state.last_event) && state.last_event !== lastAction)
    log(state.last_event === "next_scene" ? "Browser → next scene" : "Browser → toggle blackout");
  lastAction = state.last_event;
}
function connect() {
  socket = new WebSocket(`${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/ws`);
  socket.onopen = () => { setConnection(true); log("Connected to the C++ backend"); };
  socket.onmessage = event => { try { update(JSON.parse(event.data)); } catch { /* Ignore malformed messages. */ } };
  socket.onclose = () => { setConnection(false); clearTimeout(reconnectTimer); reconnectTimer = setTimeout(connect, 2000); };
  socket.onerror = () => socket.close();
}
function action(name) {
  if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({action: name}));
}
$("next").addEventListener("click", () => action("next_scene"));
$("blackout").addEventListener("click", () => action("toggle_blackout"));
$("fullscreen").addEventListener("click", async () => {
  try { await stage.requestFullscreen(); } catch { log("Fullscreen unavailable in this browser"); }
});
document.addEventListener("keydown", event => {
  if (event.repeat || event.target instanceof HTMLInputElement || event.target instanceof HTMLButtonElement) return;
  if (event.code === "ArrowRight") { event.preventDefault(); action("next_scene"); }
  if (event.code === "KeyB") action("toggle_blackout");
});
function resize() {
  const bounds = stage.getBoundingClientRect(), dpr = Math.min(devicePixelRatio || 1, 2);
  width = bounds.width; height = bounds.height;
  canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
}
new ResizeObserver(resize).observe(stage);
function aurora(t, hue) {
  const glow = ctx.createRadialGradient(width*.48, height*.65, 0, width*.5, height*.5, width*.65);
  glow.addColorStop(0, `hsla(${hue}, 65%, 23%, .55)`); glow.addColorStop(1, "#060d10");
  ctx.fillStyle = glow; ctx.fillRect(0, 0, width, height);
  for (let ribbon = 0; ribbon < 34; ribbon++) {
    ctx.beginPath();
    for (let x = -10; x <= width+10; x += 8) {
      const y = height*.5 + Math.sin(x/width*5 + t*.3 + ribbon*.048)*height*.18 +
        Math.sin(x/width*9-t*.2)*height*.055 + ribbon*height*.005;
      if (x === -10) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    }
    ctx.strokeStyle = `hsla(${hue+ribbon*.8},70%,${45+ribbon*.6}%,${.04+Math.sin(ribbon/34*Math.PI)*.24})`;
    ctx.lineWidth = 1.2; ctx.stroke();
  }
}
const particles = Array.from({length: 65}, (_, i) => ({x: ((i*37)%101)/101, y: ((i*61)%103)/103, r: 1+(i%3)*.65}));
function constellation(t, hue) {
  const points = particles.map((p,i) => ({x: (p.x+Math.sin(t*.15+i)*.035)*width, y: (p.y+Math.cos(t*.12+i)*.04)*height, r:p.r}));
  for (let i=0; i<points.length; i++) {
    const p=points[i];
    for(let j=i+1;j<points.length;j++) {
      const q=points[j], distance=Math.hypot(p.x-q.x,p.y-q.y), limit=width*.14;
      if (distance < limit) { ctx.strokeStyle=`hsla(${hue},50%,65%,${(1-distance/limit)*.3})`;ctx.beginPath();ctx.moveTo(p.x,p.y);ctx.lineTo(q.x,q.y);ctx.stroke(); }
    }
    ctx.fillStyle=`hsla(${hue},60%,80%,.85)`;ctx.beginPath();ctx.arc(p.x,p.y,p.r,0,Math.PI*2);ctx.fill();
  }
}
function contours(t, hue) {
  for(let row=0;row<27;row++) {
    ctx.beginPath();
    for(let x=0;x<=width+5;x+=6) {
      const dx=x/width, y=height*(.1+row*.032)+Math.sin(dx*8+t*.25+row*.12)*height*.06*Math.sin(dx*Math.PI);
      if(x===0) ctx.moveTo(x,y);else ctx.lineTo(x,y);
    }
    ctx.strokeStyle=`hsla(${hue+row},60%,65%,${.2+.45*Math.sin(row/27*Math.PI)})`;ctx.lineWidth=1;ctx.stroke();
  }
}
let previousFrame = 0;
function render(ms) {
  requestAnimationFrame(render);
  if (ms-previousFrame < 1000/30) return; previousFrame=ms;
  ctx.fillStyle="#000";ctx.fillRect(0,0,width,height);
  if (state?.blackout) return;
  if (connected && state?.device_online && state.device?.temperature_available && Number.isFinite(state.device.temperature_c))
    displayedTemp += (state.device.temperature_c-displayedTemp)*.025;
  const heat=Math.max(0,Math.min(1,(displayedTemp-16)/16));
  const hue=190-heat*150, t=reducedMotion.matches ? 0 : ms/1000;
  ctx.fillStyle="#060d10";ctx.fillRect(0,0,width,height);
  [aurora,constellation,contours][state?.scene_index ?? 0](t,hue);
}
connect(); requestAnimationFrame(render);
