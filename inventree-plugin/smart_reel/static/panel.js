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
    let racks = [];

    function rackName(id) {
        const r = racks.find((x) => x.location_id === id);
        return r ? r.name : `location ${id}`;
    }

    async function refresh() {
        let jobs;
        try {
            [jobs, racks] = await Promise.all([
                call("GET", "/pickjobs?all=1").then((d) => d.jobs || []),
                call("GET", "/racks").then((d) => d.racks || []),
            ]);
        } catch (e) {
            root.innerHTML = `<div class="sr-err">SmartReel API error: ${esc(e.message)}</div>`;
            return;
        }
        // A build can now have one job per rack (review item 10b).
        const mine = jobs.filter((j) => j.build_id === buildId);
        mine.length ? renderJobs(mine) : renderPicker();
    }

    // ---- Reel picker: choose which reels to pull, then fan out (item 10) ----
    async function renderPicker() {
        let lines;
        try {
            lines = await call("GET", `/pickjobs/options?build_id=${buildId}`)
                .then((d) => d.lines || []);
        } catch (e) {
            root.innerHTML = `<div class="sr-err">SmartReel API error: ${esc(e.message)}</div>`;
            return;
        }
        if (!racks.length) {
            root.innerHTML = `<p class="sr-err">No SmartReel racks configured.
                Provision a rack from its stock-location page first.</p>`;
            return;
        }

        const rows = lines.map((ln) => {
            if (!ln.candidates.length) {
                return `<tr><td></td><td>${esc(ln.part_id)}</td>
                    <td>${esc(ln.part_name)}</td>
                    <td colspan="2" class="sr-muted">no reel in any rack</td></tr>`;
            }
            // Default-check when there's exactly one candidate; force an
            // explicit choice when a part has several reels.
            const auto = ln.candidates.length === 1;
            return ln.candidates.map((c, i) => `
                <tr>
                    <td><input type="checkbox" class="sr-pick" value="${c.stock_id}"
                         ${auto ? "checked" : ""}></td>
                    <td>${i === 0 ? esc(ln.part_id) : ""}</td>
                    <td>${i === 0 ? esc(ln.part_name) : ""}</td>
                    <td>reel #${c.stock_id} · qty ${c.qty}${c.batch ? " · " + esc(c.batch) : ""}</td>
                    <td>${esc(c.rack_name)} slot ${c.slot_num}</td>
                </tr>`).join("");
        }).join("");

        root.innerHTML = `
            <p>Select the reels to pick. Sending fans out
               <b>one job per rack</b>; each rack's HMI lights only its reels.</p>
            <table>
                <tr><th></th><th>Part</th><th>Name</th><th>Reel</th><th>Rack / slot</th></tr>
                ${rows || `<tr><td colspan="5" class="sr-muted">No BOM lines.</td></tr>`}
            </table>
            <button id="sr-send">Send to SmartReel</button>
            <div class="sr-err" id="sr-msg"></div>`;

        root.querySelector("#sr-send").addEventListener("click", async () => {
            const ids = [...root.querySelectorAll(".sr-pick:checked")]
                .map((el) => Number(el.value));
            if (!ids.length) {
                root.querySelector("#sr-msg").textContent = "Tick at least one reel to pick.";
                return;
            }
            try {
                await call("POST", "/pickjobs/from-build", {
                    build_id: buildId,
                    stock_ids: ids,
                    op_id: `panel-${buildId}-${Date.now()}`,
                });
                refresh();
            } catch (e) {
                root.querySelector("#sr-msg").textContent = e.message;
            }
        });
    }

    // ---- Existing jobs (one card per rack) ----
    function renderJobs(jobsForBuild) {
        const cards = jobsForBuild.map((job) => {
            const rows = job.items.map((it) => `
                <tr>
                    <td>${it.picked ? "✅" : "·"}</td>
                    <td>${esc(it.part_id)}</td>
                    <td>${esc(it.part_name)}</td>
                    <td>${it.picked ? "" :
                        (it.located_slots.length
                            ? "slot " + it.located_slots.join(", ")
                            : '<span class="sr-muted">not in rack</span>')}</td>
                </tr>`).join("");
            return `
                <p>Pick job <b>${esc(job.id)}</b>
                   <span class="sr-badge ${esc(job.status)}">${esc(job.status)}</span>
                   <span class="sr-muted">→ ${esc(rackName(job.rack_id))} ·
                   requested ${esc(job.requested_at)}</span></p>
                <table>
                    <tr><th></th><th>Part</th><th>Name</th><th>Location</th></tr>
                    ${rows}
                </table>`;
        }).join("<hr>");

        root.innerHTML = `
            ${cards}
            <div style="margin-top:8px">
                <button id="sr-reselect">Re-select reels (resend)</button>
                <button id="sr-remove">Remove from SmartReel</button>
            </div>
            <div class="sr-err" id="sr-msg"></div>`;
        root.querySelector("#sr-reselect").addEventListener("click", renderPicker);
        root.querySelector("#sr-remove").addEventListener("click", async () => {
            try {
                // One DELETE clears every rack's job for this build.
                await call("DELETE", `/pickjobs/${encodeURIComponent(jobsForBuild[0].id)}`);
                refresh();
            } catch (e) {
                root.querySelector("#sr-msg").textContent = e.message;
            }
        });
    }

    refresh();
}

/* ---------------- Provisioning panel (any stock-location page) ---------------- */

export async function renderProvisionPanel(target, data) {
    const root = shell(target);
    const locationId = data?.context?.location_id ?? data?.id;
    const q = (rotate) =>
        `/provision?location=${encodeURIComponent(locationId)}` + (rotate ? "&rotate=1" : "");

    async function refresh(rotate) {
        let p;
        try {
            p = await call("GET", q(rotate));
        } catch (e) {
            root.innerHTML = `<div class="sr-err">${esc(e.message)}</div>`;
            return;
        }
        root.innerHTML = `
            <p>This location is provisioned as a <b>SmartReel rack</b>. Scan from
               the HMI: <b>Settings → Network → Scan setup code</b> — no typing.</p>
            <div class="sr-qr">${p.svg ||
                '<div class="sr-muted">QR rendering unavailable — use the payload below</div>'}</div>
            <div class="sr-muted">Rack: ${esc(p.rack.name)} · server ${esc(p.base_url)} ·
                token <code>${esc(p.token_name)}</code></div>
            <div class="sr-payload">${esc(p.payload)}</div>
            <button id="sr-rotate">Rotate token (revokes the old one)</button>
            <div class="sr-err" id="sr-msg"></div>`;
        root.querySelector("#sr-rotate").addEventListener("click", () => refresh(true));
    }

    // Lazy: don't designate the location a rack on mere page view. Offer a
    // button; provisioning (which tags it + issues the token) runs on click.
    root.innerHTML = `
        <p>Use this stock location as a SmartReel rack and provision an HMI for it.</p>
        <p class="sr-muted">Provisioning designates this location as a rack, creates
           a setup QR, and issues an API token bound to this rack.</p>
        <button id="sr-provision">Provision SmartReel rack here</button>
        <div class="sr-err" id="sr-msg"></div>`;
    root.querySelector("#sr-provision").addEventListener("click", () => refresh(false));
}
