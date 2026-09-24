/**
 * ESP32-C3 Fall Detection Dashboard Logic
 * Auto-refreshes backend status, latest event, telemetry, alert state, and history table.
 */

document.addEventListener("DOMContentLoaded", () => {
    // Initial fetch
    fetchDashboardData();

    // Auto refresh every 2 seconds
    setInterval(fetchDashboardData, 2000);
});

async function fetchDashboardData() {
    try { await checkHealthStatus(); } catch (e) { console.error("Health check error:", e); }
    try { await fetchDeviceStatus(); } catch (e) { console.error("Device status error:", e); }
    try { await fetchLatestEvent(); } catch (e) { console.error("Latest event error:", e); }
    try { await fetchEventHistory(); } catch (e) { console.error("Event history error:", e); }
}

/**
 * Check backend health status via GET /health
 */
async function checkHealthStatus() {
    const backendBadge = document.getElementById("backend-badge");
    const statusBackend = document.getElementById("status-backend");
    const statusBackendSub = document.getElementById("status-backend-sub");
    const statusDatabase = document.getElementById("status-database");
    const statusDatabaseSub = document.getElementById("status-database-sub");

    try {
        const response = await fetch("/health", { cache: "no-store" });
        if (response.ok) {
            const data = await response.json();
            if (data.status === "ok") {
                // Header badge
                if (backendBadge) {
                    backendBadge.className = "badge badge-online";
                    backendBadge.innerHTML = '<span class="dot"></span> BACKEND ONLINE';
                }

                // Backend card
                if (statusBackend) {
                    statusBackend.textContent = "🟢 ONLINE";
                    statusBackend.className = "status-value state-online";
                }
                if (statusBackendSub) {
                    statusBackendSub.textContent = "HTTP 200 OK";
                    statusBackendSub.title = "FastAPI service active on /health";
                }

                // Database card
                if (statusDatabase) {
                    statusDatabase.textContent = data.database_connected ? "🟢 ONLINE" : "🔴 OFFLINE";
                    statusDatabase.className = data.database_connected ? "status-value state-online" : "status-value state-offline";
                }
                if (statusDatabaseSub) {
                    statusDatabaseSub.textContent = data.database_connected ? "SQLite Storage Connected" : "Database Error";
                }

                // Gemini card
                updateGeminiUI(data.gemini_status, data.gemini_last_error, data.gemini_last_success_time);

                // Telegram card
                updateTelegramUI(data.telegram_status, data.telegram_last_error, data.telegram_last_success_time);

                return;
            }
        }
        setBackendOffline();
    } catch (err) {
        setBackendOffline();
    }
}

/**
 * Fetch real-time device heartbeat status via GET /api/device/status
 */
async function fetchDeviceStatus() {
    const statusWearable = document.getElementById("status-wearable");
    const statusWearableSub = document.getElementById("status-wearable-sub");

    try {
        const response = await fetch("/api/device/status?device_id=ESP32C3_01", { cache: "no-store" });
        if (!response.ok) return;

        const data = await response.json();
        const devId = data.device_id || "ESP32C3_01";

        if (data.status === "online") {
            if (statusWearable) {
                statusWearable.textContent = "🟢 ONLINE";
                statusWearable.className = "status-value state-online";
            }
            if (statusWearableSub) {
                const sec = data.seconds_since_last_seen != null ? data.seconds_since_last_seen : 0;
                const rssiInfo = data.wifi_rssi != null ? ` (RSSI: ${data.wifi_rssi} dBm)` : "";
                statusWearableSub.textContent = `Last seen: ${sec}s ago${rssiInfo}`;
                statusWearableSub.title = `Device '${devId}' heartbeat active (${sec}s ago)`;
            }
        } else if (data.status === "offline") {
            if (statusWearable) {
                statusWearable.textContent = "🔴 OFFLINE";
                statusWearable.className = "status-value state-offline";
            }
            if (statusWearableSub) {
                const sec = data.seconds_since_last_seen != null ? data.seconds_since_last_seen : 30;
                const ageStr = formatDuration(sec);
                statusWearableSub.textContent = `Last seen: ${ageStr} ago`;
                statusWearableSub.title = `Device '${devId}' offline. No heartbeat received for ${ageStr}`;
            }
        } else {
            // never_connected
            if (statusWearable) {
                statusWearable.textContent = "🟡 NEVER CONNECTED";
                statusWearable.className = "status-value state-warning";
            }
            if (statusWearableSub) {
                statusWearableSub.textContent = "Awaiting first heartbeat ping";
                statusWearableSub.title = `Device '${devId}' has not sent any heartbeat yet`;
            }
        }
    } catch (err) {
        console.error("Error fetching device status:", err);
    }
}

