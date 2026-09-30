/* Cloudflare Access tokens, as the grill checks them before letting a request through the tunnel.
 *
 * This is the only thing standing between the public internet and a grill whose API has no login,
 * so every way a token can be wrong is tried here against a real RS256 signature made with a key
 * generated for the test: the right token passes, and the wrong team, the wrong application, an
 * expired session, a changed payload, another key's signature and the "none" algorithm do not. */
#include "net/cloudflare.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void setUp(void) {}
void tearDown(void) {}

#if defined(PF_HAVE_WEBPUSH)
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

static EVP_PKEY *g_key, *g_other;
static char g_pem[4096];
static const char TEAM[] = "smokehouse";
static const char AUD[] = "4714c1358e65fe4b408ad6d432a5f878f08194bdb4752441fd56faefa9b2b6f2";
static double g_now;

static EVP_PKEY *rsa_key(void)
{
	EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	EVP_PKEY *k = NULL;
	EVP_PKEY_keygen_init(c);
	EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048);
	EVP_PKEY_keygen(c, &k);
	EVP_PKEY_CTX_free(c);
	return k;
}

static void b64url(const unsigned char *in, size_t n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t w = 0, i = 0;
	for (; i + 2 < n; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
		out[w++] = t[v >> 18 & 63]; out[w++] = t[v >> 12 & 63]; out[w++] = t[v >> 6 & 63]; out[w++] = t[v & 63];
	}
	if (n - i == 1) { unsigned v = (unsigned)in[i] << 16; out[w++] = t[v >> 18 & 63]; out[w++] = t[v >> 12 & 63]; }
	else if (n - i == 2) { unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8; out[w++] = t[v >> 18 & 63]; out[w++] = t[v >> 12 & 63]; out[w++] = t[v >> 6 & 63]; }
	out[w] = 0;
}

/* header.payload.signature, signed by `k` */
static void make(char *out, const char *alg, const char *payload, EVP_PKEY *k)
{
	char hdr[128], h64[256], p64[1024];
	snprintf(hdr, sizeof hdr, "{\"alg\":\"%s\",\"kid\":\"k1\",\"typ\":\"JWT\"}", alg);
	b64url((const unsigned char *)hdr, strlen(hdr), h64);
	b64url((const unsigned char *)payload, strlen(payload), p64);
	sprintf(out, "%s.%s", h64, p64);
	unsigned char sig[512];
	size_t sl = sizeof sig;
	EVP_MD_CTX *m = EVP_MD_CTX_new();
	EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, k);
	EVP_DigestSign(m, sig, &sl, (const unsigned char *)out, strlen(out));
	EVP_MD_CTX_free(m);
	char s64[1024];
	b64url(sig, sl, s64);
	strcat(out, ".");
	strcat(out, s64);
}

static void payload(char *out, size_t n, const char *team, const char *aud, double exp)
{
	snprintf(out, n, "{\"aud\":[\"%s\"],\"email\":\"cook@example.com\",\"exp\":%.0f,\"iat\":%.0f,\"iss\":\"https://%s.cloudflareaccess.com\",\"sub\":\"x\"}",
	         aud, exp, g_now - 10, team);
}

static int check(const char *jwt)
{
	char why[256];
	return pf_cloudflare_check_with_pem(jwt, g_pem, TEAM, AUD, g_now, why, sizeof why);
}

static void test_a_valid_token_passes(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, TEAM, AUD, g_now + 3600);
	make(jwt, "RS256", p, g_key);
	TEST_ASSERT_EQUAL_INT(0, check(jwt));
}

static void test_another_team_is_refused(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, "someoneelse", AUD, g_now + 3600);
	make(jwt, "RS256", p, g_key);
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
}

static void test_another_application_is_refused(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, TEAM, "0000000000000000000000000000000000000000000000000000000000000000", g_now + 3600);
	make(jwt, "RS256", p, g_key);
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
}

static void test_an_expired_session_is_refused(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, TEAM, AUD, g_now - 3600);
	make(jwt, "RS256", p, g_key);
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
}

static void test_another_key_is_refused(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, TEAM, AUD, g_now + 3600);
	make(jwt, "RS256", p, g_other);
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
}

static void test_a_changed_payload_is_refused(void)
{
	char p[512], jwt[2048], forged[2048];
	payload(p, sizeof p, TEAM, AUD, g_now + 3600);
	make(jwt, "RS256", p, g_key);
	/* the signature from one payload on another: a later expiry */
	char p2[512], h64[256], p64[1024];
	payload(p2, sizeof p2, TEAM, AUD, g_now + 999999);
	const char *d1 = strchr(jwt, '.'), *d2 = strchr(d1 + 1, '.');
	snprintf(h64, sizeof h64, "%.*s", (int)(d1 - jwt), jwt);
	b64url((const unsigned char *)p2, strlen(p2), p64);
	snprintf(forged, sizeof forged, "%s.%s%s", h64, p64, d2);
	TEST_ASSERT_EQUAL_INT(-1, check(forged));
}

static void test_the_none_algorithm_is_refused(void)
{
	char p[512], jwt[2048];
	payload(p, sizeof p, TEAM, AUD, g_now + 3600);
	make(jwt, "none", p, g_key);
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
	*strrchr(jwt, '.') = 0;
	strcat(jwt, ".");
	TEST_ASSERT_EQUAL_INT(-1, check(jwt));
}

static void test_garbage_is_refused(void)
{
	TEST_ASSERT_EQUAL_INT(-1, check(""));
	TEST_ASSERT_EQUAL_INT(-1, check("a.b"));
	TEST_ASSERT_EQUAL_INT(-1, check("a.b.c.d"));
	TEST_ASSERT_EQUAL_INT(-1, check("!!!.???.***"));
}

int main(void)
{
	g_now = (double)time(NULL);
	g_key = rsa_key();
	g_other = rsa_key();
	BIO *b = BIO_new(BIO_s_mem());
	PEM_write_bio_PUBKEY(b, g_key);
	int l = BIO_read(b, g_pem, sizeof g_pem - 1);
	g_pem[l > 0 ? l : 0] = 0;
	BIO_free(b);
	UNITY_BEGIN();
	RUN_TEST(test_a_valid_token_passes);
	RUN_TEST(test_another_team_is_refused);
	RUN_TEST(test_another_application_is_refused);
	RUN_TEST(test_an_expired_session_is_refused);
	RUN_TEST(test_another_key_is_refused);
	RUN_TEST(test_a_changed_payload_is_refused);
	RUN_TEST(test_the_none_algorithm_is_refused);
	RUN_TEST(test_garbage_is_refused);
	EVP_PKEY_free(g_key);
	EVP_PKEY_free(g_other);
	return UNITY_END();
}

#else

static void test_nothing_passes_without_crypto(void)
{
	char why[128];
	TEST_ASSERT_EQUAL_INT(-1, pf_cloudflare_check("a.b.c", "grill.example.com", why, sizeof why));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_nothing_passes_without_crypto);
	return UNITY_END();
}

#endif
