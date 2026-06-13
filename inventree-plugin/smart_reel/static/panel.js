/* SmartReel web-UI panels (no build step; ES module loaded by InvenTree).
 *
 * Exports:
 *   renderBuildPanel(target, data)     - Build Order page: send/track pick job
 *   renderProvisionPanel(target, data) - Rack location page: HMI setup QR
 *
 * `data` is provided by InvenTree: { model, id, context, ... }. All calls go
 * to our own plugin API with the browser session + CSRF token.
 */

const API = "/plugin/smartreel/api/v1";

function csrftoken() {
    const m = document.cookie.match(/(?:^|;\s*)csrftoken=([^;]+)/);
    return m ? m[1] : "";
}

async function call(method, path, body) {
    const r = await fetch(API + path, {
        method,
        headers: {
            "Accept": "application/json",
            "Content-Type": "application/json",
            "X-CSRFToken": csrftoken(),
        },
        body: body === undefined ? undefined : JSON.stringify(body),
    });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(data.detail || `${r.status} ${r.statusText}`);
    return data;
}

function esc(s) {
    return String(s ?? "").replace(/[&<>"']/g, (c) => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
}

const CSS = `
  .sr-panel { font-size: 14px; }
  .sr-panel table { width: 100%; border-collapse: collapse; margin-top: 8px; }
  .sr-panel th, .sr-panel td { text-align: left; padding: 4px 8px;
      border-bottom: 1px solid rgba(128,128,128,.25); }
  .sr-panel .sr-muted { opacity: .65; }
  .sr-panel .sr-err { color: #c92a2a; margin-top: 8px; }
  .sr-panel button { margin: 8px 8px 0 0; padding: 6px 14px; cursor: pointer; }
  .sr-badge { display: inline-block; padding: 1px 10px; border-radius: 10px;
      border: 1px solid currentColor; font-size: 12px; text-transform: uppercase; }
  .sr-badge.pending { color: #868e96; }
  .sr-badge.partial { color: #e8590c; }
  .sr-badge.done { color: #2f9e44; }
  .sr-qr svg { width: 220px; height: 220px; background: #fff; padding: 8px;
      border-radius: 6px; display: block; margin: 10px 0; }
  .sr-payload { font-family: monospace; font-size: 11px; word-break: break-all;
      opacity: .65; margin-top: 6px; }
`;

function shell(target) {
    target.innerHTML = `<style>${CSS}</style><div class="sr-panel">loading…</div>`;
    return target.querySelector(".sr-panel");
}

/* ---------------- Build Order panel ---------------- */

export async function renderBuildPanel(target, data) {
    const root = shell(target);
    const buildId = data?.context?.build_id ?? data?.id;

    async function refresh() {
        let jobs;
        try {
            jobs = (await call("GET", "/pickjobs")).jobs || [];
        } catch (e) {
            root.innerHTML = `<div class="sr-err">SmartReel API error: ${esc(e.message)}</div>`;
            return;
        }
        const job = jobs.find((j) => j.build_id === buildId);
        job ? renderJob(job) : renderEmpty();
    }

    function renderEmpty() {
        root.innerHTML = `
            <p>No SmartReel pick job for this build order.</p>
            <p class="sr-muted">Sending creates one item per BOM line; the
            SmartReel HMI lights up the slots that hold each part and reels
            transfer to the staging location as they're picked.</p>
            <button id="sr-send">Send to SmartReel</button>
            <div class="sr-err" id="sr-msg"></div>`;
        root.querySelector("#sr-send").addEventListener("click", async () => {
            try {
                await call("POST", "/pickjobs/from-build",
                    { build_id: buildId, op_id: `panel-${buildId}-${Date.now()}` });
                refresh();
            } catch (e) {
                root.querySelector("#sr-msg").textContent = e.message;
            }
        });
    }

    function renderJob(job) {
        const rows = job.items.map((it) => `
            <tr>
                <td>${it.picked ? "✅" : "·"}</td>
                <td>${esc(it.part_id)}</td>
                <td>${esc(it.part_name)}</td>
                <td>${it.qty}</td>
                <td>${it.picked ? "" :
                    (it.located_slots.length
                        ? "slot " + it.located_slots.join(", ")
                        : '<span class="sr-muted">not in rack</span>')}</td>
            </tr>`).join("");
        root.innerHTML = `
            <p>Pick job <b>${esc(job.id)}</b>
               <span class="sr-badge ${esc(job.status)}">${esc(job.status)}</span>
               <span class="sr-muted">requested ${esc(job.requested_at)}</span></p>
            <table>
                <tr><th></th><th>Part</th><th>Name</th><th>Qty</th><th>Location</th></tr>
                ${rows}
            </table>
            <button id="sr-resend">Resend (reset progress)</button>
            <button id="sr-remove">Remove from SmartReel</button>
            <div class="sr-err" id="sr-msg"></div>`;
        root.querySelector("#sr-resend").addEventListener("click", async () => {
            try {
                await call("POST", "/pickjobs/from-build",
                    { build_id: buildId, op_id: `panel-${buildId}-${Date.now()}` });
                refresh();
            } catch (e) {
                root.querySelector("#sr-msg").textContent = e.message;
            }
        });
        root.querySelector("#sr-remove").addEventListener("click", async () => {
            try {
                await call("DELETE", `/pickjobs/${encodeURIComponent(job.id)}`);
                refresh();
            } catch (e) {
                root.querySelector("#sr-msg").textContent = e.message;
            }
        });
    }

    refresh();
}

/* ---------------- Provisioning panel (rack location page) ---------------- */

export async function renderProvisionPanel(target, _data) {
    const root = shell(target);

    async function refresh(rotate) {
        let p;
        try {
            p = await call("GET", "/provision" + (rotate ? "?rotate=1" : ""));
        } catch (e) {
            root.innerHTML = `<div class="sr-err">${esc(e.message)}</div>`;
            return;
        }
        root.innerHTML = `
            <p>Scan from the HMI: <b>Settings → Network → Scan setup code</b>.
               No typing required.</p>
            <div class="sr-qr">${p.svg ||
                '<div class="sr-muted">QR rendering unavailable — use the payload below</div>'}</div>
            <div class="sr-muted">Server: ${esc(p.base_url)} ·
                token <code>${esc(p.token_name)}</code></div>
            <div class="sr-payload">${esc(p.payload)}</div>
            <button id="sr-rotate">Rotate token (revokes the old one)</button>
            <div class="sr-err" id="sr-msg"></div>`;
        root.querySelector("#sr-rotate").addEventListener("click", () => refresh(true));
    }

    refresh(false);
}