function formatDuration(sec) {
    if (sec < 60) return `${sec}s`;
    const mins = Math.floor(sec / 60);
    const remSec = sec % 60;
    if (mins < 60) return `${mins}m ${remSec}s`;
    const hours = Math.floor(mins / 60);
    return `${hours}h ${mins % 60}m`;
}

function setBackendOffline() {
    const backendBadge = document.getElementById("backend-badge");
    const statusBackend = document.getElementById("status-backend");
    const statusBackendSub = document.getElementById("status-backend-sub");

    const statusGemini = document.getElementById("status-gemini");
    const statusGeminiSub = document.getElementById("status-gemini-sub");

    const statusTelegram = document.getElementById("status-telegram");
    const statusTelegramSub = document.getElementById("status-telegram-sub");

    const statusDatabase = document.getElementById("status-database");
    const statusDatabaseSub = document.getElementById("status-database-sub");

    const statusWearable = document.getElementById("status-wearable");
    const statusWearableSub = document.getElementById("status-wearable-sub");

    if (backendBadge) {
        backendBadge.className = "badge badge-offline";
        backendBadge.innerHTML = '<span class="dot"></span> BACKEND OFFLINE';
    }

    if (statusBackend) {
        statusBackend.textContent = "🔴 OFFLINE";
        statusBackend.className = "status-value state-offline";
    }
    if (statusBackendSub) {
        statusBackendSub.textContent = "Unreachable (Server Stopped)";
        statusBackendSub.title = "Failed to connect to FastAPI /health";
    }

    if (statusDatabase) {
        statusDatabase.textContent = "🔴 OFFLINE";
        statusDatabase.className = "status-value state-offline";
    }
    if (statusDatabaseSub) {
        statusDatabaseSub.textContent = "Backend Unreachable";
    }

    if (statusWearable) {
        statusWearable.textContent = "🔴 UNKNOWN";
        statusWearable.className = "status-value state-offline";
    }
    if (statusWearableSub) {
        statusWearableSub.textContent = "Backend Offline";
        statusWearableSub.title = "Backend is unreachable";
    }

    if (statusGemini) {
        statusGemini.textContent = "🔴 UNKNOWN";
        statusGemini.className = "status-value state-offline";
    }
    if (statusGeminiSub) {
        statusGeminiSub.textContent = "Backend Offline";
        statusGeminiSub.title = "Backend is unreachable";
    }

    if (statusTelegram) {
        statusTelegram.textContent = "🔴 UNKNOWN";
        statusTelegram.className = "status-value state-offline";
    }
    if (statusTelegramSub) {
        statusTelegramSub.textContent = "Backend Offline";
        statusTelegramSub.title = "Backend is unreachable";
    }
}

