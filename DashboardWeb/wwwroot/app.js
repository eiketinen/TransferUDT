const loginPanel = document.querySelector("#login");
const appPanel = document.querySelector("#app");
const loginForm = document.querySelector("#login-form");
const loginError = document.querySelector("#login-error");
let refreshTimer = null;
let refreshSeconds = 30;

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
    loginError.textContent = "Login invalido ou configuracao incompleta.";
    return;
  }
  await showApp();
});

document.querySelector("#refresh").addEventListener("click", loadAll);
document.querySelector("#auto-refresh").addEventListener("change", scheduleRefresh);
document.querySelector("#metrics-window").addEventListener("change", loadAll);
document.querySelector("#log-source").addEventListener("change", loadLogs);
document.querySelector("#log-level").addEventListener("change", loadLogs);
document.querySelector("#log-query").addEventListener("input", debounce(loadLogs, 250));

async function showApp() {
  loginPanel.hidden = true;
  appPanel.hidden = false;
  await loadAll();
  scheduleRefresh();
}

async function checkSession() {
  const response = await fetch("/api/session");
  const data = await response.json();
  if (data.authenticated) await showApp();
}

async function loadAll() {
  const windowHours = Number(document.querySelector("#metrics-window").value || 24);
  const [overview, agents, transfers, health, metrics] = await Promise.all([
    getJson("/api/overview", emptyOverview()),
    getJson("/api/agents", []),
    getJson("/api/transfers", []),
    getJson("/api/health", { status: "unhealthy", components: [] }),
    getJson(`/api/metrics?windowHours=${windowHours}`, emptyMetrics(windowHours))
  ]);

  refreshSeconds = metrics.autoRefreshSeconds || 30;
  document.querySelector("#updated-at").textContent = `Atualizado em ${new Date().toLocaleString()}`;
  setText("#agents-online", metrics.agentsOnline);
  setText("#agents-offline", metrics.agentsOffline);
  setText("#pending-files", metrics.pendingFiles);
  setText("#failed-files", metrics.failedFiles);
  setText("#completed-window", metrics.server.completedTransfersInWindow);
  setText("#server-errors-window", metrics.server.errorEventsInWindow);
  document.querySelector("#completed-window").nextElementSibling.textContent = `Concluidas em ${metrics.windowHours}h`;
  document.querySelector("#server-errors-window").nextElementSibling.textContent = `Erros do Server em ${metrics.windowHours}h`;
  document.querySelector("#heartbeat-samples").textContent = metrics.heartbeatSamples === 1
    ? "1 heartbeat na janela"
    : `${metrics.heartbeatSamples} heartbeats na janela`;

  renderHealth(health);
  renderAgents(agents);
  renderServer(overview.server ?? emptyOverview().server, metrics.server);
  renderTrend(metrics.trend);
  renderAlerts(metrics.alerts);
  renderTransfers(transfers);
  await loadLogs();
  scheduleRefresh();
}

async function loadLogs() {
  const params = new URLSearchParams({ limit: "200" });
  const source = document.querySelector("#log-source").value;
  const level = document.querySelector("#log-level").value;
  const query = document.querySelector("#log-query").value.trim();
  if (source) params.set("source", source);
  if (level) params.set("level", level);
  if (query) params.set("q", query);
  const logs = await getJson(`/api/logs?${params}`, []);
  document.querySelector("#logs").textContent = logs
    .map(line => `[${line.source}] ${line.level} ${line.clientId ?? ""} ${line.message}`)
    .join("\n");
}

function renderHealth(health) {
  const badge = document.querySelector("#overall-health");
  badge.textContent = health.status || "unhealthy";
  badge.className = `status-badge ${statusClass(health.status)}`;
  document.querySelector("#health-components").innerHTML = (health.components || []).map(component => `
    <div class="health-item">
      <span class="status-dot ${statusClass(component.status)}" aria-hidden="true"></span>
      <div><strong>${escapeHtml(componentLabel(component.name))}</strong><span>${escapeHtml(component.message)}</span></div>
    </div>
  `).join("") || '<p class="empty">Sem dados de saude.</p>';
}

function renderAgents(agents) {
  document.querySelector("#agents-table").innerHTML = agents.map(agent => `
    <tr>
      <td>${escapeHtml(agent.clientId)}</td>
      <td>${escapeHtml(agent.hostname)}</td>
      <td class="${agent.isOnline ? "ok" : "bad"}">${agent.isOnline ? "online" : "offline"}</td>
      <td>${agent.pendingFiles}</td>
      <td>${agent.failedFiles}</td>
      <td>${formatBytes(agent.diskFreeBytes)}</td>
      <td>${new Date(agent.lastHeartbeatAt).toLocaleString()}</td>
    </tr>
  `).join("") || '<tr><td colspan="7" class="empty">Nenhum Agent conhecido.</td></tr>';
}

