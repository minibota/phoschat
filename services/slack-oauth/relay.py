"""Stateless PhosChat Slack OAuth callback and refresh relay."""

import base64
import json
import os
import re
import ssl
from http.client import HTTPException
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode
from urllib.request import (
    HTTPRedirectHandler,
    HTTPSHandler,
    ProxyHandler,
    Request,
    build_opener,
)

CALLBACK_PATH = "/phoschat/slack/callback"
REFRESH_PATH = "/phoschat/slack/refresh"
LOOPBACK_CALLBACK = "http://localhost:8765" + CALLBACK_PATH
SLACK_TOKEN_URL = "https://slack.com/api/oauth.v2.access"
MAX_INPUT = 8192
MAX_SLACK_RESPONSE = 65536
CLIENT_ID = re.compile(r"[0-9]{1,32}\.[0-9]{1,32}")
STATE = re.compile(r"[A-Za-z0-9_-]{43}")
CODE = re.compile(r"[A-Za-z0-9._~-]{1,1024}")
REFRESH_TOKEN = re.compile(r"xoxe-[A-Za-z0-9._-]{2,4091}")
USER_TOKEN = re.compile(r"(?:xoxe\.)?xoxp-[A-Za-z0-9._-]{2,4086}")
SCOPE = re.compile(r"[A-Za-z0-9:.,_ -]{0,4096}")
WORKSPACE_ID = re.compile(r"T[A-Z0-9]{1,31}")
USER_ID = re.compile(r"[UW][A-Z0-9]{1,31}")
TRANSIENT_ERRORS = {"internal_error", "fatal_error", "service_unavailable", "request_timeout"}
HEADERS = [
    ("Cache-Control", "no-store"),
    ("Pragma", "no-cache"),
    ("Referrer-Policy", "no-referrer"),
    ("X-Content-Type-Options", "nosniff"),
]


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


def valid_state(state):
    if not STATE.fullmatch(state):
        return False
    data = base64.urlsafe_b64decode(state + "=")
    return len(data) == 32 and base64.urlsafe_b64encode(data).decode("ascii")[:-1] == state


def one(fields, name):
    values = fields.get(name, [])
    return values[0] if len(values) == 1 else ""


def parse_fields(encoded, max_fields):
    return parse_qs(
        encoded, keep_blank_values=True, strict_parsing=True, max_num_fields=max_fields, errors="strict"
    )


def respond(start_response, status, body=b"", headers=()):
    start_response(status, HEADERS + [("Content-Length", str(len(body))), *headers])
    return [body]


def oauth_json(start_response, status, payload, headers=()):
    return respond(
        start_response,
        status,
        json.dumps(payload, separators=(",", ":")).encode("utf-8"),
        [("Content-Type", "application/json"), *headers],
    )


def failure(start_response, status, error, headers=()):
    return oauth_json(start_response, status, {"ok": False, "error": error}, headers)


def retry_later(start_response, value):
    delay = value if re.fullmatch(r"[1-9][0-9]{0,4}", value) and int(value) <= 86400 else "60"
    return failure(start_response, "429 Too Many Requests", "ratelimited", [("Retry-After", delay)])


def callback(environ, start_response):
    query = environ.get("QUERY_STRING", "")
    if len(query) > MAX_INPUT:
        return failure(start_response, "400 Bad Request", "invalid_callback")
    try:
        fields = parse_fields(query, 12)
    except ValueError:
        return failure(start_response, "400 Bad Request", "invalid_callback")

    state = one(fields, "state")
    code = one(fields, "code")
    error = one(fields, "error")
    if not valid_state(state):
        return failure(start_response, "400 Bad Request", "invalid_callback")
    if "error" not in fields and CODE.fullmatch(code):
        data = {"state": state, "code": code}
    elif "code" not in fields and error == "access_denied":
        data = {"state": state, "error": error}
    else:
        return failure(start_response, "400 Bad Request", "invalid_callback")

    # The desktop checks state against its pending session and holds the PKCE verifier.
    location = LOOPBACK_CALLBACK + "?" + urlencode(data)
    return respond(
        start_response, "303 See Other", headers=[("Content-Type", "text/plain"), ("Location", location)]
    )


def token_payload(payload):
    """Copy only supported user-credential fields into a successful refresh response."""
    access = payload.get("access_token")
    refresh = payload.get("refresh_token")
    expires = payload.get("expires_in")
    token_type = payload.get("token_type", "user")
    if (
        not isinstance(access, str)
        or not USER_TOKEN.fullmatch(access)
        or not isinstance(refresh, str)
        or not REFRESH_TOKEN.fullmatch(refresh)
        or type(expires) is not int
        or not 0 < expires <= 86400
        or token_type != "user"
    ):
        raise ValueError("Invalid Slack user grant")
    result = {
        "ok": True,
        "access_token": access,
        "refresh_token": refresh,
        "expires_in": expires,
        "token_type": "user",
    }
    if "scope" in payload:
        scope = payload["scope"]
        if not isinstance(scope, str) or not SCOPE.fullmatch(scope):
            raise ValueError("Invalid Slack scopes")
        result["scope"] = scope
    if "user_id" in payload or "id" in payload:
        user = payload.get("user_id", payload.get("id"))
        if not isinstance(user, str) or not USER_ID.fullmatch(user):
            raise ValueError("Invalid Slack user identity")
        if "id" in payload and payload["id"] != user:
            raise ValueError("Inconsistent Slack user identity")
        result["user_id"] = user
    team = payload.get("team")
    if team is not None:
        workspace = team.get("id") if isinstance(team, dict) else None
        if not isinstance(workspace, str) or not WORKSPACE_ID.fullmatch(workspace):
            raise ValueError("Invalid Slack workspace identity")
        result["team"] = {"id": workspace}
    return result


