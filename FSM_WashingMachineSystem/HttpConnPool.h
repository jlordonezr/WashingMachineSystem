#ifndef HTTP_CONN_POOL_H
#define HTTP_CONN_POOL_H

#include <stdint.h>

// Waiting room for web connections (3.16). Hardware-free, unit tested
// (tests/test_connpool.cpp).
//
// The ESP32 WebServer serves one connection at a time, and it waits up to
// 5 s for a connection that is open but has not sent its request yet. Phones
// open such connections all the time (Safari connects ahead "just in case",
// and a request it gave up on can leave one behind). While the server waited,
// every other request, including the app's status poll, sat in the queue, so
// the app reported "took too long" or "Offline" during a wash although the
// controller was fine (measured: 4.9 s instead of 0.02 s).
//
// The pool holds connections that have not sent anything yet. The server takes
// the first one whose request has arrived, so a silent connection never holds
// up the others. Connections that stay silent longer than maxIdleMs, or that
// the other side closed, are dropped; when the pool is full, the one waiting
// longest makes room for the new one.
//
// Client needs: int available(), bool connected(), void stop(), copyable,
// and a default-constructed value that means "no connection".
template <class Client, uint8_t N>
class HttpConnPool {
public:
  // A newly accepted connection.
  void add(const Client& c, uint32_t now) {
    int slot = -1;
    for (uint8_t i = 0; i < N; ++i) if (!used_[i]) { slot = i; break; }
    if (slot < 0) {                               // full: close the one waiting longest
      slot = 0;
      for (uint8_t i = 1; i < N; ++i) if ((int32_t)(since_[i] - since_[slot]) < 0) slot = i;
      drop(slot);
    }
    clients_[slot] = c;
    since_[slot] = now;
    used_[slot] = true;
  }

  // Moves the connection that has waited longest among those with request
  // bytes ready into out; false if none. Drops closed and long-silent ones.
  bool takeReady(Client& out, uint32_t now, uint32_t maxIdleMs) {
    int best = -1;
    for (uint8_t i = 0; i < N; ++i) {
      if (!used_[i]) continue;
      if (clients_[i].available() > 0) {
        if (best < 0 || (int32_t)(since_[i] - since_[best]) < 0) best = i;
      } else if (!clients_[i].connected() || now - since_[i] > maxIdleMs) {
        drop(i);
      }
    }
    if (best < 0) return false;
    out = clients_[best];
    clients_[best] = Client();                    // the server now holds the only copy
    used_[best] = false;
    return true;
  }

  uint8_t  count() const   { uint8_t n = 0; for (uint8_t i = 0; i < N; ++i) n += used_[i]; return n; }
  uint32_t dropped() const { return dropped_; }   // silent, closed or pushed out (Service tab)

private:
  void drop(int i) {
    clients_[i].stop();
    clients_[i] = Client();
    used_[i] = false;
    dropped_++;
  }
  Client   clients_[N] = {};
  uint32_t since_[N]   = {};
  bool     used_[N]    = {};
  uint32_t dropped_    = 0;
};

#endif