function renderServer(status, metrics) {
  document.querySelector("#server-status").innerHTML = `
    <div><strong>Banco</strong><span>${status.dbExists ? "disponivel" : "ausente"}</span></div>
    <div><strong>Log</strong><span>${status.logExists ? "disponivel" : "ausente"}</span></div>
    <div><strong>Transferencias</strong><span>${metrics.completedTransfersTotal}</span></div>
    <div><strong>Chunks armazenados</strong><span>${metrics.storedChunks}</span></div>
    <div><strong>Chunks pendentes</strong><span>${metrics.pendingChunks}</span></div>
    <div><strong>Chunks com falha</strong><span>${metrics.failedChunks}</span></div>
    <div class="wide"><strong>Ultima conclusao</strong><span>${metrics.lastCompletedAt ? escapeHtml(metrics.lastCompletedAt) : "sem registro"}</span></div>
  `;
}

function renderTrend(points) {
  if (!points || points.length === 0) {
    document.querySelector("#trend").innerHTML = '<p class="empty">Sem amostras na janela selecionada.</p>';
    return;
  }
  const maxBacklog = Math.max(1, ...points.map(point => point.pendingFiles + point.failedFiles));
  document.querySelector("#trend").innerHTML = points.slice(-48).map(point => {
    const pendingWidth = Math.max(2, point.pendingFiles / maxBacklog * 100);
    const failedWidth = point.failedFiles === 0 ? 0 : Math.max(2, point.failedFiles / maxBacklog * 100);
    return `
      <div class="trend-row" title="${point.reportingAgents} Agent(s), ${point.pendingFiles} pendente(s), ${point.failedFiles} falha(s)">
        <time>${new Date(point.timestamp).toLocaleString([], { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" })}</time>
        <div class="bars"><span class="pending-bar" style="width:${pendingWidth}%"></span><span class="failed-bar" style="width:${failedWidth}%"></span></div>
        <strong>${point.pendingFiles + point.failedFiles}</strong>
      </div>`;
  }).join("");
}

function renderAlerts(alerts) {
  document.querySelector("#alerts").innerHTML = (alerts || []).map(alert => `
    <div class="alert ${alert.severity === "ERROR" ? "alert-error" : "alert-warn"}">
      <strong>${escapeHtml(alert.clientId || alert.source)}</strong>
      <span>${escapeHtml(alert.message)}</span>
      <time>${new Date(alert.at).toLocaleString()}</time>
    </div>
  `).join("") || '<p class="empty">Nenhum alerta ativo.</p>';
}

function renderTransfers(transfers) {
  document.querySelector("#transfers-table").innerHTML = transfers.map(item => `
    <tr>
      <td>${escapeHtml(item.clientId)}</td>
      <td>${escapeHtml(item.filename)}</td>
      <td>${escapeHtml(item.status)}</td>
      <td>${escapeHtml(item.completedAt)}</td>
    </tr>
  `).join("") || '<tr><td colspan="4" class="empty">Nenhuma transferencia concluida.</td></tr>';
}

function scheduleRefresh() {
  if (refreshTimer) clearInterval(refreshTimer);
  refreshTimer = null;
  if (document.querySelector("#auto-refresh").checked && !appPanel.hidden) {
    refreshTimer = setInterval(loadAll, Math.max(10, refreshSeconds) * 1000);
  }
}

async function getJson(url, fallback) {
  try {
    const response = await fetch(url);
    if (response.status === 401) {
      loginPanel.hidden = false;
      appPanel.hidden = true;
      if (refreshTimer) clearInterval(refreshTimer);
      return fallback;
    }
    return response.ok ? response.json() : fallback;
  } catch {
    return fallback;
  }
}

function emptyOverview() {
  return { server: { dbExists: false, logExists: false, reconstructedFiles: 0, chunks: 0, errors: 0 } };
}

function emptyMetrics(windowHours) {
  return {
    windowHours,
    autoRefreshSeconds: 30,
    agentsOnline: 0,
    agentsOffline: 0,
    pendingFiles: 0,
    failedFiles: 0,
    heartbeatSamples: 0,
    server: { completedTransfersTotal: 0, completedTransfersInWindow: 0, errorEventsInWindow: 0, pendingChunks: 0, failedChunks: 0, storedChunks: 0, lastCompletedAt: null },
    trend: [],
    alerts: []
  };
}

function componentLabel(name) {
  return ({ dashboardDatabase: "Banco do Dashboard", serverDatabase: "Banco do Server", serverLog: "Log do Server", agents: "Agents" })[name] || name;
}

function statusClass(status) {
  return status === "healthy" ? "healthy" : status === "degraded" ? "degraded" : "unhealthy";
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return "n/d";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit += 1; }
  return `${value.toFixed(unit < 2 ? 0 : 1)} ${units[unit]}`;
}

function setText(selector, value) {
  document.querySelector(selector).textContent = value ?? 0;
}

function debounce(fn, wait) {
  let timeout;
  return (...args) => {
    clearTimeout(timeout);
    timeout = setTimeout(() => fn(...args), wait);
  };
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, ch => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;" }[ch]));
}

checkSession();
