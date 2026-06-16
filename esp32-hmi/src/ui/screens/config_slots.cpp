// CONFIG-SLOTS -- rack identity + live RS485 port topology and the
// logical slot numbering derived from the modules actually present.
//
// The Core reports modules-per-port (sensed from the rail voltage); we
// number every reel-slot contiguously, port-ascending, skipping empty
// ports (see app::SlotMap). Data comes from the background-cached reel
// info so building this screen never blocks the LVGL thread.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "app/slot_map.h"
#include "rs485/rs485.h"

#include <stdio.h>

namespace ui::screens {

void build_config_slots(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);

    // ---- Pull the live topology and compute the numbering ----------
    rs485::ReelInfo ri[rs485::N_PORTS];
    int  n_ports = 0;
    bool have    = rs485::cached_reel_info(ri, rs485::N_PORTS, &n_ports);

    uint8_t counts[app::SlotMap::N_PORTS] = { 0 };
    if (have) {
        for (int i = 0; i < n_ports; ++i)
            if (ri[i].port < app::SlotMap::N_PORTS)
                counts[ri[i].port] = ri[i].module_count;
    }
    app::SlotMap map;
    map.rebuild(counts);

    // ---- Rack identity --------------------------------------------
    {
        lv_obj_t* fc = form_card(sc, "RACK IDENTITY");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Rack name", nullptr);
        form_input(r, "Lab Rack A", 200);

        r = form_row(fc);
        form_row_label(r, "Inventree location", "Sub-locations are slot numbers");
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", app::state().inv_location_id);
        form_input(r, buf, 0, true);
    }

    // ---- RS485 ports (live) ---------------------------------------
    {
        char meta[24];
        if (have) snprintf(meta, sizeof(meta), "%d slots present", map.total_slots);
        else      snprintf(meta, sizeof(meta), "scanning...");
        lv_obj_t* fc = form_card(sc, "RS485 PORTS");

        for (int p = 0; p < app::SlotMap::N_PORTS; ++p) {
            lv_obj_t* r = form_row(fc);

            char title[16];
            snprintf(title, sizeof(title), "Port %d", p + 1);

            char desc[48];
            if (!have) {
                snprintf(desc, sizeof(desc), "scanning...");
            } else if (map.module_count[p] == 0) {
                snprintf(desc, sizeof(desc), "empty  (sense %u mV)",
                         p < n_ports ? ri[p].sense_mv : 0);
            } else {
                snprintf(desc, sizeof(desc), "%u module%s  -  %d slots",
                         map.module_count[p], map.module_count[p] == 1 ? "" : "s",
                         map.port_slots[p]);
            }
            form_row_label(r, title, desc);

            // Right column: the logical range this port occupies.
            char range[16];
            if (have && map.port_slots[p] > 0) {
                snprintf(range, sizeof(range), "%d-%d",
                         map.port_start[p],
                         map.port_start[p] + map.port_slots[p] - 1);
            } else {
                snprintf(range, sizeof(range), "-");
            }
            form_input(r, range, 0, true);
        }

        // Summary total.
        lv_obj_t* rt = form_row(fc);
        form_row_label(rt, "Total logical slots", meta);
        char tot[8];
        snprintf(tot, sizeof(tot), "%d", map.total_slots);
        form_input(rt, tot, 0, true);
    }

    // The old "NUMBERING" card (Order / Skip numbers / Save numbering) was a
    // non-functional mockup -- the fields were display-only and the Save
    // button had no handler. Numbering is derived automatically from the live
    // topology (SlotMap), so the card is removed (review item R15).
}

} // namespace ui::screens
