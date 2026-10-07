# SPDX-License-Identifier: MIT
"""Authentication, CSRF, mutation reuse, read-only and bind-policy tests for admin web."""

import http.client
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import urllib.parse


def check(value, reason):
    if not value:raise AssertionError(reason)


class Browser:
    def __init__(self, port):self.port, self.cookie = port, ""
    def request(self, method, path, fields=None):
        body = urllib.parse.urlencode(fields or {}) if fields is not None else None
        headers = {}
        if fields is not None:headers["Content-Type"] = "application/x-www-form-urlencoded"
        if self.cookie:headers["Cookie"] = self.cookie
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        connection.request(method, path, body, headers);response = connection.getresponse();data = response.read().decode()
        cookie = response.getheader("Set-Cookie")
        if cookie:self.cookie = cookie.split(";", 1)[0]
        result = response.status, dict(response.getheaders()), data
        connection.close();return result


def start(script, database, admin, password, read_only=False):
    arguments = [sys.executable, str(script), "--database", str(database), "--admin", str(admin),
                 "--password-file", str(password), "--port", "0"]
    if read_only:arguments.append("--read-only")
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    line = process.stdout.readline().strip();check(line.startswith("CNA admin web listening"), "startup: " + line)
    return process, int(line.rsplit(":", 1)[1].rstrip("/"))


def stop(process):
    process.terminate();process.wait(timeout=10);process.stdout.close();process.stderr.close()


def login(browser, password):
    status, headers, body = browser.request("POST", "/login", {"password": password})
    check(status == 303 and browser.cookie.startswith("cna_admin_session="), "login")
    status, headers, body = browser.request("GET", "/")
    check(status == 200 and "Dashboard" in body and headers.get("X-Frame-Options") == "DENY", "dashboard/security headers")
    match = re.search(r'name="csrf" value="([^"]+)"', body);check(match, "CSRF token")
    return match.group(1)


def main():
    build = pathlib.Path(sys.argv[1]).resolve();root_repo = pathlib.Path(__file__).resolve().parents[1]
    script = root_repo / "tools/cna-gamer-services-admin-web/admin_web.py";admin = build / "cna-gamer-services-admin"
    with tempfile.TemporaryDirectory(prefix="cna-admin-web-") as temporary:
        root = pathlib.Path(temporary);database = root / "service.sqlite3";password_file = root / "password"
        password_file.write_text("correct horse battery staple\n");password_file.chmod(0o600)
        subprocess.run([str(admin), str(database), "title", "initial", "Initial"], check=True)
        process, port = start(script, database, admin, password_file)
        try:
            browser = Browser(port)
            status, headers, _ = browser.request("GET", "/")
            check(status == 303 and headers.get("Location") == "/login", "authentication required")
            status, _, body = browser.request("POST", "/login", {"password": "wrong-secret-value"})
            check(status == 401 and "wrong-secret-value" not in body, "wrong password refusal")
            csrf = login(browser, "correct horse battery staple")
            status, _, _ = browser.request("POST", "/action/create-title", {"id": "no-csrf", "name": "No"})
            check(status == 403, "CSRF required")
            status, _, _ = browser.request("POST", "/action/create-title", {
                "csrf": csrf, "id": "web-title", "name": "Web title"})
            check(status == 303, "title mutation")
            status, _, body = browser.request("GET", "/titles")
            check(status == 200 and "web-title" in body, "title visible")
            status, _, _ = browser.request("POST", "/action/create-account", {
                "csrf": csrf, "username": "web-user", "gamertag": "WebUser",
                "password": "account-secret-not-for-output"})
            check(status == 303, "account provision")
            status, _, body = browser.request("GET", "/accounts?q=web-user")
            check(status == 200 and "web-user" in body and "account-secret-not-for-output" not in body,
                  "account search does not disclose password")
            status, _, _ = browser.request("POST", "/action/reset-online", {
                "csrf": csrf, "title": "web-title", "confirm": "different"})
            check(status == 400, "destructive confirmation")
            with sqlite3.connect(database) as connection:
                check(connection.execute("SELECT COUNT(*) FROM titles WHERE id='web-title'").fetchone()[0] == 1,
                      "C++ admin mutation persisted")
            attacker = Browser(port)
            for _ in range(10):
                status, _, _ = attacker.request("POST", "/login", {"password": "wrong-secret-value"})
                check(status == 401, "login attempt budget")
            status, headers, body = attacker.request("POST", "/login", {"password": "wrong-secret-value"})
            check(status == 429 and headers.get("Retry-After") == "60" and "wait one minute" in body,
                  "login rate limit")
        finally:stop(process)

        read_process, read_port = start(script, database, admin, password_file, True)
        try:
            browser = Browser(read_port);csrf = login(browser, "correct horse battery staple")
            status, _, body = browser.request("POST", "/action/create-title", {
                "csrf": csrf, "id": "refused", "name": "Refused"})
            check(status == 403 and "Read-only" in body, "read-only mutation refusal")
        finally:stop(read_process)

        refused = subprocess.run([sys.executable, str(script), "--database", str(database), "--admin", str(admin),
            "--password-file", str(password_file), "--listen", "0.0.0.0", "--port", "0"],
            capture_output=True, text=True, timeout=15)
        check(refused.returncode != 0 and "REMOTE_BIND_REFUSED" in refused.stderr, "remote bind gate")
        remote_no_tls = subprocess.run([sys.executable, str(script), "--database", str(database), "--admin", str(admin),
            "--password-file", str(password_file), "--listen", "0.0.0.0", "--port", "0", "--allow-remote"],
            capture_output=True, text=True, timeout=15)
        check(remote_no_tls.returncode != 0 and "REMOTE_TLS_REQUIRED" in remote_no_tls.stderr, "remote TLS gate")
        password_file.chmod(0o644)
        unsafe = subprocess.run([sys.executable, str(script), "--database", str(database), "--admin", str(admin),
            "--password-file", str(password_file), "--port", "0"], capture_output=True, text=True, timeout=15)
        check(unsafe.returncode != 0 and "PASSWORD_FILE_PERMISSIONS" in unsafe.stderr, "password permissions")
        print("Admin web authentication, CSRF, C++ mutations, read-only mode and bind policy passed")


if __name__ == "__main__":main()
