// Tests for HttpConnPool.h (web connections that have not sent a request yet, 3.16).
// g++ -std=c++17 -O1 -Wall test_connpool.cpp -o test_connpool && ./test_connpool
#include "../HttpConnPool.h"
#include <cstdio>
#include <memory>

static int passes = 0, failures = 0;
#define CHECK(c, ...) do { if (c) ++passes; else { ++failures; printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A connection as the pool sees it. Copies share one socket, like NetworkClient.
struct Sock { int id; int bytes = 0; bool open = true; bool stopped = false; };
struct FakeClient {
  std::shared_ptr<Sock> s;
  int  available() const { return s && s->open ? s->bytes : 0; }
  bool connected() const { return s && s->open; }
  void stop()            { if (s) { s->open = false; s->stopped = true; } }
};
static FakeClient conn(int id) { FakeClient c; c.s = std::make_shared<Sock>(); c.s->id = id; return c; }

typedef HttpConnPool<FakeClient, 4> Pool;

// P1: the case from the field. A silent connection opened first must not hold
// up a status request that arrives after it.
static void P1() {
  Pool p; FakeClient idle = conn(1), status = conn(2);
  p.add(idle, 0); p.add(status, 10);
  status.s->bytes = 120;                                  // "GET /api/status ..."
  FakeClient got;
  CHECK(p.takeReady(got, 20, 5000) && got.s && got.s->id == 2, "status request not served while a silent connection waits");
  CHECK(p.count() == 1 && !idle.s->stopped, "the silent connection was closed too early");
}
// P2: nothing is served before its request arrives, and nothing waits forever.
static void P2() {
  Pool p; FakeClient a = conn(1); p.add(a, 1000);
  FakeClient got;
  CHECK(!p.takeReady(got, 2000, 5000), "a connection without a request was served");
  CHECK(!p.takeReady(got, 6000, 5000) && p.count() == 1, "dropped before 5 s of silence");
  CHECK(!p.takeReady(got, 6001, 5000) && p.count() == 0 && a.s->stopped, "not dropped after 5 s of silence");
  CHECK(p.dropped() == 1, "drop not counted");
}
// P3: connections the phone closed are removed at once.
static void P3() {
  Pool p; FakeClient a = conn(1); p.add(a, 0);
  a.s->open = false;
  FakeClient got;
  CHECK(!p.takeReady(got, 10, 5000) && p.count() == 0, "closed connection kept");
}
// P4: when two requests are ready, the older one is served first.
static void P4() {
  Pool p; FakeClient a = conn(1), b = conn(2);
  p.add(b, 50); p.add(a, 0);                              // b sits in slot 0, a arrived first
  a.s->bytes = b.s->bytes = 10;
  FakeClient got;
  CHECK(p.takeReady(got, 60, 5000) && got.s->id == 1, "served out of order");
  CHECK(p.takeReady(got, 60, 5000) && got.s->id == 2, "second request lost");
  CHECK(p.count() == 0, "pool not empty");
}
// P5: a full pool makes room by closing the connection that waited longest.
static void P5() {
  Pool p; FakeClient c[5];
  for (int i = 0; i < 4; ++i) { c[i] = conn(i); p.add(c[i], 100 + i); }
  c[4] = conn(4); p.add(c[4], 200);
  CHECK(c[0].s->stopped && !c[1].s->stopped && p.count() == 4, "full pool did not close the oldest");
  c[4].s->bytes = 5;
  FakeClient got;
  CHECK(p.takeReady(got, 210, 5000) && got.s->id == 4, "new request lost when the pool was full");
}
// P6: taking a connection leaves the pool without a copy, so the socket closes
// as soon as the server is done with it.
static void P6() {
  Pool p; FakeClient a = conn(1); p.add(a, 0); a.s->bytes = 1;
  std::weak_ptr<Sock> w = a.s; a = FakeClient();
  { FakeClient got; CHECK(p.takeReady(got, 1, 5000), "not served"); }
  CHECK(w.expired(), "the pool still holds the served connection");
}
// P7: millis() wrapping round does not drop a connection early.
static void P7() {
  Pool p; FakeClient a = conn(1); p.add(a, 0xFFFFFF00u);
  FakeClient got;
  CHECK(!p.takeReady(got, 0x00000100u, 5000) && p.count() == 1, "dropped across the millis() wrap");
}

int main() {
  P1(); P2(); P3(); P4(); P5(); P6(); P7();
  printf("%d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
