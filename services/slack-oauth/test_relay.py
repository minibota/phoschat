import base64
import io
import json
import threading
import unittest
from email.message import Message
from http.client import IncompleteRead
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlsplit
from wsgiref.util import setup_testing_defaults
from wsgiref.validate import validator

import relay

CLIENT_ID = "1234.5678"
SECRET = "fixture-secret"
STATE = base64.urlsafe_b64encode(bytes(range(32))).decode("ascii").rstrip("=")
REFRESH_BODY = urlencode(
    {"client_id": CLIENT_ID, "grant_type": "refresh_token", "refresh_token": "xoxe-fixture-old"}
)


class StubSlack:
    def __init__(self, response, retry_after=None):
        self.response = response
        self.headers = Message()
        if retry_after is not None:
            self.headers["Retry-After"] = retry_after
        self.requests = []

    def open(self, request, timeout):
        self.requests.append((request, timeout))
        if isinstance(self.response, Exception):
            raise self.response
        body = io.BytesIO(json.dumps(self.response).encode("utf-8"))
        body.headers = self.headers
        return body


def request(app, path, method="GET", query="", body="", content_type="application/x-www-form-urlencoded"):
    environ = {}
    setup_testing_defaults(environ)
    data = body.encode("ascii")
    environ.update(
        {
            "PATH_INFO": path,
            "REQUEST_METHOD": method,
            "QUERY_STRING": query,
            "CONTENT_TYPE": content_type,
            "CONTENT_LENGTH": str(len(data)),
            "wsgi.input": io.BytesIO(data),
        }
    )
    result = {}

    def start_response(status, headers):
        result.update(status=int(status.split()[0]), headers=dict(headers))

    response = validator(app)(environ, start_response)
    try:
        result["body"] = b"".join(response)
    finally:
        response.close()
    return result


