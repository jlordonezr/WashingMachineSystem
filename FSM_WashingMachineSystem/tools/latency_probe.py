#!/usr/bin/env python3
"""Times the controller's web answers, for example during a whole wash (3.16).

    python3 tools/latency_probe.py                       # 60 s, reusewater.local
    python3 tools/latency_probe.py --minutes 70          # a whole cycle
    python3 tools/latency_probe.py --idle 2              # also hold 2 silent connections open, as Safari does
    python3 tools/latency_probe.py --host 192.168.1.50

The name is looked up once and timed on its own (mDNS), then every request goes
to the IP address, so the numbers show the controller, not the name lookup.
It asks /api/ping (no password needed, firmware 3.16+); older firmware answers
it with 401, which is just as fast to time. Exit code 1 if any request took
longer than --limit ms (default 300) or failed.
"""
import argparse
import socket
import statistics
import sys
import threading
import time


def lookup(host):
    t = time.monotonic()
    try:
        infos = socket.getaddrinfo(host, 80, proto=socket.IPPROTO_TCP)
    except OSError as e:
        return None, time.monotonic() - t, str(e)
    v4 = [i[4][0] for i in infos if i[0] == socket.AF_INET]
    return (v4[0] if v4 else infos[0][4][0]), time.monotonic() - t, None


def request(ip, path, timeout):
    """One HTTP/1.1 GET on a new connection, like the app. Returns (ms, status) or (ms, error)."""
    t = time.monotonic()
    try:
        with socket.create_connection((ip, 80), timeout=timeout) as s:
            s.sendall(("GET %s HTTP/1.1\r\nHost: reusewater.local\r\nConnection: close\r\n\r\n" % path).encode())
            data = b""
            while b"\r\n" not in data:
                chunk = s.recv(256)
                if not chunk:
                    break
                data += chunk
            ms = (time.monotonic() - t) * 1000
            line = data.split(b"\r\n", 1)[0].decode(errors="replace")
            return ms, line.split(" ")[1] if " " in line else "no answer"
    except OSError as e:
        return (time.monotonic() - t) * 1000, type(e).__name__


def hold_idle(ip, stop):
    """Keeps one silent connection open, opening a new one every 4 s."""
    while not stop.is_set():
        try:
            s = socket.create_connection((ip, 80), timeout=3)
            stop.wait(4)
            s.close()
        except OSError:
            stop.wait(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="reusewater.local")
    ap.add_argument("--path", default="/api/ping")
    ap.add_argument("--minutes", type=float, default=1.0)
    ap.add_argument("--every", type=float, default=1.0, help="seconds between requests")
    ap.add_argument("--idle", type=int, default=0, help="silent connections to hold open")
    ap.add_argument("--limit", type=float, default=300.0, help="ms counted as too slow")
    a = ap.parse_args()

    ip, dns_s, err = lookup(a.host)
    if not ip:
        print("Name lookup failed after %.2f s: %s" % (dns_s, err))
        return 1
    print("Name lookup: %s -> %s in %.0f ms%s" % (a.host, ip, dns_s * 1000,
          "  <- slow: the controller may not answer the IPv6 question (fixed in 3.16)" if dns_s > 1 else ""))

    stop = threading.Event()
    for _ in range(a.idle):
        threading.Thread(target=hold_idle, args=(ip, stop), daemon=True).start()
    if a.idle:
        print("Holding %d silent connection(s) open, as Safari does." % a.idle)
        time.sleep(0.3)

    times, slow, failed = [], 0, 0
    end = time.monotonic() + a.minutes * 60
    try:
        while time.monotonic() < end:
            ms, result = request(ip, a.path, timeout=8)
            ok = result.isdigit()
            if ok:
                times.append(ms)
            else:
                failed += 1
            if not ok or ms > a.limit:
                slow += ok
                print("%s  %6.0f ms  %s%s" % (time.strftime("%H:%M:%S"), ms, result, "" if ok else "  FAILED"))
            time.sleep(max(0.0, a.every - ms / 1000))
    except KeyboardInterrupt:
        pass
    stop.set()

    if not times:
        print("No answers.")
        return 1
    times.sort()
    p95 = times[min(len(times) - 1, int(len(times) * 0.95))]
    print("%d answers: median %.0f ms, 95%% under %.0f ms, slowest %.0f ms; %d over %.0f ms, %d failed"
          % (len(times), statistics.median(times), p95, times[-1], slow, a.limit, failed))
    return 1 if slow or failed else 0


if __name__ == "__main__":
    sys.exit(main())