class OAuthRelay:
    def __init__(self, client_id, client_secret, opener=None):
        if not CLIENT_ID.fullmatch(client_id):
            raise ValueError("Set SLACK_CLIENT_ID to the app's numeric client ID")
        if not re.fullmatch(r"[!-~]{1,512}", client_secret):
            raise ValueError("Set SLACK_CLIENT_SECRET in the server's secret store")
        authorization = f"{client_id}:{client_secret}".encode("ascii")
        self.client_id = client_id
        self.authorization = "Basic " + base64.b64encode(authorization).decode("ascii")
        self.opener = opener or build_opener(
            ProxyHandler({}), NoRedirect(), HTTPSHandler(context=ssl.create_default_context())
        )

    def __call__(self, environ, start_response):
        path = environ.get("PATH_INFO")
        method = environ.get("REQUEST_METHOD")
        if path == CALLBACK_PATH and method == "GET":
            return callback(environ, start_response)
        if path == REFRESH_PATH and method == "POST":
            return self.refresh(environ, start_response)
        if path == "/healthz" and method == "GET":
            return respond(start_response, "200 OK", b"ok\n", [("Content-Type", "text/plain")])
        if path in {CALLBACK_PATH, REFRESH_PATH, "/healthz"}:
            allowed = "POST" if path == REFRESH_PATH else "GET"
            return failure(start_response, "405 Method Not Allowed", "method_not_allowed", [("Allow", allowed)])
        return failure(start_response, "404 Not Found", "not_found")

    def refresh(self, environ, start_response):
        content_type = environ.get("CONTENT_TYPE", "").partition(";")[0].strip().lower()
        if content_type != "application/x-www-form-urlencoded":
            return failure(start_response, "415 Unsupported Media Type", "invalid_request")
        length = environ.get("CONTENT_LENGTH", "")
        if not re.fullmatch(r"[0-9]{1,5}", length) or environ.get("QUERY_STRING"):
            return failure(start_response, "400 Bad Request", "invalid_request")
        size = int(length)
        if size > MAX_INPUT:
            return failure(start_response, "413 Payload Too Large", "invalid_request")
        try:
            body = environ["wsgi.input"].read(size)
            if len(body) != size:
                raise ValueError("Incomplete request")
            fields = parse_fields(body.decode("ascii"), 3)
        except (ValueError, OSError):
            return failure(start_response, "400 Bad Request", "invalid_request")

        token = one(fields, "refresh_token")
        if (
            set(fields) != {"client_id", "grant_type", "refresh_token"}
            or one(fields, "client_id") != self.client_id
            or one(fields, "grant_type") != "refresh_token"
            or not REFRESH_TOKEN.fullmatch(token)
        ):
            return failure(start_response, "400 Bad Request", "invalid_request")

        upstream = Request(
            SLACK_TOKEN_URL,
            data=urlencode({"grant_type": "refresh_token", "refresh_token": token}).encode("ascii"),
            headers={
                "Accept": "application/json",
                "Authorization": self.authorization,
                "Content-Type": "application/x-www-form-urlencoded",
            },
            method="POST",
        )
        try:
            with self.opener.open(upstream, timeout=10) as response:
                retry_after = response.headers.get("Retry-After", "")
                raw = response.read(MAX_SLACK_RESPONSE + 1)
        except HTTPError as exc:
            with exc:
                if exc.code == 429:
                    return retry_later(start_response, exc.headers.get("Retry-After", ""))
                if exc.code in {400, 403}:
                    return failure(start_response, "400 Bad Request", "invalid_grant")
                return failure(start_response, "503 Service Unavailable", "slack_unavailable")
        except (URLError, OSError, HTTPException):
            return failure(start_response, "503 Service Unavailable", "slack_unavailable")

        try:
            if len(raw) > MAX_SLACK_RESPONSE:
                raise ValueError("Oversized Slack response")
            payload = json.loads(raw)
            if not isinstance(payload, dict):
                raise ValueError("Invalid Slack response")
            if payload.get("ok") is False:
                error = payload.get("error")
                if error == "ratelimited":
                    return retry_later(start_response, retry_after)
                if isinstance(error, str) and error in TRANSIENT_ERRORS:
                    return failure(start_response, "503 Service Unavailable", "slack_unavailable")
                return failure(start_response, "400 Bad Request", "invalid_grant")
            if payload.get("ok") is not True:
                raise ValueError("Invalid Slack response")
            grant = token_payload(payload)
        except (ValueError, RecursionError):
            return failure(start_response, "502 Bad Gateway", "invalid_slack_response")
        return oauth_json(start_response, "200 OK", grant)


def create_application():
    return OAuthRelay(os.environ.get("SLACK_CLIENT_ID", ""), os.environ.get("SLACK_CLIENT_SECRET", ""))
