"""SmartReelPlugin - InvenTree integration for SmartReel hardware.

Exposes the HMI workflow API (docs/hmi-plugin-api.md in the SmartReel
repo) under /plugin/smartreel/api/v1/, plus web-UI panels:

- Build Order page: "Send to SmartReel" pick-job panel (user story 4)
- Rack StockLocation page: HMI provisioning QR + rack status
"""

from django.urls import path

from plugin import InvenTreePlugin
from plugin.mixins import SettingsMixin, UrlsMixin, UserInterfaceMixin

from . import PLUGIN_VERSION
from . import api


class SmartReelPlugin(UserInterfaceMixin, UrlsMixin, SettingsMixin, InvenTreePlugin):
    """InvenTree-side counterpart of the SmartReel HMI."""

    NAME = "SmartReelPlugin"
    SLUG = "smartreel"
    TITLE = "Smart Reel"
    DESCRIPTION = "Integration with SmartReel smart reel holders"
    VERSION = PLUGIN_VERSION
    AUTHOR = "Lukas"

    MIN_VERSION = "1.0.0"

    SETTINGS = {
        "HMI_URL": {
            "name": "HMI base URL",
            "description": "Server base URL embedded in the provisioning QR, as the "
                           "HMI must reach it — an https:// LAN IP or domain, e.g. "
                           "https://192.168.1.235 or https://inventree.example.com. "
                           "Leave empty to fall back to the InvenTree base URL / the "
                           "address the browser used.",
            "default": "",
        },
        # Each SmartReel rack is provisioned from its own stock-location page
        # (the "SmartReel HMI" panel); the rack's identity rides on the HMI
        # token, so there is no global "rack location" setting. Staging and
        # pulled below are instance-wide defaults each rack can override.
        "STAGING_LOCATION": {
            "name": "Staging location",
            "description": "Default destination for picked reels (user picks and pick "
                           "jobs). Used by every rack unless a rack overrides it.",
            "model": "stock.stocklocation",
        },
        "PULLED_LOCATION": {
            "name": "Pulled location",
            "description": "Destination for reels removed without a pick "
                           "(anomaly reconciliation), so stock is never silently lost. "
                           "Used by every rack unless a rack overrides it.",
            "model": "stock.stocklocation",
        },
    }

    def setup_urls(self):
        """Routes under /plugin/smartreel/ — no trailing slashes (see api.py)."""
        # /plugin/ paths sit behind InvenTree's AuthRequiredMiddleware, which
        # 401s anonymous requests unless the view is marked auth_exempt. The
        # health probe is the one endpoint that must work unauthenticated.
        health = api.HealthView.as_view()
        health.auth_exempt = True
        return [
            path("api/v1/health", health, name="health"),
            path("api/v1/rack", api.RackView.as_view(), name="rack"),
            path("api/v1/rack/register", api.RegisterView.as_view(), name="register"),
            path("api/v1/rack/slots/<int:slot_num>/assign", api.AssignView.as_view(), name="assign"),
            path("api/v1/rack/slots/<int:slot_num>/pick", api.PickView.as_view(), name="pick"),
            path("api/v1/rack/slots/<int:slot_num>/clear", api.ClearView.as_view(), name="clear"),
            path("api/v1/barcode/resolve", api.ResolveView.as_view(), name="resolve"),
            path("api/v1/pickjobs", api.PickJobsView.as_view(), name="pickjobs"),
            path("api/v1/racks", api.RacksView.as_view(), name="racks"),
            path("api/v1/pickjobs/from-build", api.JobView.as_view(), name="job-from-build"),
            path("api/v1/pickjobs/<str:job_id>", api.JobView.as_view(), name="job"),
            path("api/v1/pickjobs/<str:job_id>/items/<int:idx>/pick", api.JobItemPickView.as_view(), name="job-item-pick"),
            path("api/v1/parts/locate", api.LocateView.as_view(), name="locate"),
            path("api/v1/anomaly", api.AnomalyView.as_view(), name="anomaly"),
            path("api/v1/provision", api.ProvisionView.as_view(), name="provision"),
        ]

    def get_ui_panels(self, request, context: dict, **kwargs):
        """Build Order pick-job panel + rack-location provisioning panel."""
        panels = []
        target_model = context.get("target_model")
        try:
            # context comes from query params, so the id arrives as a string
            target_id = int(context.get("target_id") or 0)
        except (TypeError, ValueError):
            target_id = 0

        if target_model == "build" and target_id:
            panels.append({
                "key": "smartreel-pickjob",
                "title": "SmartReel",
                "description": "Send this build's BOM to the SmartReel rack as a pick job",
                "icon": "ti:list-check:outline",
                "source": self.plugin_static_file("panel.js:renderBuildPanel"),
                "context": {"build_id": target_id},
            })

        # Provisioning panel on every stock-location page: any location can
        # be designated a SmartReel rack and provisioned (multi-unit). The
        # panel itself shows current status + a provision/rotate button.
        if target_model == "stocklocation" and target_id:
            panels.append({
                "key": "smartreel-provision",
                "title": "SmartReel HMI",
                "description": "Provision a SmartReel HMI for this location (setup QR)",
                "icon": "ti:qrcode:outline",
                "source": self.plugin_static_file("panel.js:renderProvisionPanel"),
                "context": {"location_id": target_id},
            })

        return panels