function updateGeminiUI(status, lastError, lastSuccessTime) {
    const el = document.getElementById("status-gemini");
    const sub = document.getElementById("status-gemini-sub");
    if (!el || !sub) return;

    if (status === "not_configured") {
        el.textContent = "🟡 NOT CONFIGURED";
        el.className = "status-value state-warning";
        const msg = lastError || "GEMINI_API_KEY is not configured";
        sub.textContent = msg;
        sub.title = msg;
    } else if (status === "configured") {
        el.textContent = "🟡 CONFIGURED";
        el.className = "status-value state-warning";
        sub.textContent = "Status: Not recently tested";
        sub.title = "GEMINI_API_KEY is set. Awaiting first event assessment.";
    } else if (status === "working" || status === "success") {
        el.textContent = "🟢 WORKING";
        el.className = "status-value state-online";
        const tsMsg = lastSuccessTime ? ` (${formatTimestamp(lastSuccessTime)})` : "";
        sub.textContent = "Last request successful" + tsMsg;
        sub.title = "Last Gemini AI assessment succeeded" + tsMsg;
    } else if (status === "error") {
        el.textContent = "🔴 ERROR";
        el.className = "status-value state-offline";
        const reason = lastError || "Gemini request failed";
        sub.textContent = "Reason: " + reason;
        sub.title = "Reason: " + reason;
    } else {
        el.textContent = "🟡 CONFIGURED";
        el.className = "status-value state-warning";
        sub.textContent = "Status: Not recently tested";
        sub.title = "Status: Not recently tested";
    }
}

function updateTelegramUI(status, lastError, lastSuccessTime) {
    const el = document.getElementById("status-telegram");
    const sub = document.getElementById("status-telegram-sub");
    if (!el || !sub) return;

    if (status === "not_configured") {
        el.textContent = "🟡 NOT CONFIGURED";
        el.className = "status-value state-warning";
        const msg = lastError || "TELEGRAM_BOT_TOKEN or CHAT_ID unconfigured";
        sub.textContent = msg;
        sub.title = msg;
    } else if (status === "configured") {
        el.textContent = "🟡 CONFIGURED";
        el.className = "status-value state-warning";
        sub.textContent = "Status: Not recently tested";
        sub.title = "Telegram credentials set. Awaiting first fall alert.";
    } else if (status === "delivered" || status === "success") {
        el.textContent = "🟢 DELIVERED";
        el.className = "status-value state-online";
        const tsMsg = lastSuccessTime ? ` (${formatTimestamp(lastSuccessTime)})` : "";
        sub.textContent = "Last delivery successful" + tsMsg;
        sub.title = "Last Telegram alert delivered successfully" + tsMsg;
    } else if (status === "failed" || status === "error") {
        el.textContent = "🔴 FAILED";
        el.className = "status-value state-offline";
        const reason = lastError || "Telegram delivery failed";
        sub.textContent = "Reason: " + reason;
        sub.title = "Reason: " + reason;
    } else {
        el.textContent = "🟡 CONFIGURED";
        el.className = "status-value state-warning";
        sub.textContent = "Status: Not recently tested";
        sub.title = "Status: Not recently tested";
    }
}

/**
 * Normalize alert status for visual badge display
 */
function normalizeStatus(statusRaw, failureReason) {
    if (!statusRaw) {
        if (failureReason && failureReason.trim() !== "") return { label: "FAILED", class: "status-failed" };
        return { label: "PENDING", class: "status-pending" };
    }
    const s = String(statusRaw).toUpperCase();
    if (s === "SENT" || s === "ALERT_SENT") {
        return { label: "SENT", class: "status-sent" };
    }
    if (s === "FAILED" || s === "ALERT_FAILED" || s === "ALERT_FAILED_OR_SKIPPED") {
        return { label: "FAILED", class: "status-failed" };
    }
    if (s === "CANCELLED" || s === "CANCELED") {
        return { label: "CANCELLED", class: "status-cancelled" };
    }
    if (s === "PENDING" || s === "RECEIVED" || s === "ASSESSED") {
        return { label: "PENDING", class: "status-pending" };
    }
    if (failureReason && failureReason.trim() !== "") return { label: "FAILED", class: "status-failed" };
    return { label: s, class: "status-pending" };
}

/**
 * Fetch latest fall event via GET /api/dashboard/latest
 */
async function fetchLatestEvent() {
    try {
        const response = await fetch("/api/dashboard/latest", { cache: "no-store" });
        if (!response.ok) {
            resetLatestMetrics();
            return;
        }

        const data = await response.json();

        // Handle case where endpoint returns {"event": null} or null
        const event = (data && data.event === null) ? null : data;

        if (!event || (!event.event_id && !event.id)) {
            resetLatestMetrics();
            return;
        }

        updateLatestMetrics(event);
    } catch (err) {
        console.error("Error fetching latest event:", err);
    }
}

