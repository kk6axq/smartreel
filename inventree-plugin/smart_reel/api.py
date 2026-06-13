"""HTTP layer for the SmartReel plugin API.

Thin DRF views over services.py. Contract: docs/hmi-plugin-api.md in the
SmartReel repo — the mock server (mock-inventree/) implements the same
shapes. The HMI authenticates with `Authorization: Token <ApiToken>`;
the Build Order panel uses the browser session.

Paths are registered WITHOUT trailing slashes because the HMI builds
`<base>/api/v1/<path>` literally and Django's APPEND_SLASH redirect
would turn POSTs into GETs.
"""
from __future__ import annotations

import datetime

from django.utils import timezone

from rest_framework import permissions, status
from rest_framework.authentication import SessionAuthentication
from rest_framework.response import Response
from rest_framework.views import APIView

from users.authentication import ApiTokenAuthentication

from . import PLUGIN_VERSION, services


class SmartReelAPIView(APIView):
    """Base: InvenTree token auth (HMI) or session auth (web panel)."""

    authentication_classes = [ApiTokenAuthentication, SessionAuthentication]
    permission_classes = [permissions.IsAuthenticated]

    def handle_exception(self, exc):
        """Map service-layer exceptions onto contract error responses."""
        if isinstance(exc, LookupError):
            return Response({"detail": str(exc)}, status=status.HTTP_404_NOT_FOUND)
        if isinstance(exc, services.SlotConflict):
            return Response({"detail": str(exc)}, status=status.HTTP_409_CONFLICT)
        if isinstance(exc, services.ConfigError):
            return Response({"detail": str(exc)}, status=status.HTTP_409_CONFLICT)
        if isinstance(exc, ValueError):
            return Response({"detail": str(exc)}, status=status.HTTP_400_BAD_REQUEST)
        return super().handle_exception(exc)

    # -- shared helpers ----------------------------------------------------

    def idempotent(self, request, fn):
        """Replay a cached response for a repeated op_id, else run fn()."""
        op_id = (request.data or {}).get("op_id")
        cached = services.op_lookup(op_id)
        if cached is not None:
            return Response(cached)
        resp = fn()
        services.op_remember(op_id, resp)
        return Response(resp)

    def require_int(self, request, field) -> int:
        val = (request.data or {}).get(field)
        if not isinstance(val, int):
            raise ValueError(f"missing/invalid '{field}'")
        return val


class HealthView(APIView):
    """GET /health — unauthenticated liveness probe."""

    authentication_classes: list = []
    permission_classes = [permissions.AllowAny]

    def get(self, request):
        return Response({
            "ok": True,
            "server": "smartreel-plugin",
            "version": PLUGIN_VERSION,
            "time": timezone.now().isoformat(timespec="seconds"),
        })


class RackView(SmartReelAPIView):
    """GET /rack — full snapshot (boot sync + reconciliation)."""

    def get(self, request):
        return Response(services.rack_snapshot())


class RegisterView(SmartReelAPIView):
    """POST /rack/register — idempotent slot sub-location creation."""

    def post(self, request):
        n = self.require_int(request, "n_slots")
        if n < 1 or n > 512:
            raise ValueError(f"implausible n_slots {n}")

        def run():
            slots = services.ensure_slots(n, request.user)
            return {
                "location_id": services.require(
                    services.rack_location(), "RACK_LOCATION").pk,
                "slot_locations": [
                    {"slot": k, "location_id": v.pk} for k, v in sorted(slots.items())
                ],
            }

        return self.idempotent(request, run)


class ResolveView(SmartReelAPIView):
    """POST /barcode/resolve — what did the HMI just scan?"""

    def post(self, request):
        code = (request.data or {}).get("code")
        if not code or not isinstance(code, str):
            raise ValueError("missing 'code'")
        # Resolution is read-only; no idempotency cache needed.
        return Response(services.resolve_barcode(code))


class AssignView(SmartReelAPIView):
    """POST /rack/slots/{n}/assign — load a reel into a slot."""

    def post(self, request, slot_num: int):
        stock_item_id = self.require_int(request, "stock_item_id")
        return self.idempotent(
            request,
            lambda: services.assign_slot(slot_num, stock_item_id, request.user),
        )


class PickView(SmartReelAPIView):
    """POST /rack/slots/{n}/pick — whole-reel transfer to staging."""

    def post(self, request, slot_num: int):
        dest = (request.data or {}).get("destination_id")
        if dest is not None and not isinstance(dest, int):
            raise ValueError("invalid 'destination_id'")
        return self.idempotent(
            request,
            lambda: services.pick_slot(slot_num, request.user, destination_id=dest),
        )


