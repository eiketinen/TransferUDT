const loginPanel = document.querySelector("#login");
const appPanel = document.querySelector("#app");
const loginForm = document.querySelector("#login-form");
const loginError = document.querySelector("#login-error");

loginForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  loginError.textContent = "";
  const password = document.querySelector("#password").value;
  const response = await fetch("/api/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ password })
  });
  if (!response.ok) {
    loginError.textContent = "Login inválido ou configuração incompleta.";
    return;
  }
  await showApp();
});

document.querySelector("#refresh").addEventListener("click", loadAll);

async function showApp() {
  loginPanel.hidden = true;
  appPanel.hidden = false;
  await loadAll();
}

async function checkSession() {
  const response = await fetch("/api/session");
  const data = await response.json();
  if (data.authenticated) await showApp();
}

async function loadAll() {
  const emptyOverview = {
    agentsOnline: 0,
    agentsOffline: 0,
    pendingFiles: 0,
    failedFiles: 0,
    server: {
      dbExists: false,
      logExists: false,
      reconstructedFiles: 0,
      chunks: 0,
      errors: 0
    }
  };
  const [overview, agents, transfers, logs] = await Promise.all([
    getJson("/api/overview", emptyOverview),
    getJson("/api/agents", []),
    getJson("/api/transfers", []),
    getJson("/api/logs?limit=120", [])
  ]);

  document.querySelector("#updated-at").textContent = `Atualizado em ${new Date().toLocaleString()}`;
  document.querySelector("#agents-online").textContent = overview.agentsOnline;
  document.querySelector("#agents-offline").textContent = overview.agentsOffline;
  document.querySelector("#pending-files").textContent = overview.pendingFiles;
  document.querySelector("#failed-files").textContent = overview.failedFiles;

  document.querySelector("#agents-table").innerHTML = agents.map(agent => `
    <tr>
      <td>${escapeHtml(agent.clientId)}</td>
      <td>${escapeHtml(agent.hostname)}</td>
      <td class="${agent.isOnline ? "ok" : "bad"}">${agent.isOnline ? "online" : "offline"}</td>
      <td>${agent.pendingFiles}</td>
      <td>${agent.failedFiles}</td>
      <td>${new Date(agent.lastHeartbeatAt).toLocaleString()}</td>
    </tr>
  `).join("");

  const server = overview.server ?? emptyOverview.server;
  document.querySelector("#server-status").innerHTML = `
    <div><strong>DB</strong><br>${server.dbExists ? "encontrado" : "ausente"}</div>
    <div><strong>Log</strong><br>${server.logExists ? "encontrado" : "ausente"}</div>
    <div><strong>Reconstruídos</strong><br>${server.reconstructedFiles}</div>
    <div><strong>Chunks</strong><br>${server.chunks}</div>
    <div><strong>Erros</strong><br>${server.errors}</div>
  `;

  document.querySelector("#transfers-table").innerHTML = transfers.map(item => `
    <tr>
      <td>${escapeHtml(item.clientId)}</td>
      <td>${escapeHtml(item.filename)}</td>
      <td>${escapeHtml(item.status)}</td>
      <td>${escapeHtml(item.completedAt)}</td>
    </tr>
  `).join("");

  document.querySelector("#logs").textContent = logs
    .map(line => `[${line.source}] ${line.level} ${line.clientId ?? ""} ${line.message}`)
    .join("\n");
}

async function getJson(url, fallback) {
  try {
    const response = await fetch(url);
    if (response.status === 401) {
      loginPanel.hidden = false;
      appPanel.hidden = true;
      return fallback;
    }
    if (!response.ok) {
      return fallback;
    }
    return response.json();
  } catch {
    return fallback;
  }
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, ch => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "\"": "&quot;",
    "'": "&#39;"
  }[ch]));
}

checkSession();
