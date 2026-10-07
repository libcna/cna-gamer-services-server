# SPDX-License-Identifier: MIT
"""Loopback diagnostics, bounded metrics, structured logs and CLI smoke coverage."""

import json
import pathlib
import subprocess
import sys
import tempfile
import urllib.request
import uuid


def check(value, reason):
    if not value:
        raise AssertionError(reason)


def open_url(url, data=None):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    with opener.open(request, timeout=10) as response:
        return response.status, response.headers.get_content_type(), response.read().decode()


def envelope(operation, arguments=None, token=None):
    value = {"v": 1, "id": uuid.uuid4().hex, "game": "diagnostics", "op": operation,
             "args": arguments or {}}
    if token:
        value["token"] = token
    return json.dumps(value).encode()


def stop(process):
    process.terminate()
    process.wait(timeout=10)
    process.stdout.close()
    process.stderr.close()


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    server = build / "cna-gamer-services-server"
    admin = build / "cna-gamer-services-admin"
    with tempfile.TemporaryDirectory(prefix="cna-diagnostics-") as temporary:
        root = pathlib.Path(temporary)
        database = root / "service.sqlite3"
        subprocess.run([str(admin), str(database), "title", "diagnostics", "Diagnostics"], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(admin), str(database), "user", "operator", "Operator"],
                       input="metrics-test-password\n", text=True, check=True, stdout=subprocess.DEVNULL)

        process = subprocess.Popen(
            [str(server), "--database", str(database), "--listen", "127.0.0.1", "--port", "0",
             "--insecure-loopback", "--diagnostics-port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            service_line = process.stdout.readline().strip()
            diagnostics_line = process.stdout.readline().strip()
            check(service_line.startswith("CNA service listening on 127.0.0.1:"), "human service startup log")
            check(diagnostics_line.startswith("CNA diagnostics listening on 127.0.0.1:"), "human diagnostics startup log")
            service_url = "http://127.0.0.1:" + service_line.rsplit(":", 1)[1] + "/cna/v1"
            diagnostics_url = "http://127.0.0.1:" + diagnostics_line.rsplit(":", 1)[1]

            status, content_type, body = open_url(diagnostics_url + "/healthz")
            check(status == 200 and content_type == "application/json" and json.loads(body) == {"status": "ok"},
                  "liveness response")
            status, _, body = open_url(diagnostics_url + "/readyz")
            readiness = json.loads(body)
            check(status == 200 and readiness["status"] == "ready" and
                  readiness["schemaVersion"] == readiness["expectedSchemaVersion"] == 23, "readiness response")

            _, _, body = open_url(service_url, envelope("hello"))
            check(json.loads(body)["error"] == "OK", "hello request")
            attacker_operation = "attacker-chosen-metric-label"
            _, _, body = open_url(service_url, envelope(attacker_operation))
            check(json.loads(body)["error"] == "UNKNOWN_OPERATION", "unknown operation")
            _, _, body = open_url(service_url, b"not-json")
            check(json.loads(body)["error"] == "MALFORMED_MESSAGE", "malformed envelope")
            _, _, body = open_url(service_url, envelope("auth.login", {
                "username": "operator", "password": "wrong-password"}))
            check(json.loads(body)["error"] == "AUTHENTICATION_FAILED", "failed authentication")
            _, _, body = open_url(service_url, envelope("auth.login", {
                "username": "operator", "password": "metrics-test-password"}))
            signed_in = json.loads(body)
            check(signed_in["error"] == "OK", "successful authentication")
            token = signed_in["result"]["token"]
            _, _, body = open_url(service_url, envelope("auth.ping", token=token))
            check(json.loads(body)["error"] == "OK", "authenticated request")

            status, content_type, metrics = open_url(diagnostics_url + "/metrics")
            check(status == 200 and content_type == "text/plain", "Prometheus response")
            for sample in (
                "cna_up 1", "cna_ready 1", "cna_schema_version 23", "cna_titles 1", "cna_accounts 1",
                'cna_operations_total{operation="hello"} 1',
                'cna_operations_total{operation="unknown"} 1',
                'cna_operations_total{operation="invalid"} 1',
                'cna_responses_total{outcome="AUTHENTICATION_FAILED"} 1',
            ):
                check(sample in metrics, "missing metric: " + sample)
            check(attacker_operation not in metrics, "attacker-controlled metric label")
            check(token not in metrics and "metrics-test-password" not in metrics, "secret in metrics")
        finally:
            stop(process)

        json_database = root / "json.sqlite3"
        json_process = subprocess.Popen(
            [str(server), "--database", str(json_database), "--listen", "127.0.0.1", "--port", "0",
             "--insecure-loopback", "--log-format", "json"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            startup = json.loads(json_process.stdout.readline())
            check(startup["event"] == "service_listening" and startup["level"] == "info" and
                  startup["address"] == "127.0.0.1" and startup["port"] > 0 and
                  startup["tls"] is False and startup["version"], "structured startup log")
        finally:
            stop(json_process)

        refused = subprocess.run(
            [str(server), "--database", str(root / "refused.sqlite3"), "--listen", "127.0.0.1",
             "--port", "0", "--insecure-loopback", "--diagnostics-listen", "0.0.0.0",
             "--diagnostics-port", "0"], capture_output=True, text=True, timeout=15,
        )
        check(refused.returncode != 0 and "DIAGNOSTICS_BIND_REFUSED" in refused.stderr,
              "non-loopback diagnostics must fail closed")
        check("--diagnostics-port" in subprocess.check_output([str(server), "--help"], text=True), "CLI help")
        check("0.1.0" in subprocess.check_output([str(server), "--version"], text=True), "CLI version")
        print("Diagnostics health, readiness, bounded metrics, JSON logs and loopback policy passed")


if __name__ == "__main__":
    main()