class OAuthRelayTest(unittest.TestCase):
    def test_callback_forwards_only_code_and_state_to_fixed_loopback(self):
        upstream = StubSlack({})
        app = relay.OAuthRelay(CLIENT_ID, SECRET, upstream)
        result = request(
            app,
            relay.CALLBACK_PATH,
            query=urlencode(
                {
                    "state": STATE,
                    "code": "grant.code-123",
                    "access_token": "should-not-appear",
                    "redirect_uri": "https://unrelated.example/callback",
                }
            ),
        )
        self.assertEqual(result["status"], 303)
        location = urlsplit(result["headers"]["Location"])
        self.assertEqual((location.scheme, location.netloc), ("http", "localhost:8765"))
        self.assertEqual(location.path, relay.CALLBACK_PATH)
        self.assertEqual(parse_qs(location.query), {"state": [STATE], "code": ["grant.code-123"]})
        self.assertEqual(result["headers"]["Cache-Control"], "no-store")
        self.assertEqual(result["headers"]["Referrer-Policy"], "no-referrer")
        self.assertEqual(upstream.requests, [])

    def test_callback_handles_cancel_and_rejects_ambiguous_inputs(self):
        app = relay.OAuthRelay(CLIENT_ID, SECRET, StubSlack({}))
        cancel = request(app, relay.CALLBACK_PATH, query=f"state={STATE}&error=access_denied")
        self.assertEqual(cancel["status"], 303)
        self.assertEqual(
            parse_qs(urlsplit(cancel["headers"]["Location"]).query),
            {"state": [STATE], "error": ["access_denied"]},
        )
        for query in (
            f"state={STATE}&code=one&code=two",
            f"state={STATE}&state={STATE}&code=one",
            f"state={STATE}&code=one&error=access_denied",
            f"state={STATE}&error=unexpected",
            f"state={STATE}&code=bad%0D%0ALocation",
            f"state={STATE[:-1]}B&code=bad",
            "state=short&code=bad",
        ):
            with self.subTest(query=query):
                result = request(app, relay.CALLBACK_PATH, query=query)
                self.assertEqual(result["status"], 400)
                self.assertNotIn("Location", result["headers"])

    def test_refresh_authenticates_to_slack_and_returns_only_user_grant(self):
        grant = {
            "ok": True,
            "access_token": "xoxe.xoxp-fixture-access",
            "refresh_token": "xoxe-fixture-new",
            "expires_in": 43200,
            "token_type": "user",
            "scope": "chat:write,files:write",
            "id": "U1234",
            "team": {"id": "T9876", "name": "private name"},
        }
        upstream = StubSlack({**grant, "private_data": "should-not-appear"})
        app = relay.OAuthRelay(CLIENT_ID, SECRET, upstream)
        result = request(app, relay.REFRESH_PATH, method="POST", body=REFRESH_BODY)
        self.assertEqual(result["status"], 200)
        expected = {**grant, "user_id": "U1234", "team": {"id": "T9876"}}
        del expected["id"]
        self.assertEqual(json.loads(result["body"]), expected)
        self.assertEqual(result["headers"]["Cache-Control"], "no-store")
        self.assertEqual(len(upstream.requests), 1)
        sent, timeout = upstream.requests[0]
        self.assertEqual(sent.full_url, "https://slack.com/api/oauth.v2.access")
        self.assertEqual(sent.method, "POST")
        basic = base64.b64encode(f"{CLIENT_ID}:{SECRET}".encode("ascii")).decode("ascii")
        self.assertEqual(sent.get_header("Authorization"), "Basic " + basic)
        self.assertEqual(
            parse_qs(sent.data.decode("ascii")),
            {"grant_type": ["refresh_token"], "refresh_token": ["xoxe-fixture-old"]},
        )
        self.assertEqual(timeout, 10)

    def test_refresh_rejects_wrong_app_grant_and_unexpected_parameters(self):
        upstream = StubSlack({})
        app = relay.OAuthRelay(CLIENT_ID, SECRET, upstream)
        for body in (
            REFRESH_BODY.replace(CLIENT_ID, "4321.8765"),
            REFRESH_BODY.replace("grant_type=refresh_token", "grant_type=authorization_code"),
            REFRESH_BODY + "&client_secret=attacker-controlled",
            REFRESH_BODY.replace("xoxe-fixture-old", "xoxb-bot-token"),
            REFRESH_BODY + "&refresh_token=xoxe-duplicate",
        ):
            result = request(app, relay.REFRESH_PATH, method="POST", body=body)
            self.assertEqual(result["status"], 400)
        result = request(app, relay.REFRESH_PATH, method="POST", body="x" * (relay.MAX_INPUT + 1))
        self.assertEqual(result["status"], 413)
        result = request(app, relay.REFRESH_PATH, method="POST", body=REFRESH_BODY, query="extra=1")
        self.assertEqual(result["status"], 400)
        self.assertEqual(upstream.requests, [])

    def test_refresh_sanitizes_failures_and_rate_limit(self):
        rate_headers = Message()
        rate_headers["Retry-After"] = "12"
        for response, status, error in (
            (HTTPError(relay.SLACK_TOKEN_URL, 429, "fixture", rate_headers, io.BytesIO()), 429, "ratelimited"),
            (URLError("sensitive upstream details"), 503, "slack_unavailable"),
            (IncompleteRead(b"sensitive partial response"), 503, "slack_unavailable"),
            ({"ok": False, "error": "secret upstream message"}, 400, "invalid_grant"),
            ({"ok": False, "error": "internal_error"}, 503, "slack_unavailable"),
            ({"ok": True, "access_token": "xoxb-bot-token"}, 502, "invalid_slack_response"),
        ):
            app = relay.OAuthRelay(CLIENT_ID, SECRET, StubSlack(response))
            result = request(app, relay.REFRESH_PATH, method="POST", body=REFRESH_BODY)
            self.assertEqual(result["status"], status)
            self.assertEqual(json.loads(result["body"]), {"ok": False, "error": error})
            if status == 429:
                self.assertEqual(result["headers"]["Retry-After"], "12")

    def test_refresh_does_not_follow_upstream_redirect(self):
        received = []

        class RedirectingSlack(BaseHTTPRequestHandler):
            def do_POST(self):
                received.append(self.path)
                self.send_response(302)
                self.send_header("Location", "/untrusted")
                self.end_headers()

            def do_GET(self):
                received.append(self.path)
                self.send_response(500)
                self.end_headers()

            def log_message(self, *_):
                pass

        with ThreadingHTTPServer(("127.0.0.1", 0), RedirectingSlack) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                endpoint = f"http://127.0.0.1:{server.server_port}/token"
                with patch.object(relay, "SLACK_TOKEN_URL", endpoint):
                    app = relay.OAuthRelay(CLIENT_ID, SECRET)
                    result = request(app, relay.REFRESH_PATH, method="POST", body=REFRESH_BODY)
            finally:
                server.shutdown()
                worker.join()
        self.assertEqual(received, ["/token"])
        self.assertEqual(result["status"], 503)
        self.assertNotIn("Location", result["headers"])


if __name__ == "__main__":
    unittest.main()
