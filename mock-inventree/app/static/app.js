/* SmartReel mock dashboard.
 *
 * Polls /api/v1/{health,rack,pickjobs,_dev/anomalies,_dev/ops} every 2 s
 * and re-renders. Token comes from localStorage; the form lets you set it
 * without reloading. /health is unauthenticated so we use it as the "online"
 * heartbeat even before a token is set.
 *
 * Vanilla JS only; no build step. Render functions are total -- they wipe
 * and rebuild their target node each tick, so the diff between server
 * states is just whatever lv the browser draws.
 */
(() => {
    const POLL_MS = 2000;
    const API = "/api/v1";

    const $ = (id) => document.getElementById(id);

    // ---- token ---------------------------------------------------------

    const tokenInput = $("token");
    tokenInput.value = localStorage.getItem("sr_token") || "dev-token";

    $("save-token").addEventListener("click", () => {
        localStorage.setItem("sr_token", tokenInput.value.trim());
        tick();   // re-poll immediately with the new token
    });
    tokenInput.addEventListener("keydown", (e) => {
        if (e.key === "Enter") $("save-token").click();
    });

    const authHeaders = () => {
        const t = (tokenInput.value || "").trim();
        return t ? { "Authorization": "Token " + t } : {};
    };

    // ---- fetch helpers -------------------------------------------------

    async function getJSON(path, opts = {}) {
        const r = await fetch(API + path, {
            headers: { "Accept": "application/json", ...authHeaders(), ...(opts.headers || {}) },
            ...opts,
        });
        if (!r.ok) {
            const txt = await r.text().catch(() => "");
            throw new Error(`${r.status} ${r.statusText} ${txt}`);
        }
        return r.json();
    }

    // ---- connection indicator -----------------------------------------

    let lastOK = false;
    function setOnline(ok, meta) {
        lastOK = ok;
        $("conn-dot").classList.toggle("online", ok);
        $("server-meta").textContent = meta;
        $("last-tick").textContent = new Date().toLocaleTimeString();
    }

    // ---- render: stats -------------------------------------------------

    function renderStats({ rack, jobs, anomalies, ops }) {
        const slots = rack?.slots || [];
        const occ   = slots.filter(s => s.stock).length;
        $("s-slots").textContent = slots.length || "–";
        $("s-occ").textContent   = occ || "0";
        $("s-empty").textContent = slots.length ? (slots.length - occ) : "–";
        $("s-jobs").textContent  = jobs?.jobs?.length ?? "–";
        $("s-anom").textContent  = anomalies?.anomalies?.length ?? "–";
        $("s-ops").textContent   = ops?.count ?? "–";
    }

    // ---- render: rack --------------------------------------------------

    let selectedSlot = null;

    function renderRack(rack) {
        const root = $("rack");
        const slots = rack?.slots || [];
        if (!slots.length) { root.textContent = "no rack data"; return; }

        // group into rows of 16 (display convention; the wire shape no
        // longer carries chain/position — those are HMI-internal)
        const PER_ROW = 16;
        const byChain = new Map();
        for (const s of slots) {
            const chain = Math.floor((s.slot - 1) / PER_ROW) + 1;
            if (!byChain.has(chain)) byChain.set(chain, []);
            byChain.get(chain).push(s);
        }

        const frag = document.createDocumentFragment();
        for (const [chain, chainSlots] of [...byChain.entries()].sort((a, b) => a[0] - b[0])) {
            const row = document.createElement("div");
            row.className = "chain-row";
            const lbl = document.createElement("div");
            lbl.className = "chain-label";
            lbl.textContent = "Ch " + chain;
            row.appendChild(lbl);

            chainSlots.sort((a, b) => a.slot - b.slot);
            for (const s of chainSlots) {
                const btn = document.createElement("button");
                btn.className = "slot" + (s.stock ? " is-occ" : "")
                    + (selectedSlot === s.slot ? " selected" : "");
                btn.title = slotTitle(s);
                btn.dataset.slot = s.slot;
                btn.addEventListener("click", () => {
                    selectedSlot = s.slot;
                    renderSlotDetail(s);
                    // re-mark selected
                    for (const el of root.querySelectorAll(".slot.selected")) el.classList.remove("selected");
                    btn.classList.add("selected");
                });
                const num = document.createElement("span");
                num.className = "n";
                num.textContent = s.slot;
                btn.appendChild(num);
                row.appendChild(btn);
            }
            frag.appendChild(row);
        }
        root.replaceChildren(frag);

        // refresh detail card if a slot is selected
        if (selectedSlot != null) {
            const found = slots.find(s => s.slot === selectedSlot);
            if (found) renderSlotDetail(found);
        }
    }

    function slotTitle(s) {
        if (!s.stock) return `slot ${s.slot} · empty`;
        return `slot ${s.slot} · ${s.stock.part.id} qty ${s.stock.qty}`;
    }

    function renderSlotDetail(s) {
        const root = $("slot-detail");
        if (!s.stock) {
            root.innerHTML = `<div class="kv">
                <span>slot</span><span>${s.slot}</span>
                <span>state</span><span>EMPTY</span>
                <span>location_id</span><span>${s.location_id}</span>
            </div>`;
            return;
        }
        const p = s.stock.part;
        root.innerHTML = `<div class="kv">
            <span>slot</span><span>${s.slot}</span>
            <span>part</span><span>${esc(p.id)} – ${esc(p.name)}</span>
            <span>package / mfg</span><span>${esc(p.pkg)} · ${esc(p.mfg)}</span>
            <span>qty / batch</span><span>${s.stock.qty} · ${esc(s.stock.batch)}</span>
            <span>barcode</span><span>${esc(s.stock.barcode)}</span>
            <span>stock id</span><span>${s.stock.id}</span>
            <span>location_id</span><span>${s.location_id}</span>
        </div>`;
    }

    // ---- render: pick jobs --------------------------------------------

    function renderJobs(payload) {
        const root = $("jobs");
        const jobs = payload?.jobs || [];
        if (!jobs.length) { root.innerHTML = '<div class="muted" style="padding:12px 14px">no jobs</div>'; return; }

        const frag = document.createDocumentFragment();
        for (const j of jobs) {
            const done = j.items.filter(it => it.picked).length;
            const total = j.items.length;
            const pct = total ? Math.round(done * 100 / total) : 0;

            const div = document.createElement("div");
            div.className = "job";
            div.innerHTML = `
                <div class="job-head">
                    <div>
                        <div class="job-name">${esc(j.id)} · ${esc(j.name)}</div>
                        <div class="job-meta">requested ${esc(j.requested_at)} · ${done}/${total} items</div>
                    </div>
                    <span class="badge ${esc(j.status)}">${esc(j.status)}</span>
                </div>
                <div class="progress"><div style="width:${pct}%"></div></div>
                <div class="items">
                    ${j.items.map(it => `
                        <div class="item ${it.picked ? "done" : ""}">
                            <span>${it.picked ? "✓" : "·"} ${esc(it.part_id)} – ${esc(it.part_name)}</span>
                            <span class="qty">qty ${it.qty}</span>
                            <span class="slots">${it.located_slots.length ? "@" + it.located_slots.join(",") : "—"}</span>
                        </div>
                    `).join("")}
                </div>
            `;
            frag.appendChild(div);
        }
        root.replaceChildren(frag);
    }

    // ---- render: anomalies --------------------------------------------

    function renderAnomalies(payload) {
        const root = $("anomalies");
        const list = payload?.anomalies || [];
        if (!list.length) { root.className = "muted"; root.textContent = "none"; return; }
        root.className = "";
        const rows = list.slice().reverse().map(a => `
            <div class="row">
                <span>#${a.id}</span>
                <span class="k-${esc(a.kind)}">${esc(a.kind)}</span>
                <span>slot ${a.slot_num ?? "–"}</span>
                <span>${esc(new Date(a.at).toLocaleTimeString())}</span>
                <span>${esc(a.detail || "")}</span>
            </div>
        `).join("");
        root.innerHTML = rows;
    }

    // ---- render: provisioning QR ----------------------------------------

    $("show-provision").addEventListener("click", async () => {
        const root = $("provision");
        try {
            const p = await getJSON("/_dev/provision");
            root.innerHTML =
                (p.svg || '<div class="muted">install segno for a QR image</div>') +
                `<div class="prov-payload">${esc(p.payload)}</div>`;
        } catch (e) {
            root.innerHTML = `<div class="muted">failed: ${esc(e.message)}</div>`;
        }
    });

    // ---- tick ----------------------------------------------------------

    async function tick() {
        // /health is unauthenticated so it's the heartbeat even when the
        // token is wrong; everything else gates on auth.
        let health;
        try {
            health = await getJSON("/health");
        } catch (e) {
            setOnline(false, "offline · " + e.message);
            return;
        }

        const meta = `${health.server} v${health.version} · ${new Date(health.time).toLocaleTimeString()}`;

        // Try the gated endpoints. If auth fails we still keep the heartbeat green.
        let rack, jobs, anomalies, ops;
        let authOK = true;
        try {
            [rack, jobs, anomalies, ops] = await Promise.all([
                getJSON("/rack"),
                getJSON("/pickjobs"),
                getJSON("/_dev/anomalies"),
                getJSON("/_dev/ops"),
            ]);
        } catch (e) {
            authOK = false;
            setOnline(true, meta + " · auth: " + e.message);
        }
        if (authOK) {
            setOnline(true, meta);
            renderStats({ rack, jobs, anomalies, ops });
            renderRack(rack);
            renderJobs(jobs);
            renderAnomalies(anomalies);
        }
    }

    // ---- util ----------------------------------------------------------

    function esc(s) {
        return String(s ?? "").replace(/[&<>"']/g, (c) => ({
            "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
        }[c]));
    }

    // ---- boot ----------------------------------------------------------

    tick();
    setInterval(tick, POLL_MS);
})();