class ClearView(SmartReelAPIView):
    """POST /rack/slots/{n}/clear — anomaly reconcile to the pulled bin."""

    def post(self, request, slot_num: int):
        reason = (request.data or {}).get("reason")
        return self.idempotent(
            request,
            lambda: services.clear_slot(slot_num, request.user, reason=reason),
        )


class PickJobsView(SmartReelAPIView):
    """GET /pickjobs — jobs sourced from build-order metadata."""

    def get(self, request):
        return Response({"jobs": services.list_jobs()})


class JobItemPickView(SmartReelAPIView):
    """POST /pickjobs/{id}/items/{idx}/pick — pick one reel for a job item."""

    def post(self, request, job_id: str, idx: int):
        slot_num = self.require_int(request, "slot_num")
        return self.idempotent(
            request,
            lambda: services.pick_job_item(job_id, idx, slot_num, request.user),
        )


class JobView(SmartReelAPIView):
    """POST /pickjobs/from-build + DELETE /pickjobs/{id} (web panel)."""

    def post(self, request):
        from build.models import Build

        build_id = self.require_int(request, "build_id")
        build = Build.objects.filter(pk=build_id).first()
        if build is None:
            raise LookupError(f"build {build_id} not found")
        dest = (request.data or {}).get("destination_id")
        if dest is not None and not isinstance(dest, int):
            raise ValueError("invalid 'destination_id'")
        return self.idempotent(
            request,
            lambda: services.create_job_from_build(build, request.user, dest),
        )

    def delete(self, request, job_id: str):
        if not services.delete_job(job_id):
            raise LookupError(f"job {job_id} not found")
        return Response({"deleted": job_id})


class LocateView(SmartReelAPIView):
    """GET /parts/locate?part_id=… — slots currently holding a part."""

    def get(self, request):
        part_id = request.query_params.get("part_id")
        if not part_id:
            raise ValueError("missing 'part_id'")
        return Response(services.locate_part(part_id))


class AnomalyView(SmartReelAPIView):
    """POST /anomaly — HMI-detected unscanned activity."""

    KINDS = ("removed", "added", "divider")

    def post(self, request):
        kind = (request.data or {}).get("kind")
        if kind not in self.KINDS:
            raise ValueError(f"kind must be one of {self.KINDS}")
        slot_num = (request.data or {}).get("slot_num")
        detail = str((request.data or {}).get("detail") or "")
        return self.idempotent(
            request,
            lambda: services.log_anomaly(kind, slot_num, detail),
        )


class ProvisionView(SmartReelAPIView):
    """GET /provision — SRPROV1 payload + QR for the web panel.

    Issues (or rotates, with ?rotate=1) a dedicated InvenTree ApiToken
    named 'smartreel-hmi' for the requesting user, so HMI access can be
    revoked independently of any personal token. Staff only; meant to be
    called from the browser session, never from the HMI.
    """

    TOKEN_NAME = "smartreel-hmi"

    def get(self, request):
        import json

        from users.models import ApiToken

        if not request.user.is_staff:
            return Response(
                {"detail": "staff access required"},
                status=status.HTTP_403_FORBIDDEN,
            )

        rotate = request.query_params.get("rotate") in ("1", "true")
        qs = ApiToken.objects.filter(user=request.user, name=self.TOKEN_NAME)
        token = qs.first()
        if rotate and token is not None:
            qs.delete()
            token = None
        if token is None:
            token = ApiToken.objects.create(
                user=request.user,
                name=self.TOKEN_NAME,
                expiry=timezone.now().date() + datetime.timedelta(days=3650),
            )

        base = _public_base_url(request)
        payload = "SRPROV1:" + json.dumps(
            {"u": f"{base}/plugin/smartreel", "t": token.key},
            separators=(",", ":"),
        )
        return Response({
            "payload": payload,
            "svg": _qr_svg(payload),
            "token_name": self.TOKEN_NAME,
            "base_url": base,
        })


def _public_base_url(request) -> str:
    """Server base URL as the HMI should see it.

    Precedence: the plugin's HMI_URL setting (explicit, survives whatever
    hostname the browser used to view the QR) → InvenTree's global base
    URL → the host of this request.
    """
    override = services.get_plugin().get_setting("HMI_URL")
    if override:
        return str(override).rstrip("/")
    try:
        from common.settings import get_global_setting

        base = get_global_setting("INVENTREE_BASE_URL")
        if base:
            return str(base).rstrip("/")
    except Exception:
        pass
    return request.build_absolute_uri("/").rstrip("/")


def _qr_svg(payload: str) -> str | None:
    """Render the payload as an SVG QR, or None if no QR lib available."""
    try:
        import io

        import qrcode
        import qrcode.image.svg

        img = qrcode.make(payload, image_factory=qrcode.image.svg.SvgPathImage)
        buf = io.BytesIO()
        img.save(buf)
        return buf.getvalue().decode("utf-8")
    except Exception:
        return None
