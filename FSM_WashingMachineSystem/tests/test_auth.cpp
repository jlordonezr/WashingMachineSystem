// Tests AuthToken.h: SHA-256 and HMAC-SHA256 against published test vectors,
// then the "remember this device" tokens and the cookie parser (3.11).
#include "../AuthToken.h"
#include <cstdio>
#include <string>

static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) ++pass; else { ++fail; printf("    FAIL: %s\n", m); } } while (0)

static std::string hex(const uint8_t* b, size_t n) { char o[129]; authHex(b, n, o); return o; }
static std::string sha(const std::string& s) { Sha256 h; h.update(s.data(), s.size()); uint8_t o[32]; h.final(o); return hex(o, 32); }

int main() {
  printf("A1 SHA-256 test vectors (FIPS 180-2)\n");
  CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty string");
  CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit message (padding into a second block)");
  CHECK(sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "one million a");

  printf("A2 HMAC-SHA256 test vectors (RFC 4231)\n");
  uint8_t out[32];
  uint8_t k1[20]; memset(k1, 0x0b, sizeof(k1));
  hmacSha256(k1, sizeof(k1), "Hi There", nullptr, nullptr, out);
  CHECK(hex(out, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "case 1");
  hmacSha256((const uint8_t*)"Jefe", 4, "what do ya want ", "for nothing?", nullptr, out);   // also checks joined parts
  CHECK(hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "case 2");
  uint8_t k6[131]; memset(k6, 0xaa, sizeof(k6));
  hmacSha256(k6, sizeof(k6), "Test Using Larger Than Block-Size Key - Hash Key First", nullptr, nullptr, out);
  CHECK(hex(out, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "case 6 (key longer than a block)");

  printf("A3 tokens: admin and viewer, remember flag, length\n");
  uint8_t key[AUTH_KEY_LEN]; for (int i = 0; i < AUTH_KEY_LEN; ++i) key[i] = (uint8_t)(i * 7 + 1);
  uint8_t nonce[AUTH_NONCE_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  char tok[AUTH_TOKEN_LEN + 1], vtok[AUTH_TOKEN_LEN + 1];
  authMakeToken(key, 'a', true, nonce, "admin-pass-1", tok);
  authMakeToken(key, 'v', false, nonce, "viewer-pass-1", vtok);
  CHECK(strlen(tok) == AUTH_TOKEN_LEN && tok[0] == 'a' && tok[1] == '1' && tok[18] == '.', "admin token format");
  bool rem = false;
  CHECK(authCheckToken(key, tok, "admin-pass-1", "viewer-pass-1", &rem) == 'a' && rem, "admin token not accepted");
  CHECK(authCheckToken(key, vtok, "admin-pass-1", "viewer-pass-1", &rem) == 'v' && !rem, "viewer token not accepted");

  printf("A4 tokens stop working when they should\n");
  CHECK(authCheckToken(key, tok, "admin-pass-2", "viewer-pass-1") == 0, "admin token survives a password change");
  CHECK(authCheckToken(key, vtok, "admin-pass-1", "viewer-pass-2") == 0, "viewer token survives a viewer password change");
  CHECK(authCheckToken(key, vtok, "admin-pass-1", "") == 0, "viewer token works with the viewer turned off");
  CHECK(authCheckToken(key, vtok, "viewer-pass-1", "x-other-pass") == 0, "a viewer token became admin");
  uint8_t key2[AUTH_KEY_LEN]; memcpy(key2, key, sizeof(key)); key2[5] ^= 1;
  CHECK(authCheckToken(key2, tok, "admin-pass-1", "viewer-pass-1") == 0, "token survives a new key (sign out other devices)");
  for (size_t i = 0; i < AUTH_TOKEN_LEN; ++i) {           // any changed character, including the role
    char t2[AUTH_TOKEN_LEN + 1]; memcpy(t2, tok, sizeof(t2));
    t2[i] = t2[i] == 'a' ? 'v' : (t2[i] == '0' ? '1' : (t2[i] == '1' ? '0' : (t2[i] == 'f' ? 'e' : 'f')));
    if (t2[i] == '.') t2[i] = '-';
    if (authCheckToken(key, t2, "admin-pass-1", "admin-pass-1") != 0) { CHECK(false, "a changed token was accepted"); break; }
  }
  CHECK(authCheckToken(key, "", "admin-pass-1", "") == 0 && authCheckToken(key, nullptr, "admin-pass-1", "") == 0, "empty token accepted");
  std::string longer = std::string(tok) + "0";
  CHECK(authCheckToken(key, longer.c_str(), "admin-pass-1", "") == 0, "longer token accepted");

  printf("A5 cookie header\n");
  char v[AUTH_TOKEN_LEN + 1];
  std::string h = std::string("theme=dark; gwsid=") + tok + "; other=1";
  CHECK(authCookieValue(h.c_str(), v) && strcmp(v, tok) == 0, "token not found among other cookies");
  h = std::string("gwsid=") + tok;
  CHECK(authCookieValue(h.c_str(), v) && strcmp(v, tok) == 0, "token alone not found");
  CHECK(!authCookieValue("xgwsid=abc; gwsidx=def", v), "a cookie with a similar name was taken");
  CHECK(!authCookieValue("gwsid=short", v) && !authCookieValue("", v) && !authCookieValue(nullptr, v), "a bad cookie was taken");

  printf("\n%d checks passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