function resetLatestMetrics() {
    const devEl = document.getElementById("status-device");
    if (devEl) devEl.textContent = "ESP32C3_01";
    const lastEvEl = document.getElementById("status-lastevent");
    if (lastEvEl) lastEvEl.textContent = "No events recorded";

    document.getElementById("card-impact").textContent = "--";
    document.getElementById("card-rotation").textContent = "--";
    document.getElementById("card-posture").textContent = "--";
    document.getElementById("card-stillness").textContent = "--";
    document.getElementById("card-confidence").textContent = "--";

    // Show normal panel, hide alert panel
    document.getElementById("normal-status-panel").classList.remove("hidden");
    document.getElementById("fall-alert-panel").classList.add("hidden");
}

function updateLatestMetrics(event) {
    // Device ID
    const devEl = document.getElementById("status-device");
    if (devEl) devEl.textContent = event.device_id || "ESP32C3_01";

    // Last Event Time
    const rawTime = event.created_at || event.timestamp || "";
    const lastEvEl = document.getElementById("status-lastevent");
    if (lastEvEl) lastEvEl.textContent = formatTimestamp(rawTime);

    // Telemetry Cards
    const impact = event.impact_g != null ? event.impact_g.toFixed(2) : "--";
    const rotation = event.rotation_rads != null ? event.rotation_rads.toFixed(2) : "--";
    const posture = event.posture_change_deg != null ? event.posture_change_deg.toFixed(1) : "--";
    const stillness = event.stillness_variation != null ? event.stillness_variation.toFixed(2) : "--";
    const confidenceVal = event.fall_confidence != null ? Math.round(event.fall_confidence * 100) : 0;
    const confidenceStr = event.fall_confidence != null ? confidenceVal + "%" : "--";

    document.getElementById("card-impact").textContent = impact;
    document.getElementById("card-rotation").textContent = rotation;
    document.getElementById("card-posture").textContent = posture;
    document.getElementById("card-stillness").textContent = stillness;
    document.getElementById("card-confidence").textContent = confidenceStr;

    // Determine if it represents a fall alert
    const isFall = (event.fall_confidence != null && event.fall_confidence >= 0.5);

    const normalPanel = document.getElementById("normal-status-panel");
    const alertPanel = document.getElementById("fall-alert-panel");

    if (isFall) {
        normalPanel.classList.add("hidden");
        alertPanel.classList.remove("hidden");

        // Status Badge
        const statusInfo = normalizeStatus(event.alert_status, event.failure_reason);
        const statusBadge = document.getElementById("alert-status-badge");
        statusBadge.textContent = `Status: ${statusInfo.label}`;
        statusBadge.className = `badge-status ${statusInfo.class}`;

        // Severity Badge
        const severity = (event.severity || "HIGH").toUpperCase();
        const sevBadge = document.getElementById("alert-severity-badge");
        sevBadge.textContent = `SEVERITY: ${severity}`;
        
        // Telegram Status
        const tgBadge = document.getElementById("alert-telegram-badge");
        if (event.telegram_delivered) {
            tgBadge.textContent = "Telegram: ✅ DELIVERED";
            tgBadge.className = "badge-telegram delivered";
        } else {
            tgBadge.textContent = "Telegram: ❌ NOT DELIVERED";
            tgBadge.className = "badge-telegram failed";
        }

        // Telemetry breakdown
        document.getElementById("alert-time").textContent = formatTimestamp(rawTime);
        document.getElementById("alert-confidence").textContent = confidenceStr;
        document.getElementById("alert-impact").textContent = impact + " g";
        document.getElementById("alert-rotation").textContent = rotation + " rad/s";
        document.getElementById("alert-posture").textContent = posture + "°";
        document.getElementById("alert-stillness").textContent = stillness + " m/s²";

        // Gemini AI fields
        document.getElementById("alert-gemini-assessment").textContent = event.assessment || "No assessment available.";
        document.getElementById("alert-caregiver-message").textContent = event.caregiver_message || "No caregiver message available.";
        document.getElementById("alert-recommended-action").textContent = event.recommended_action || "No recommended action available.";

        // Failure Reason section (only shown when alert failed or failure_reason is present)
        const failBox = document.getElementById("alert-failure-reason-box");
        const failText = document.getElementById("alert-failure-reason");
        const showFailure = (statusInfo.label === "FAILED") || (event.failure_reason && event.failure_reason.trim() !== "");

        if (showFailure && event.failure_reason && event.failure_reason.trim() !== "") {
            failText.textContent = event.failure_reason;
            failBox.classList.remove("hidden");
        } else {
            failBox.classList.add("hidden");
        }
    } else {
        alertPanel.classList.add("hidden");
        normalPanel.classList.remove("hidden");
    }
}

