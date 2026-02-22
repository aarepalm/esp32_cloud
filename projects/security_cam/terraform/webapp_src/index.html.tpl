<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Security Camera Gallery</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: monospace;
      background: #0f1117;
      color: #e0e0e0;
      padding: 1.5rem;
    }

    header {
      display: flex;
      align-items: baseline;
      gap: 1rem;
      margin-bottom: 1.5rem;
    }

    h1 { font-size: 1.2rem; color: #7eb8f7; }

    #status { font-size: 0.8rem; color: #555; flex: 1; }
    #status.ok  { color: #4caf50; }
    #status.err { color: #f44336; }

    button {
      background: none;
      border: 1px solid #333;
      color: #888;
      padding: 0.25rem 0.6rem;
      cursor: pointer;
      font-family: monospace;
      font-size: 0.8rem;
      border-radius: 3px;
    }
    button:hover { border-color: #f44336; color: #f44336; }

    #grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
      gap: 1rem;
    }

    .card {
      background: #13151f;
      border: 1px solid #1e2130;
      border-radius: 4px;
      overflow: hidden;
      display: flex;
      flex-direction: column;
    }

    /* Kept clips get a coloured border so they stand out */
    .card.kept { border-color: #4caf50; }

    .card img {
      width: 100%;
      aspect-ratio: 4/3;
      object-fit: cover;
      display: block;
      background: #0a0c14;
    }

    .no-thumb {
      width: 100%;
      aspect-ratio: 4/3;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #333;
      font-size: 0.75rem;
      background: #0a0c14;
    }

    .meta {
      padding: 0.5rem 0.7rem 0.3rem;
      flex: 1;
    }

    .ts {
      font-size: 0.75rem;
      color: #7eb8f7;
      margin-bottom: 0.2rem;
    }

    .sz { font-size: 0.72rem; color: #555; }

    /* Bottom action bar: download | keep | delete */
    .actions {
      display: flex;
      border-top: 1px solid #1e2130;
    }

    .actions a, .actions button {
      flex: 1;
      display: block;
      text-align: center;
      padding: 0.4rem 0;
      color: #888;
      text-decoration: none;
      font-size: 0.78rem;
      border: none;
      border-radius: 0;
      background: none;
      cursor: pointer;
      font-family: monospace;
    }

    .actions a + a, .actions a + button, .actions button + button, .actions button + a {
      border-left: 1px solid #1e2130;
    }

    .actions a:hover      { color: #7eb8f7; background: #0f1117; }
    .actions .btn-keep    { color: #888; }
    .actions .btn-keep:hover       { color: #4caf50; background: #0f1117; }
    .actions .btn-keep.active      { color: #4caf50; }
    .actions .btn-keep.active:hover { color: #888; }
    .actions .btn-del:hover  { color: #f44336; background: #0f1117; }

    /* Dim the card while an async action is in flight */
    .card.busy { opacity: 0.5; pointer-events: none; }
  </style>
</head>
<body>

<header>
  <h1>Security Camera</h1>
  <span id="status">Loading...</span>
  <button onclick="logout()">logout</button>
</header>

<div id="grid"></div>

<script>
// Config injected by Terraform at apply time
const CFG = {
  cognitoDomain: "${cognito_domain}",
  clientId:      "${client_id}",
  redirectUri:   "${redirect_uri}",
  apiUrl:        "${api_url}",
};

// ── Auth (same flow as telemetry dashboard) ───────────────────────────────────

function loginUrl() {
  return "https://" + CFG.cognitoDomain + "/login?" + new URLSearchParams({
    client_id: CFG.clientId, response_type: "code",
    scope: "openid email", redirect_uri: CFG.redirectUri,
  });
}

function logout() {
  sessionStorage.clear();
  window.location.href = "https://" + CFG.cognitoDomain + "/logout?" +
    new URLSearchParams({ client_id: CFG.clientId, logout_uri: CFG.redirectUri });
}

async function exchangeCode(code) {
  const r = await fetch("https://" + CFG.cognitoDomain + "/oauth2/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      grant_type: "authorization_code", client_id: CFG.clientId,
      code: code, redirect_uri: CFG.redirectUri,
    }),
  });
  if (!r.ok) throw new Error("Token exchange failed: " + r.status);
  return r.json();
}

async function getValidToken() {
  const params = new URLSearchParams(window.location.search);
  const code   = params.get("code");
  if (code) {
    window.history.replaceState({}, "", window.location.pathname);
    const tokens = await exchangeCode(code);
    sessionStorage.setItem("id_token",     tokens.id_token);
    sessionStorage.setItem("token_expiry", Date.now() + tokens.expires_in * 1000);
    return tokens.id_token;
  }
  const token  = sessionStorage.getItem("id_token");
  const expiry = parseInt(sessionStorage.getItem("token_expiry") || "0");
  if (token && Date.now() < expiry - 30000) return token;
  window.location.href = loginUrl();
  return null;
}

// ── Gallery ───────────────────────────────────────────────────────────────────

let g_token = null;

function setStatus(msg, cls) {
  const el = document.getElementById("status");
  el.textContent = msg;
  el.className   = cls || "";
}

function formatTs(ts) {
  return ts.replace("T", " ").substring(0, 19) + " UTC";
}

async function manageClip(action, clipKey) {
  const r = await fetch(CFG.apiUrl + "/manage", {
    method:  "POST",
    headers: {
      Authorization:  "Bearer " + g_token,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ action: action, clip_key: clipKey }),
  });
  if (!r.ok) throw new Error("manage API error: " + r.status);
  return r.json();
}

function renderGallery(clips) {
  const grid = document.getElementById("grid");
  if (!clips.length) {
    grid.innerHTML = "<p style='color:#555;padding:2rem'>No clips found.</p>";
    return;
  }
  grid.innerHTML = clips.map(function(c) {
    const img = c.thumb_url
      ? "<img src='" + c.thumb_url + "' alt='thumbnail' loading='lazy'>"
      : "<div class='no-thumb'>no thumbnail</div>";

    const keptClass  = c.kept ? " kept" : "";
    const keepLabel  = c.kept ? "unkeep" : "keep";
    const keepActive = c.kept ? " active" : "";

    // Use data attributes so event handlers can read clip_key and kept state
    return "<div class='card" + keptClass + "' data-key='" + c.clip_key + "' data-kept='" + c.kept + "'>"
         + img
         + "<div class='meta'>"
         + "<div class='ts'>" + formatTs(c.timestamp) + "</div>"
         + "<div class='sz'>" + c.size_mb + " MB"
         + (c.kept ? " &nbsp;<span style='color:#4caf50'>&#128274; kept</span>" : "") + "</div>"
         + "</div>"
         + "<div class='actions'>"
         + "<a href='" + c.clip_url + "' download>download</a>"
         + "<button class='btn-keep" + keepActive + "' onclick='onKeep(this)'>" + keepLabel + "</button>"
         + "<button class='btn-del' onclick='onDelete(this)'>delete</button>"
         + "</div>"
         + "</div>";
  }).join("");
}

// Keep / unkeep button handler
async function onKeep(btn) {
  const card    = btn.closest(".card");
  const clipKey = card.dataset.key;
  const isKept  = card.dataset.kept === "true";
  const action  = isKept ? "unkeep" : "keep";

  card.classList.add("busy");
  try {
    await manageClip(action, clipKey);
    // Toggle UI state without a full reload
    const newKept = !isKept;
    card.dataset.kept = newKept;
    card.classList.toggle("kept", newKept);
    btn.textContent = newKept ? "unkeep" : "keep";
    btn.classList.toggle("active", newKept);
    const sz = card.querySelector(".sz");
    sz.innerHTML = card.querySelector(".meta .sz").textContent.split(" MB")[0]
      + " MB"
      + (newKept ? " &nbsp;<span style='color:#4caf50'>&#128274; kept</span>" : "");
  } catch(e) {
    alert("Error: " + e.message);
  }
  card.classList.remove("busy");
}

// Delete button handler — asks for confirmation, then removes the card
async function onDelete(btn) {
  const card    = btn.closest(".card");
  const clipKey = card.dataset.key;
  const name    = clipKey.split("/").pop();

  if (!confirm("Delete " + name + "?\n\nThis cannot be undone.")) return;

  card.classList.add("busy");
  try {
    await manageClip("delete", clipKey);
    card.remove();
    // Update count in status bar
    const remaining = document.querySelectorAll(".card").length;
    setStatus(remaining + " clip" + (remaining !== 1 ? "s" : ""), "ok");
  } catch(e) {
    card.classList.remove("busy");
    alert("Error: " + e.message);
  }
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function init() {
  try {
    g_token = await getValidToken();
    if (!g_token) return;
  } catch(e) {
    setStatus("Auth error: " + e.message, "err");
    return;
  }

  setStatus("Fetching clips...");
  try {
    const r = await fetch(CFG.apiUrl + "/list", {
      headers: { Authorization: "Bearer " + g_token },
    });
    if (!r.ok) throw new Error("API error: " + r.status);
    const clips = await r.json();
    renderGallery(clips);
    setStatus(clips.length + " clip" + (clips.length !== 1 ? "s" : ""), "ok");
  } catch(e) {
    setStatus("Error: " + e.message, "err");
  }
}

init();
</script>
</body>
</html>
