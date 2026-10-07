#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Small authenticated, CSRF-protected administration site for one SQLite service."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import email.parser
import email.policy
import hashlib
import hmac
import html
import http.cookies
import http.server
import ipaddress
import json
import os
import pathlib
import secrets
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.parse


MAX_BODY = 17 * 1024 * 1024
SESSION_SECONDS = 3600
MAX_SESSIONS = 64
LOGIN_ATTEMPTS = 10
LOGIN_WINDOW_SECONDS = 60
MAX_LOGIN_SOURCES = 256
MAX_REQUEST_THREADS = 16
COOKIE = "cna_admin_session"
STYLE = """
:root{font:16px/1.5 system-ui;color:#17202a;background:#f5f7f9}body{margin:0}header{background:#172b4d;color:white;padding:1rem 2rem}nav{display:flex;flex-wrap:wrap;gap:.8rem}nav a{color:#dce8ff}nav form{display:inline}main{max-width:1200px;margin:auto;padding:1.5rem}section{background:white;padding:1rem 1.25rem;margin:0 0 1rem;border-radius:.35rem;box-shadow:0 1px 4px #ccd}table{border-collapse:collapse;width:100%}th,td{border-bottom:1px solid #dde3e8;text-align:left;padding:.4rem;vertical-align:top}form{display:grid;grid-template-columns:repeat(auto-fit,minmax(12rem,1fr));gap:.6rem;align-items:end}label{display:grid;gap:.2rem}input,select,textarea,button{font:inherit;padding:.42rem}button{cursor:pointer}.danger{border-left:5px solid #b42318}.notice{background:#e8f5e9;padding:.7rem}.error{background:#ffebe9;padding:.7rem}code{overflow-wrap:anywhere}.muted{color:#5d6975}h1,h2{line-height:1.2}
"""


def esc(value):
    return html.escape(str(value), quote=True)


def table(headers, rows):
    head = "".join(f"<th>{esc(value)}</th>" for value in headers)
    body = "".join("<tr>" + "".join(f"<td>{esc(value)}</td>" for value in row) + "</tr>" for row in rows)
    return f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def field(name, label, kind="text", value="", required=True):
    required_text = " required" if required else ""
    return f'<label>{esc(label)}<input type="{esc(kind)}" name="{esc(name)}" value="{esc(value)}"{required_text}></label>'


def form(action, csrf, fields, button="Apply", danger=False, multipart=False):
    encoding = ' enctype="multipart/form-data"' if multipart else ""
    css = ' class="danger"' if danger else ""
    return (f'<form method="post" action="/action/{esc(action)}"{encoding}{css}>'
            f'<input type="hidden" name="csrf" value="{esc(csrf)}">{fields}'
            f'<button type="submit">{esc(button)}</button></form>')


class AdminServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = MAX_REQUEST_THREADS

    def __init__(self, address, handler, options, password_digest):
        super().__init__(address, handler)
        self.options = options
        self.password_digest = password_digest
        self.sessions = {}
        self.sessions_lock = threading.Lock()
        self.login_failures = collections.OrderedDict()
        self.login_lock = threading.Lock()
        self.request_slots = threading.BoundedSemaphore(MAX_REQUEST_THREADS)
        self.temporary = tempfile.TemporaryDirectory(prefix="cna-admin-web-")

    def close(self):
        self.temporary.cleanup();self.server_close()

    def new_session(self):
        with self.sessions_lock:
            now = time.monotonic()
            self.sessions = {key: value for key, value in self.sessions.items() if value["expires"] > now}
            while len(self.sessions) >= MAX_SESSIONS:
                del self.sessions[min(self.sessions, key=lambda key: self.sessions[key]["expires"])]
            token = secrets.token_urlsafe(32)
            self.sessions[token] = {"csrf": secrets.token_urlsafe(32), "expires": now + SESSION_SECONDS}
            return token, self.sessions[token]

    def session(self, token):
        with self.sessions_lock:
            value = self.sessions.get(token)
            if value is None or value["expires"] <= time.monotonic():
                self.sessions.pop(token, None);return None
            value["expires"] = time.monotonic() + SESSION_SECONDS
            return dict(value)

    def remove_session(self, token):
        with self.sessions_lock:
            self.sessions.pop(token, None)

    def login_allowed(self, source):
        now = time.monotonic()
        with self.login_lock:
            expired = [key for key, value in self.login_failures.items()
                       if now - value[0] >= LOGIN_WINDOW_SECONDS]
            for key in expired:self.login_failures.pop(key, None)
            value = self.login_failures.get(source)
            return value is None or value[1] < LOGIN_ATTEMPTS

    def record_login(self, source, succeeded):
        with self.login_lock:
            if succeeded:
                self.login_failures.pop(source, None)
                return
            now = time.monotonic()
            started, count = self.login_failures.get(source, (now, 0))
            if now - started >= LOGIN_WINDOW_SECONDS:started, count = now, 0
            self.login_failures[source] = (started, count + 1)
            self.login_failures.move_to_end(source)
            while len(self.login_failures) > MAX_LOGIN_SOURCES:self.login_failures.popitem(last=False)

    def process_request(self, request, client_address):
        if not self.request_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.request_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.request_slots.release()


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "CNAAdminWeb/1"
    sys_version = ""

    def log_message(self, _format, *_arguments):
        return

    def audit(self, event, outcome="ok"):
        print(json.dumps({"timestamp": dt.datetime.now(dt.timezone.utc).isoformat(), "event": event,
                          "outcome": outcome}, separators=(",", ":")), flush=True)

    def security_headers(self, content_type="text/html; charset=utf-8", length=None):
        self.send_header("Content-Type", content_type)
        if length is not None:self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()")
        if self.server.options.tls_cert:
            self.send_header("Strict-Transport-Security", "max-age=31536000")
        self.send_header("Content-Security-Policy", "default-src 'self'; form-action 'self'; frame-ancestors 'none'")

    def send_bytes(self, status, content, content_type="text/html; charset=utf-8", extra=None):
        data = content if isinstance(content, bytes) else content.encode()
        self.send_response(status)
        if extra:
            for name, value in extra:self.send_header(name, value)
        self.security_headers(content_type, len(data));self.end_headers();self.wfile.write(data)

    def redirect(self, location, extra=None):
        headers = [("Location", location)] + (extra or [])
        self.send_response(303)
        for name, value in headers:self.send_header(name, value)
        self.security_headers(length=0);self.end_headers()

    def cookie_token(self):
        jar = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
        return jar[COOKIE].value if COOKIE in jar else ""

    def authenticated(self):
        token = self.cookie_token()
        if not token:
            return "", None
        return token, self.server.session(token)

    def query(self, sql, parameters=()):
        uri = "file:" + urllib.parse.quote(str(self.server.options.database)) + "?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=5) as connection:
            connection.execute("PRAGMA query_only=ON")
            return connection.execute(sql, parameters).fetchall()

    def page(self, title, body, session, notice=""):
        nav = " ".join(f'<a href="{path}">{label}</a>' for path, label in (
            ("/", "Dashboard"), ("/titles", "Titles"), ("/accounts", "Accounts"),
            ("/achievements", "Achievements"), ("/leaderboards", "Leaderboards"),
            ("/assets", "Assets"), ("/avatars", "Avatars"), ("/sessions", "Sessions"),
            ("/social", "Social"), ("/maintenance", "Maintenance")))
        logout = (f'<form method="post" action="/logout"><input type="hidden" name="csrf" '
                  f'value="{esc(session["csrf"])}"><button>Sign out</button></form>')
        message = f'<p class="notice">{esc(notice)}</p>' if notice else ""
        mode = "read-only" if self.server.options.read_only else "read/write"
        return ("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                f"<title>{esc(title)} — CNA admin</title><link rel=stylesheet href=/style.css></head><body>"
                f"<header><h1>CNA Gamer Services administration</h1><nav>{nav} {logout}</nav></header>"
                f"<main><p class=muted>Mode: {mode}. Database: {esc(self.server.options.database)}</p>{message}{body}</main></body></html>")

    def login_page(self, error=""):
        message = f'<p class="error">{esc(error)}</p>' if error else ""
        return ("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<title>CNA admin sign in</title><link rel=stylesheet href=/style.css></head><body><main>"
                f"<section><h1>Administrator sign in</h1>{message}<form method=post action=/login>"
                "<label>Password<input type=password name=password required autocomplete=current-password></label>"
                "<button>Sign in</button></form></section></main></body></html>")

    def parse_form(self):
        try:length = int(self.headers.get("Content-Length", "0"))
        except ValueError:raise ValueError("invalid content length")
        if length < 0 or length > MAX_BODY:raise ValueError("request body is too large")
        body = self.rfile.read(length);content_type = self.headers.get("Content-Type", "")
        if content_type.startswith("application/x-www-form-urlencoded"):
            parsed = urllib.parse.parse_qs(body.decode("utf-8"), keep_blank_values=True, max_num_fields=32)
            return {key: values[-1] for key, values in parsed.items()}
        if content_type.startswith("multipart/form-data"):
            message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                b"Content-Type: " + content_type.encode("ascii") + b"\r\nMIME-Version: 1.0\r\n\r\n" + body)
            result = {}
            for part in message.iter_parts():
                name = part.get_param("name", header="content-disposition")
                if not name:continue
                payload = part.get_payload(decode=True) or b""
                filename = part.get_filename()
                result[name] = (filename, payload) if filename is not None else payload.decode("utf-8")
            return result
        raise ValueError("unsupported content type")

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/style.css":return self.send_bytes(200, STYLE, "text/css; charset=utf-8")
        if path == "/login":return self.send_bytes(200, self.login_page())
        token, session = self.authenticated()
        if not session:return self.redirect("/login")
        try:
            notice = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("notice", [""])[0]
            body = self.render(path, session)
            self.send_bytes(200, self.page(path.strip("/").title() or "Dashboard", body, session, notice))
        except KeyError:
            self.send_bytes(404, self.page("Not found", "<section><h2>Not found</h2></section>", session))
        except sqlite3.Error:
            self.audit("read", "database_error")
            self.send_bytes(503, self.page("Database unavailable", "<section><h2>Database unavailable</h2>"
                            "<p>Check the path, schema and server logs.</p></section>", session))

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        try:fields = self.parse_form()
        except (ValueError, UnicodeError):return self.send_bytes(400, self.login_page("Invalid request"))
        if path == "/login":
            source = self.client_address[0]
            if not self.server.login_allowed(source):
                self.audit("login", "rate_limited")
                return self.send_bytes(429, self.login_page("Too many attempts; wait one minute"),
                                       extra=[("Retry-After", str(LOGIN_WINDOW_SECONDS))])
            password = fields.get("password", "")
            if not isinstance(password, str) or not hmac.compare_digest(
                    hashlib.sha256(password.encode()).digest(), self.server.password_digest):
                self.server.record_login(source, False)
                self.audit("login", "refused");return self.send_bytes(401, self.login_page("Authentication failed"))
            self.server.record_login(source, True)
            token, _session = self.server.new_session();secure = "; Secure" if self.server.options.tls_cert else ""
            self.audit("login")
            return self.redirect("/", [("Set-Cookie", f"{COOKIE}={token}; Path=/; HttpOnly; SameSite=Strict{secure}")])
        token, session = self.authenticated()
        if not session:return self.send_bytes(401, self.login_page("Session expired"))
        supplied = fields.get("csrf", "")
        if not isinstance(supplied, str) or not hmac.compare_digest(supplied, session["csrf"]):
            self.audit("csrf", "refused");return self.send_bytes(403, self.page("Forbidden", "<section><h2>Forbidden</h2></section>", session))
        if path == "/logout":
            self.server.remove_session(token);self.audit("logout")
            return self.redirect("/login", [("Set-Cookie", f"{COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict")])
        if not path.startswith("/action/"):return self.send_bytes(404, "Not found", "text/plain")
        if self.server.options.read_only:return self.send_bytes(403, self.page("Read only", "<section><h2>Read-only mode</h2></section>", session))
        action = path.removeprefix("/action/")
        try:
            self.action(action, fields)
            self.audit("mutation:" + action)
            return self.redirect(self.action_page(action) + "?notice=" + urllib.parse.quote("Operation completed"))
        except (ValueError, KeyError) as error:
            self.audit("mutation:" + action, "invalid")
            return self.send_bytes(400, self.page("Invalid operation", f"<section><h2>Invalid operation</h2><p>{esc(error)}</p></section>", session))
        except RuntimeError as error:
            self.audit("mutation:" + action, "admin_refused")
            return self.send_bytes(409, self.page("Operation refused", f"<section><h2>Operation refused</h2><p>{esc(error)}</p></section>", session))

    def render(self, path, session):
        csrf = session["csrf"]
        if path == "/":
            version = self.query("PRAGMA user_version")[0][0]
            counts = self.query("SELECT (SELECT COUNT(*) FROM titles),(SELECT COUNT(*) FROM users),"
                "(SELECT COUNT(*) FROM sessions WHERE expires>unixepoch()),"
                "(SELECT COUNT(*) FROM directory_sessions WHERE expires>unixepoch()),"
                "(SELECT COUNT(*) FROM session_invitations WHERE status='pending' AND expires>unixepoch()),"
                "(SELECT COUNT(*) FROM parties)")[0]
            return "<section><h2>Dashboard</h2>" + table(
                ("Schema", "Titles", "Accounts", "Access sessions", "Directory sessions", "Pending invitations", "Parties"),
                [(version, *counts)]) + "</section>"
        if path == "/titles":
            rows = self.query("SELECT t.id,t.name,t.minimum_version,COUNT(DISTINCT a.key),COUNT(DISTINCT l.key) "
                              "FROM titles t LEFT JOIN achievements a ON a.game_id=t.id LEFT JOIN leaderboards l ON l.game_id=t.id "
                              "GROUP BY t.id ORDER BY t.id")
            create = form("create-title", csrf, field("id", "ID") + field("name", "Name"), "Create title")
            minimum = form("minimum-version", csrf, field("id", "Title ID") + field("version", "Minimum version", required=False))
            return "<section><h2>Titles</h2>" + table(("ID", "Name", "Minimum version", "Achievements", "Boards"), rows) + "</section><section><h2>Create</h2>" + create + "<h2>Version policy</h2>" + minimum + "</section>"
        if path == "/accounts":
            search = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("q", [""])[0][:64]
            rows = self.query("SELECT u.username,u.gamertag,u.gamer_zone,u.region,u.picture,u.game_defaults,u.privilege_communication,u.privilege_profile_viewing,"
                "u.privilege_user_content,(SELECT COUNT(*) FROM sessions s WHERE s.user_id=u.id AND s.expires>unixepoch()),"
                "(SELECT COUNT(*) FROM earned e WHERE e.user_id=u.id) FROM users u WHERE u.username LIKE ? OR u.gamertag LIKE ? ORDER BY u.username LIMIT 200",
                (f"%{search}%", f"%{search}%"))
            search_form = f'<form method=get><label>Search<input name=q value="{esc(search)}"></label><button>Search</button></form>'
            create = form("create-account", csrf, field("username", "Username") + field("gamertag", "Gamertag") + field("password", "Initial password", "password"), "Provision account")
            credentials = form("revoke-user", csrf, field("username", "Username"), "Revoke all credentials", True) + form("expire-access", csrf, field("username", "Username"), "Expire access tokens")
            privilege_fields = field("username", "Username") + '<label>Privilege<select name=name><option>communication</option><option>profileViewing</option><option>userContent</option><option>trade</option><option>purchase</option><option>premium</option></select></label>' + field("value", "Value")
            return "<section><h2>Accounts</h2>" + search_form + table(("Username", "Gamertag", "Zone", "Region", "Picture", "Game defaults", "Communication", "Profile", "Content", "Live access", "Earned"), rows) + "</section><section><h2>Provision</h2>" + create + "<h2>Credential maintenance</h2>" + credentials + "<h2>Privileges</h2>" + form("privilege", csrf, privilege_fields) + "</section>"
        if path == "/achievements":
            rows = self.query("SELECT a.game_id,a.key,a.name,a.score,a.display,COUNT(e.user_id) FROM achievements a LEFT JOIN earned e ON e.game_id=a.game_id AND e.key=a.key GROUP BY a.game_id,a.key ORDER BY a.game_id,a.key")
            fields = field("title", "Title") + field("key", "Key") + field("name", "Name") + field("description", "Description") + field("howToEarn", "How to earn") + field("score", "Score", "number", "0")
            return "<section><h2>Achievement catalog</h2>" + table(("Title", "Key", "Name", "Score", "Display", "Earned count"), rows) + "</section><section><h2>Add achievement</h2>" + form("achievement", csrf, fields) + "</section>"
        if path == "/leaderboards":
            rows = self.query("SELECT l.game_id,l.key,l.mode,l.ascending,l.aggregation,l.arbitrated,l.columns,COUNT(e.user_id) FROM leaderboards l LEFT JOIN leaderboard_entries e ON e.game_id=l.game_id AND e.key=l.key AND e.mode=l.mode GROUP BY l.game_id,l.key,l.mode ORDER BY l.game_id,l.key,l.mode")
            entries = self.query("SELECT e.game_id,e.key,e.mode,u.gamertag,e.rating,e.columns,e.updated FROM leaderboard_entries e JOIN users u ON u.id=e.user_id ORDER BY e.updated DESC LIMIT 500")
            fields = field("title", "Title") + '<label>Definition JSON<textarea name=json required rows=6></textarea></label>'
            seed = field("title", "Title") + '<label>Fixture row JSON<textarea name=json required rows=6></textarea></label>'
            return "<section><h2>Leaderboard definitions</h2>" + table(("Title", "Key", "Mode", "Ascending", "Aggregation", "Arbitrated", "Columns", "Rows"), rows) + "<h2>Recent rows</h2>" + table(("Title", "Key", "Mode", "Gamertag", "Rating", "Columns", "Updated"), entries) + "</section><section><h2>Define board</h2>" + form("leaderboard", csrf, fields) + "<h2>Seed development fixture</h2>" + form("seed-leaderboard", csrf, seed) + "</section>"
        if path == "/assets":
            rows = self.query("SELECT a.hash,a.mime,a.size,COUNT(t.game_id) FROM assets a LEFT JOIN title_assets t ON t.hash=a.hash GROUP BY a.hash ORDER BY a.hash LIMIT 500")
            upload = field("title", "Title") + '<label>MIME<select name=mime><option>image/png</option><option>model/gltf-binary</option></select></label><label>File<input type=file name=file required></label>'
            picture = form("picture", csrf, field("username", "Username") + field("hash", "Asset SHA-256"), "Assign gamer picture")
            return "<section><h2>Assets</h2>" + table(("SHA-256", "MIME", "Bytes", "Titles"), rows) + "</section><section><h2>Import validated asset</h2>" + form("asset", csrf, upload, "Import", multipart=True) + "<h2>Gamer picture</h2>" + picture + "</section>"
        if path == "/avatars":
            catalogs = self.query("SELECT c.version,c.imported,COUNT(DISTINCT i.id),COUNT(DISTINCT a.name) FROM avatar_catalogs c LEFT JOIN avatar_catalog_items i ON i.version=c.version LEFT JOIN avatar_catalog_assets a ON a.version=c.version GROUP BY c.version ORDER BY c.version")
            accounts = self.query("SELECT u.username,u.gamertag,COALESCE(a.revision,0),COALESCE(a.updated,0) FROM users u LEFT JOIN avatars a ON a.user_id=u.id ORDER BY u.username LIMIT 500")
            catalog = form("avatar-catalog", csrf, field("directory", "Catalog directory on admin host"), "Import catalog")
            avatar_fields = field("username", "Username") + '<label>Action<select name=action><option>random</option><option>clear</option><option>set</option></select></label>' + field("body", "Body (male/female; random only)", required=False) + field("description", "Description hex (set only)", required=False)
            return "<section><h2>Catalogs</h2>" + table(("Version", "Imported", "Items", "Assets"), catalogs) + "<h2>Account avatar metadata</h2>" + table(("Username", "Gamertag", "Revision", "Updated"), accounts) + "</section><section><h2>Import catalog</h2>" + catalog + "<h2>Set/clear/randomize</h2>" + form("avatar", csrf, avatar_fields) + "</section>"
        if path == "/sessions":
            sessions = self.query("SELECT d.id,d.game_id,u.gamertag,d.kind,d.state,d.max_gamers,d.private_slots,d.allow_join,d.allow_migration,d.revision,d.expires,COUNT(m.user_id) FROM directory_sessions d JOIN users u ON u.id=d.host_id LEFT JOIN directory_members m ON m.session_id=d.id GROUP BY d.id ORDER BY d.created DESC LIMIT 500")
            members = self.query("SELECT m.game_id,m.session_id,u.gamertag,m.machine_id,m.private_slot,m.ordinal FROM directory_members m JOIN users u ON u.id=m.user_id ORDER BY m.game_id,m.session_id,m.ordinal LIMIT 1000")
            invitations = self.query("SELECT i.game_id,s.gamertag,r.gamertag,i.status,i.requested,i.created,i.expires FROM session_invitations i JOIN users s ON s.id=i.sender_id JOIN users r ON r.id=i.recipient_id ORDER BY i.created DESC LIMIT 500")
            parties = self.query("SELECT p.id,u.gamertag,p.created,COUNT(DISTINCT m.user_id),COUNT(DISTINCT i.recipient_id) FROM parties p JOIN users u ON u.id=p.leader_id LEFT JOIN party_members m ON m.party_id=p.id LEFT JOIN party_invitations i ON i.party_id=p.id GROUP BY p.id ORDER BY p.created DESC")
            return "<section><h2>Directory sessions</h2>" + table(("ID", "Title", "Host", "Kind", "State", "Max", "Private", "Join", "Migrate", "Revision", "Expires", "Members"), sessions) + "<h2>Memberships</h2>" + table(("Title", "Session", "Gamertag", "Machine", "Private", "Ordinal"), members) + "<h2>Invitations</h2>" + table(("Title", "Sender", "Recipient", "Status", "Join request", "Created", "Expires"), invitations) + "<h2>Parties</h2>" + table(("ID", "Leader", "Created", "Members", "Invitations"), parties) + "</section>"
        if path == "/social":
            counts = self.query("SELECT (SELECT COUNT(*) FROM friends),(SELECT COUNT(*) FROM blocks),(SELECT COUNT(*) FROM messages),(SELECT COUNT(*) FROM messages WHERE read=0),(SELECT COUNT(*) FROM player_reviews)")[0]
            blocks = self.query("SELECT u.gamertag,b.gamertag,bl.created FROM blocks bl JOIN users u ON u.id=bl.user_id JOIN users b ON b.id=bl.blocked_id ORDER BY bl.created DESC LIMIT 200")
            reviews = self.query("SELECT r.gamertag,s.gamertag,p.rating,p.updated FROM player_reviews p JOIN users r ON r.id=p.reviewer_id JOIN users s ON s.id=p.subject_id ORDER BY p.updated DESC LIMIT 200")
            return "<section><h2>Social metadata</h2>" + table(("Friend edges", "Blocks", "Messages", "Unread", "Reviews"), [counts]) + "<p>Message bodies are deliberately not displayed.</p><h2>Blocks</h2>" + table(("Blocker", "Blocked", "Created"), blocks) + "<h2>Reviews</h2>" + table(("Reviewer", "Subject", "Rating", "Updated"), reviews) + "</section>"
        if path == "/maintenance":
            reset_earned = form("reset-earned", csrf, field("title", "Title ID") + field("confirm", "Type title ID to confirm"), "Reset earned achievements", True)
            reset_online = form("reset-online", csrf, field("title", "Title ID") + field("confirm", "Type title ID to confirm"), "Reset online sessions", True)
            defaults = form("game-defaults", csrf, field("username", "Username") + '<label>GameDefaults JSON<textarea name=json required rows=5></textarea></label>', "Replace game defaults")
            return "<section class=danger><h2>Confirmed destructive maintenance</h2><p>These actions cannot be undone except from backup.</p>" + reset_earned + reset_online + "</section><section><h2>Profile maintenance</h2>" + defaults + "</section>"
        raise KeyError(path)

    def value(self, fields, name, maximum=4096, allow_empty=False):
        value = fields[name]
        if not isinstance(value, str) or len(value) > maximum or (not allow_empty and not value):
            raise ValueError(f"invalid {name}")
        return value

    def run_admin(self, arguments, stdin=None):
        try:
            result = subprocess.run([str(self.server.options.admin), str(self.server.options.database), *arguments],
                                    input=stdin, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError("administrator command timed out") from error
        if result.returncode != 0:raise RuntimeError(result.stderr.strip() or "administration failed")
        return result.stdout.strip()

    def action(self, action, fields):
        if action == "create-title":self.run_admin(["title", self.value(fields, "id", 64), self.value(fields, "name", 256)])
        elif action == "minimum-version":self.run_admin(["title-minimum-version", self.value(fields, "id", 64), self.value(fields, "version", 32, True)])
        elif action == "create-account":self.run_admin(["user", self.value(fields, "username", 64), self.value(fields, "gamertag", 64)], self.value(fields, "password", 1024) + "\n")
        elif action in ("revoke-user", "expire-access"):self.run_admin([action, self.value(fields, "username", 64)])
        elif action == "privilege":self.run_admin(["privilege", self.value(fields, "username", 64), self.value(fields, "name", 64), self.value(fields, "value", 64)])
        elif action == "achievement":
            document = {key: self.value(fields, key, 1024) for key in ("key", "name", "description", "howToEarn")}
            document["score"] = int(self.value(fields, "score", 8));self.run_admin(["achievement", self.value(fields, "title", 64)], json.dumps(document))
        elif action in ("leaderboard", "seed-leaderboard"):
            document = json.loads(self.value(fields, "json", 65536));self.run_admin([action, self.value(fields, "title", 64)], json.dumps(document))
        elif action == "asset":
            filename, content = fields["file"]
            if not filename or len(content) > 16 * 1024 * 1024:raise ValueError("invalid file")
            temporary = pathlib.Path(self.server.temporary.name) / (secrets.token_hex(16) + ".upload")
            try:
                temporary.write_bytes(content);os.chmod(temporary, 0o600)
                self.run_admin(["asset", self.value(fields, "title", 64), self.value(fields, "mime", 64), str(temporary)])
            finally:
                temporary.unlink(missing_ok=True)
        elif action == "picture":self.run_admin(["picture", self.value(fields, "username", 64), self.value(fields, "hash", 64)])
        elif action == "avatar-catalog":self.run_admin(["avatar-catalog", self.value(fields, "directory", 4096)])
        elif action == "avatar":
            username, mode = self.value(fields, "username", 64), self.value(fields, "action", 16)
            arguments = ["avatar", username, mode];stdin = None
            if mode == "random" and self.value(fields, "body", 16, True):arguments.append(self.value(fields, "body", 16))
            elif mode == "set":stdin = self.value(fields, "description", 4096) + "\n"
            elif mode not in ("random", "clear"):raise ValueError("invalid avatar action")
            self.run_admin(arguments, stdin)
        elif action == "game-defaults":
            document = json.loads(self.value(fields, "json", 65536));self.run_admin(["game-defaults", self.value(fields, "username", 64)], json.dumps(document))
        elif action in ("reset-earned", "reset-online"):
            title = self.value(fields, "title", 64)
            if not hmac.compare_digest(title, self.value(fields, "confirm", 64)):raise ValueError("confirmation must exactly match title ID")
            self.run_admin([action, title])
        else:raise KeyError(action)

    def action_page(self, action):
        if action in ("create-title", "minimum-version"):return "/titles"
        if action in ("create-account", "revoke-user", "expire-access", "privilege"):return "/accounts"
        if action == "achievement":return "/achievements"
        if action in ("leaderboard", "seed-leaderboard"):return "/leaderboards"
        if action in ("asset", "picture"):return "/assets"
        if action in ("avatar", "avatar-catalog"):return "/avatars"
        return "/maintenance"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=pathlib.Path)
    parser.add_argument("--admin", required=True, type=pathlib.Path, help="cna-gamer-services-admin executable")
    parser.add_argument("--password-file", required=True, type=pathlib.Path)
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47833)
    parser.add_argument("--read-only", action="store_true")
    parser.add_argument("--allow-remote", action="store_true")
    parser.add_argument("--tls-cert", type=pathlib.Path)
    parser.add_argument("--tls-key", type=pathlib.Path)
    options = parser.parse_args()
    try:
        address = ipaddress.ip_address(options.listen)
        if not 0 <= options.port <= 65535:raise ValueError("invalid port")
        remote = not address.is_loopback
        if remote and not options.allow_remote:raise ValueError("REMOTE_BIND_REFUSED")
        if remote and not (options.tls_cert and options.tls_key):raise ValueError("REMOTE_TLS_REQUIRED")
        if bool(options.tls_cert) != bool(options.tls_key):raise ValueError("TLS_CERTIFICATE_AND_KEY_REQUIRED")
        require = lambda condition, message: condition or (_ for _ in ()).throw(ValueError(message))
        require(options.database.is_file(), "DATABASE_NOT_FOUND")
        require(options.admin.is_file() and os.access(options.admin, os.X_OK), "ADMIN_EXECUTABLE_NOT_FOUND")
        mode = options.password_file.stat().st_mode
        require(mode & 0o077 == 0, "PASSWORD_FILE_PERMISSIONS")
        password = options.password_file.read_text().rstrip("\r\n")
        require(bool(password), "EMPTY_ADMIN_PASSWORD")
        password_digest = hashlib.sha256(password.encode()).digest();password = ""
        server = AdminServer((str(address), options.port), Handler, options, password_digest)
        if options.tls_cert:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(options.tls_cert, options.tls_key);server.socket = context.wrap_socket(server.socket, server_side=True)
        scheme = "https" if options.tls_cert else "http"
        print(f"CNA admin web listening on {scheme}://{address}:{server.server_address[1]}/", flush=True)
        try:server.serve_forever(poll_interval=.25)
        finally:server.close()
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=os.sys.stderr);return 1


if __name__ == "__main__":
    raise SystemExit(main())