/**
 * Fetch 10 most recent events via GET /api/events?limit=10
 */
async function fetchEventHistory() {
    const tbody = document.getElementById("history-table-body");
    try {
        const response = await fetch("/api/events?limit=10", { cache: "no-store" });
        if (!response.ok) return;

        const data = await response.json();
        const events = data.events || [];

        if (events.length === 0) {
            tbody.innerHTML = '<tr><td colspan="9" class="text-center">No fall events recorded in database.</td></tr>';
            return;
        }

        let html = "";
        events.forEach(ev => {
            const timeStr = formatTimestamp(ev.created_at || ev.timestamp);
            const devId = escapeHtml(ev.device_id || "ESP32C3_01");
            const impactStr = ev.impact_g != null ? ev.impact_g.toFixed(2) + " g" : "--";
            const rotStr = ev.rotation_rads != null ? ev.rotation_rads.toFixed(2) + " rad/s" : "--";
            const posStr = ev.posture_change_deg != null ? ev.posture_change_deg.toFixed(1) + "°" : "--";
            const confVal = ev.fall_confidence != null ? Math.round(ev.fall_confidence * 100) : 0;
            const confStr = ev.fall_confidence != null ? confVal + "%" : "--";
            
            const severity = (ev.severity || "NORMAL").toUpperCase();
            let sevClass = "sev-low";
            if (severity === "HIGH" || severity === "CRITICAL") sevClass = "sev-high";
            else if (severity === "MEDIUM") sevClass = "sev-medium";

            const statusInfo = normalizeStatus(ev.alert_status, ev.failure_reason);
            const statusBadgeHtml = `<span class="badge-status ${statusInfo.class}">${statusInfo.label}</span>`;

            let failureTd = '<td class="td-no-failure">—</td>';
            if (ev.failure_reason && ev.failure_reason.trim() !== "") {
                const escapedReason = escapeHtml(ev.failure_reason);
                failureTd = `<td class="td-failure-reason" title="${escapedReason}">${escapedReason}</td>`;
            }

            const isAlertRow = (ev.fall_confidence >= 0.5 || statusInfo.label === "FAILED") ? 'class="row-fall-alert"' : '';

            html += `
                <tr ${isAlertRow}>
                    <td>${timeStr}</td>
                    <td>${devId}</td>
                    <td>${impactStr}</td>
                    <td>${rotStr}</td>
                    <td>${posStr}</td>
                    <td><strong>${confStr}</strong></td>
                    <td><span class="sev-tag ${sevClass}">${severity}</span></td>
                    <td>${statusBadgeHtml}</td>
                    ${failureTd}
                </tr>
            `;
        });

        tbody.innerHTML = html;
    } catch (err) {
        console.error("Error fetching event history:", err);
    }
}

/**
 * Format timestamp nicely
 */
function formatTimestamp(tsStr) {
    if (!tsStr) return "--";
    try {
        const d = new Date(tsStr);
        if (!isNaN(d.getTime())) {
            return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) + ' (' + d.toLocaleDateString() + ')';
        }
    } catch (e) {}
    return tsStr;
}

/**
 * Basic HTML escaping to prevent XSS
 */
function escapeHtml(str) {
    if (typeof str !== 'string') return str;
    return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
}
