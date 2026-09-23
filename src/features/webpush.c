#define _GNU_SOURCE
/* Web Push. See webpush.h for why this exists at all; this file is the mechanics.
 *
 * The shape of one message:
 *
 *   an ephemeral P-256 key pair is made for this message alone
 *   ECDH(ephemeral private, subscription public)                -> a shared secret
 *   HKDF over that, salted with the subscription's auth secret  -> a content key and a nonce
 *   AES-128-GCM with them                                       -> the ciphertext
 *   salt | record size | ephemeral public key | ciphertext      -> the body that is posted
 *
 * and a VAPID JWT, signed with this grill's long-lived key, goes in the Authorization header so
 * the push service knows the message came from whoever the subscription was issued to. */
#include "features/webpush.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "webpush"

#if !defined(PF_HAVE_WEBPUSH)

/* Built without the crypto. Everything is a no-op that admits it, so the app can say "not
 * available in this build" rather than offering a button that quietly does nothing. */
void pf_webpush_init(void) {}
void pf_webpush_shutdown(void) {}
bool pf_webpush_available(void) { return false; }
const char *pf_webpush_public_key(void) { return ""; }
int pf_webpush_subscribe(const cJSON *sub, char *err, size_t n) { (void)sub; snprintf(err, n, "this build has no web push support"); return -1; }
int pf_webpush_unsubscribe(const char *endpoint) { (void)endpoint; return -1; }
int pf_webpush_count(void) { return 0; }
void pf_webpush_send(const char *title, const char *body, const char *code, int crit) { (void)title; (void)body; (void)code; (void)crit; }
int pf_webpush_seal_for_test(const char *p256dh_b64, const char *auth_b64, const char *as_priv_b64,
                             const char *salt_b64, const char *plaintext, char *out_b64, size_t cap)
{
	(void)p256dh_b64; (void)auth_b64; (void)as_priv_b64; (void)salt_b64; (void)plaintext;
	if (cap) out_b64[0] = 0;
	return -1;
}
cJSON *pf_webpush_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "available", false);
	cJSON_AddNumberToObject(o, "devices", 0);
	return o;
}

#else

#include <curl/curl.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#define MAX_SUBS 8
#define P256_PUB_LEN 65      /* uncompressed point: 0x04 | X(32) | Y(32) */
#define AUTH_LEN 16

typedef struct {
	bool used;
	char endpoint[512];
	unsigned char ua_public[P256_PUB_LEN];
	unsigned char auth[AUTH_LEN];
} sub_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sub_t g_subs[MAX_SUBS];
static EVP_PKEY *g_vapid;            /* this grill's long-lived identity to the push service */
static char g_vapid_pub[128];        /* base64url, what the browser subscribes with */

/* ------------------------------------------------------------------ base64url */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_encode(const unsigned char *in, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned)in[i] << 16;
		if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
		if (i + 2 < n) v |= in[i + 2];
		int keep = n - i >= 3 ? 4 : (int)(n - i) + 1;   /* no padding, as the wire format wants */
		for (int k = 0; k < keep && o + 1 < cap; k++) out[o++] = B64[(v >> (18 - 6 * k)) & 0x3F];
	}
	if (o < cap) out[o] = 0;
	return o;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-' || c == '+') return 62;
	if (c == '_' || c == '/') return 63;
	return -1;
}

/* Accepts either alphabet and tolerates padding, because what a browser hands back is not always
 * the flavour the specification names. */
static size_t b64url_decode(const char *in, unsigned char *out, size_t cap)
{
	size_t o = 0;
	unsigned v = 0;
	int bits = 0;
	for (const char *p = in; *p; p++) {
		int d = b64_val(*p);
		if (d < 0) continue;
		v = (v << 6) | (unsigned)d;
		bits += 6;
		if (bits >= 8) { bits -= 8; if (o < cap) out[o++] = (unsigned char)((v >> bits) & 0xFF); }
	}
	return o;
}

/* ------------------------------------------------------------------ HKDF (RFC 5869) */

static void hmac_sha256(const unsigned char *key, size_t klen, const unsigned char *msg, size_t mlen, unsigned char out[32])
{
	unsigned int n = 32;
	HMAC(EVP_sha256(), key, (int)klen, msg, mlen, out, &n);
}

/* Expand to at most one block, which is all this protocol ever asks for. */
static void hkdf(const unsigned char *salt, size_t saltlen, const unsigned char *ikm, size_t ikmlen,
                 const unsigned char *info, size_t infolen, unsigned char *out, size_t outlen)
{
	unsigned char prk[32], t[32];
	hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
	unsigned char buf[256];
	size_t n = 0;
	if (infolen > sizeof buf - 1) infolen = sizeof buf - 1;
	memcpy(buf, info, infolen);
	n = infolen;
	buf[n++] = 0x01;
	hmac_sha256(prk, sizeof prk, buf, n, t);
	memcpy(out, t, outlen > 32 ? 32 : outlen);
	OPENSSL_cleanse(prk, sizeof prk);
	OPENSSL_cleanse(t, sizeof t);
}

/* ------------------------------------------------------------------ keys */

static EVP_PKEY *ec_generate(void)
{
	EVP_PKEY *k = NULL;
	EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
	if (!c) return NULL;
	if (EVP_PKEY_keygen_init(c) > 0 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c, NID_X9_62_prime256v1) > 0)
		EVP_PKEY_keygen(c, &k);
	EVP_PKEY_CTX_free(c);
	return k;
}

static int ec_public_raw(EVP_PKEY *k, unsigned char out[P256_PUB_LEN])
{
	size_t n = P256_PUB_LEN;
	return EVP_PKEY_get_octet_string_param(k, "encoded-pub-key", out, P256_PUB_LEN, &n) == 1 && n == P256_PUB_LEN ? 0 : -1;
}

static EVP_PKEY *ec_from_public_raw(const unsigned char *pub, size_t n)
{
	EVP_PKEY *k = NULL;
	EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_utf8_string("group", (char *)"prime256v1", 0),
		OSSL_PARAM_construct_octet_string("pub", (void *)pub, n),
		OSSL_PARAM_construct_end(),
	};
	if (c && EVP_PKEY_fromdata_init(c) > 0) EVP_PKEY_fromdata(c, &k, EVP_PKEY_PUBLIC_KEY, params);
	EVP_PKEY_CTX_free(c);
	return k;
}

/* The private scalar, so the identity survives a restart. A subscription is bound to this key: if
 * it changed, every phone already subscribed would be pushing into a void. */
static int ec_private_raw(EVP_PKEY *k, unsigned char out[32])
{
	BIGNUM *bn = NULL;
	if (EVP_PKEY_get_bn_param(k, "priv", &bn) != 1) return -1;
	int rc = BN_bn2binpad(bn, out, 32) == 32 ? 0 : -1;
	BN_free(bn);
	return rc;
}

static EVP_PKEY *ec_from_private_raw(const unsigned char priv[32])
{
	/* Derive the matching public point rather than storing it: one source of truth. */
	EVP_PKEY *k = NULL;
	BIGNUM *bn = BN_bin2bn(priv, 32, NULL);
	EC_GROUP *grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
	EC_POINT *pt = grp ? EC_POINT_new(grp) : NULL;
	unsigned char pub[P256_PUB_LEN];
	if (bn && grp && pt && EC_POINT_mul(grp, pt, bn, NULL, NULL, NULL) == 1 &&
	    EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED, pub, sizeof pub, NULL) == P256_PUB_LEN) {
		EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
		OSSL_PARAM params[] = {
			OSSL_PARAM_construct_utf8_string("group", (char *)"prime256v1", 0),
			OSSL_PARAM_construct_octet_string("pub", pub, sizeof pub),
			OSSL_PARAM_construct_BN("priv", (void *)priv, 32),
			OSSL_PARAM_construct_end(),
		};
		/* OSSL_PARAM_construct_BN wants native-endian; build the scalar the portable way instead */
		BIGNUM *pb = BN_bin2bn(priv, 32, NULL);
		int len = BN_num_bytes(pb);
		unsigned char *le = calloc(1, (size_t)len ? (size_t)len : 1);
		if (le) {
			BN_bn2nativepad(pb, le, len);
			params[2] = OSSL_PARAM_construct_BN("priv", le, (size_t)len);
			if (c && EVP_PKEY_fromdata_init(c) > 0) EVP_PKEY_fromdata(c, &k, EVP_PKEY_KEYPAIR, params);
			free(le);
		}
		BN_free(pb);
		EVP_PKEY_CTX_free(c);
	}
	EC_POINT_free(pt);
	EC_GROUP_free(grp);
	BN_free(bn);
	return k;
}

/* ------------------------------------------------------------------ persistence */

static void subs_save(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < MAX_SUBS; i++) {
		if (!g_subs[i].used) continue;
		char pub[128], auth[32];
		b64url_encode(g_subs[i].ua_public, P256_PUB_LEN, pub, sizeof pub);
		b64url_encode(g_subs[i].auth, AUTH_LEN, auth, sizeof auth);
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "endpoint", g_subs[i].endpoint);
		cJSON_AddStringToObject(e, "p256dh", pub);
		cJSON_AddStringToObject(e, "auth", auth);
		cJSON_AddItemToArray(arr, e);
	}
	char *txt = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (txt && pf_db_handle()) pf_db_kv_put("webpush", "subs", txt);
	free(txt);
}

static void subs_load(void)
{
	char buf[4096];
	if (pf_db_kv_get("webpush", "subs", buf, sizeof buf) != 0) return;
	cJSON *arr = cJSON_Parse(buf), *e;
	int i = 0;
	cJSON_ArrayForEach(e, arr) {
		if (i >= MAX_SUBS) break;
		const char *ep = pf_json_str(e, "endpoint", "");
		if (!ep[0]) continue;
		pf_strlcpy(g_subs[i].endpoint, ep, sizeof g_subs[i].endpoint);
		if (b64url_decode(pf_json_str(e, "p256dh", ""), g_subs[i].ua_public, P256_PUB_LEN) != P256_PUB_LEN) continue;
		if (b64url_decode(pf_json_str(e, "auth", ""), g_subs[i].auth, AUTH_LEN) != AUTH_LEN) continue;
		g_subs[i].used = true;
		i++;
	}
	cJSON_Delete(arr);
}

static void vapid_load_or_make(void)
{
	char buf[256];
	unsigned char priv[32];
	if (pf_db_kv_get("webpush", "vapid", buf, sizeof buf) == 0 && b64url_decode(buf, priv, 32) == 32)
		g_vapid = ec_from_private_raw(priv);
	if (!g_vapid) {
		g_vapid = ec_generate();
		if (g_vapid && ec_private_raw(g_vapid, priv) == 0) {
			char enc[64];
			b64url_encode(priv, 32, enc, sizeof enc);
			if (pf_db_handle()) pf_db_kv_put("webpush", "vapid", enc);
			LOGI(TAG, "generated this grill's push identity");
		}
	}
	OPENSSL_cleanse(priv, sizeof priv);
	unsigned char pub[P256_PUB_LEN];
	if (g_vapid && ec_public_raw(g_vapid, pub) == 0) b64url_encode(pub, sizeof pub, g_vapid_pub, sizeof g_vapid_pub);
}

/* ------------------------------------------------------------------ lifecycle */

void pf_webpush_init(void)
{
	pthread_mutex_lock(&g_mu);
	memset(g_subs, 0, sizeof g_subs);
	vapid_load_or_make();
	subs_load();
	int n = 0;
	for (int i = 0; i < MAX_SUBS; i++) if (g_subs[i].used) n++;
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "web push ready, %d subscribed device%s", n, n == 1 ? "" : "s");
}

void pf_webpush_shutdown(void)
{
	pthread_mutex_lock(&g_mu);
	if (g_vapid) { EVP_PKEY_free(g_vapid); g_vapid = NULL; }
	pthread_mutex_unlock(&g_mu);
}

bool pf_webpush_available(void) { return true; }
const char *pf_webpush_public_key(void) { return g_vapid_pub; }

int pf_webpush_count(void)
{
	int n = 0;
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < MAX_SUBS; i++) if (g_subs[i].used) n++;
	pthread_mutex_unlock(&g_mu);
	return n;
}

cJSON *pf_webpush_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "available", true);
	cJSON_AddStringToObject(o, "key", pf_webpush_public_key());
	cJSON_AddNumberToObject(o, "devices", pf_webpush_count());
	return o;
}

int pf_webpush_subscribe(const cJSON *sub, char *err, size_t n)
{
	const char *ep = pf_json_str((cJSON *)sub, "endpoint", "");
	const char *p256dh = pf_json_str((cJSON *)sub, "keys.p256dh", "");
	const char *auth = pf_json_str((cJSON *)sub, "keys.auth", "");
	if (!ep[0] || !p256dh[0] || !auth[0]) { snprintf(err, n, "that is not a push subscription"); return -1; }

	unsigned char pub[P256_PUB_LEN], a[AUTH_LEN];
	if (b64url_decode(p256dh, pub, sizeof pub) != P256_PUB_LEN) { snprintf(err, n, "the subscription key is the wrong size"); return -1; }
	if (b64url_decode(auth, a, sizeof a) != AUTH_LEN) { snprintf(err, n, "the subscription secret is the wrong size"); return -1; }

	pthread_mutex_lock(&g_mu);
	/* One record per endpoint: a browser refreshing its subscription gives the same endpoint back,
	 * and collecting duplicates would mean every notification arriving twice. */
	int slot = -1;
	for (int i = 0; i < MAX_SUBS; i++) if (g_subs[i].used && !strcmp(g_subs[i].endpoint, ep)) { slot = i; break; }
	if (slot < 0) for (int i = 0; i < MAX_SUBS; i++) if (!g_subs[i].used) { slot = i; break; }
	if (slot < 0) { pthread_mutex_unlock(&g_mu); snprintf(err, n, "no room for another device"); return -1; }
	pf_strlcpy(g_subs[slot].endpoint, ep, sizeof g_subs[slot].endpoint);
	memcpy(g_subs[slot].ua_public, pub, sizeof pub);
	memcpy(g_subs[slot].auth, a, sizeof a);
	g_subs[slot].used = true;
	subs_save();
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "a device subscribed to push");
	return 0;
}

int pf_webpush_unsubscribe(const char *endpoint)
{
	int rc = -1;
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < MAX_SUBS; i++)
		if (g_subs[i].used && !strcmp(g_subs[i].endpoint, endpoint)) { memset(&g_subs[i], 0, sizeof g_subs[i]); rc = 0; }
	if (rc == 0) subs_save();
	pthread_mutex_unlock(&g_mu);
	return rc;
}

/* ------------------------------------------------------------------ one message */

/* RFC 8291: encrypt `plain` for a subscription, producing the aes128gcm body to post. */
static int encrypt_record(const unsigned char *ua_public, const unsigned char *auth,
                          EVP_PKEY *eph, const unsigned char fixed_salt[16],
                          const unsigned char *plain, size_t plainlen,
                          unsigned char *out, size_t cap, size_t *outlen)
{
	int rc = -1;
	EVP_PKEY *peer = ec_from_public_raw(ua_public, P256_PUB_LEN);
	EVP_PKEY_CTX *dctx = NULL;
	EVP_CIPHER_CTX *cctx = NULL;
	unsigned char as_public[P256_PUB_LEN], shared[32], ikm[32], salt[16], cek[16], nonce[12];
	size_t sharedlen = sizeof shared;

	if (!eph || !peer) goto done;
	if (ec_public_raw(eph, as_public) != 0) goto done;
	if (!(dctx = EVP_PKEY_CTX_new(eph, NULL))) goto done;
	if (EVP_PKEY_derive_init(dctx) <= 0 || EVP_PKEY_derive_set_peer(dctx, peer) <= 0) goto done;
	if (EVP_PKEY_derive(dctx, shared, &sharedlen) <= 0 || sharedlen != 32) goto done;

	/* The auth secret salts the first extraction, which is what binds the message to this
	 * subscription and not merely to this key pair. */
	{
		unsigned char info[32 + 2 * P256_PUB_LEN];
		size_t n = 0;
		memcpy(info + n, "WebPush: info", 13); n += 13;
		info[n++] = 0x00;
		memcpy(info + n, ua_public, P256_PUB_LEN); n += P256_PUB_LEN;
		memcpy(info + n, as_public, P256_PUB_LEN); n += P256_PUB_LEN;
		hkdf(auth, AUTH_LEN, shared, sizeof shared, info, n, ikm, sizeof ikm);
	}
	if (fixed_salt) memcpy(salt, fixed_salt, sizeof salt);
	else if (RAND_bytes(salt, sizeof salt) != 1) goto done;
	hkdf(salt, sizeof salt, ikm, sizeof ikm, (const unsigned char *)"Content-Encoding: aes128gcm\0", 28, cek, sizeof cek);
	hkdf(salt, sizeof salt, ikm, sizeof ikm, (const unsigned char *)"Content-Encoding: nonce\0", 24, nonce, sizeof nonce);

	/* header: salt | record size | key length | ephemeral public key, then the sealed record */
	if (cap < 16 + 4 + 1 + P256_PUB_LEN + plainlen + 1 + 16) goto done;
	size_t o = 0;
	memcpy(out + o, salt, 16); o += 16;
	unsigned rs = 4096;
	out[o++] = (unsigned char)(rs >> 24); out[o++] = (unsigned char)(rs >> 16);
	out[o++] = (unsigned char)(rs >> 8);  out[o++] = (unsigned char)rs;
	out[o++] = P256_PUB_LEN;
	memcpy(out + o, as_public, P256_PUB_LEN); o += P256_PUB_LEN;

	/* a single record, so the padding delimiter is 0x02 -- "this is the last one" */
	unsigned char *padded = malloc(plainlen + 1);
	if (!padded) goto done;
	memcpy(padded, plain, plainlen);
	padded[plainlen] = 0x02;

	int len = 0, total = 0;
	if (!(cctx = EVP_CIPHER_CTX_new())) { free(padded); goto done; }
	if (EVP_EncryptInit_ex(cctx, EVP_aes_128_gcm(), NULL, NULL, NULL) == 1 &&
	    EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
	    EVP_EncryptInit_ex(cctx, NULL, NULL, cek, nonce) == 1 &&
	    EVP_EncryptUpdate(cctx, out + o, &len, padded, (int)plainlen + 1) == 1) {
		total = len;
		if (EVP_EncryptFinal_ex(cctx, out + o + total, &len) == 1) {
			total += len;
			unsigned char tag[16];
			if (EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1) {
				memcpy(out + o + total, tag, 16);
				*outlen = o + (size_t)total + 16;
				rc = 0;
			}
		}
	}
	free(padded);
done:
	if (cctx) EVP_CIPHER_CTX_free(cctx);
	if (dctx) EVP_PKEY_CTX_free(dctx);
	if (peer) EVP_PKEY_free(peer);
	OPENSSL_cleanse(shared, sizeof shared);
	OPENSSL_cleanse(ikm, sizeof ikm);
	OPENSSL_cleanse(cek, sizeof cek);
	return rc;
}

/* The ordinary path: a fresh ephemeral key and a fresh salt for every message. */
static int encrypt_for(const unsigned char *ua_public, const unsigned char *auth,
                       const unsigned char *plain, size_t plainlen,
                       unsigned char *out, size_t cap, size_t *outlen)
{
	EVP_PKEY *eph = ec_generate();
	if (!eph) return -1;
	int rc = encrypt_record(ua_public, auth, eph, NULL, plain, plainlen, out, cap, outlen);
	EVP_PKEY_free(eph);
	return rc;
}

/* Encryption is the one part of this that has to be exactly right and cannot be judged by looking
 * at it: a wrong byte gives a message the phone silently discards. RFC 8291 publishes a worked
 * example with fixed keys and a fixed salt, so this takes them and produces a body a test can
 * compare with the one in the specification. */
int pf_webpush_seal_for_test(const char *p256dh_b64, const char *auth_b64, const char *as_priv_b64,
                             const char *salt_b64, const char *plaintext, char *out_b64, size_t cap)
{
	unsigned char ua[P256_PUB_LEN], auth[AUTH_LEN], priv[32], salt[16], body[1024];
	if (b64url_decode(p256dh_b64, ua, sizeof ua) != P256_PUB_LEN) return -1;
	if (b64url_decode(auth_b64, auth, sizeof auth) != AUTH_LEN) return -1;
	if (b64url_decode(as_priv_b64, priv, sizeof priv) != 32) return -1;
	if (b64url_decode(salt_b64, salt, sizeof salt) != 16) return -1;
	EVP_PKEY *eph = ec_from_private_raw(priv);
	if (!eph) return -1;
	size_t len = 0;
	int rc = encrypt_record(ua, auth, eph, salt, (const unsigned char *)plaintext, strlen(plaintext), body, sizeof body, &len);
	EVP_PKEY_free(eph);
	if (rc != 0) return -1;
	b64url_encode(body, len, out_b64, cap);
	return 0;
}

/* RFC 8292: a short-lived JWT saying who is sending, signed with the grill's long-lived key. */
static int vapid_jwt(const char *endpoint, char *out, size_t cap)
{
	/* the audience is the push service's origin, not the whole endpoint */
	char aud[256] = "";
	const char *p = strstr(endpoint, "://");
	if (p) {
		const char *slash = strchr(p + 3, '/');
		size_t n = slash ? (size_t)(slash - endpoint) : strlen(endpoint);
		if (n >= sizeof aud) n = sizeof aud - 1;
		memcpy(aud, endpoint, n);
		aud[n] = 0;
	}
	if (!aud[0]) return -1;

	char claims[400], hdr_b64[64], claims_b64[600], signing[700];
	snprintf(claims, sizeof claims, "{\"aud\":\"%s\",\"exp\":%lld,\"sub\":\"mailto:pifire@localhost\"}",
	         aud, (long long)(pf_wall() + 12 * 3600));
	static const char hdr[] = "{\"typ\":\"JWT\",\"alg\":\"ES256\"}";
	b64url_encode((const unsigned char *)hdr, sizeof hdr - 1, hdr_b64, sizeof hdr_b64);
	b64url_encode((const unsigned char *)claims, strlen(claims), claims_b64, sizeof claims_b64);
	int sn = snprintf(signing, sizeof signing, "%s.%s", hdr_b64, claims_b64);
	if (sn < 0 || (size_t)sn >= sizeof signing) return -1;

	/* ES256 wants the raw r and s, 32 bytes each -- not the DER sequence OpenSSL signs into. */
	unsigned char der[128];
	size_t derlen = sizeof der;
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	int ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, g_vapid) == 1 &&
	         EVP_DigestSign(md, der, &derlen, (const unsigned char *)signing, (size_t)sn) == 1;
	EVP_MD_CTX_free(md);
	if (!ok) return -1;

	const unsigned char *dp = der;
	ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &dp, (long)derlen);
	if (!sig) return -1;
	unsigned char raw[64] = { 0 };
	const BIGNUM *r = NULL, *s = NULL;
	ECDSA_SIG_get0(sig, &r, &s);
	BN_bn2binpad(r, raw, 32);
	BN_bn2binpad(s, raw + 32, 32);
	ECDSA_SIG_free(sig);

	char sig_b64[128];
	b64url_encode(raw, sizeof raw, sig_b64, sizeof sig_b64);
	int n = snprintf(out, cap, "%s.%s", signing, sig_b64);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

static size_t sink_discard(void *p, size_t sz, size_t n, void *u) { (void)p; (void)u; return sz * n; }

/* Returns the HTTP status, or 0 if it could not be sent at all. */
static long post_one(const sub_t *s, const unsigned char *body, size_t len)
{
	char jwt[900];
	if (vapid_jwt(s->endpoint, jwt, sizeof jwt) != 0) return 0;

	CURL *c = curl_easy_init();
	if (!c) return 0;
	char auth[1100];
	snprintf(auth, sizeof auth, "Authorization: vapid t=%s, k=%s", jwt, g_vapid_pub);
	struct curl_slist *h = NULL;
	h = curl_slist_append(h, auth);
	h = curl_slist_append(h, "Content-Encoding: aes128gcm");
	h = curl_slist_append(h, "Content-Type: application/octet-stream");
	h = curl_slist_append(h, "TTL: 3600");
	h = curl_slist_append(h, "Urgency: normal");
	curl_easy_setopt(c, CURLOPT_URL, s->endpoint);
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)len);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_discard);
	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	else LOGW(TAG, "push failed: %s", curl_easy_strerror(rc));
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	return status;
}

void pf_webpush_send(const char *title, const char *body, const char *code, int crit)
{
	if (!g_vapid) return;

	char json[512];
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "title", title ? title : "PiFire");
	cJSON_AddStringToObject(o, "body", body ? body : "");
	cJSON_AddStringToObject(o, "code", code ? code : "");
	cJSON_AddNumberToObject(o, "crit", crit);
	char *txt = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (!txt) return;
	pf_strlcpy(json, txt, sizeof json);
	free(txt);

	/* Copy the table and let go of the lock: a push can take seconds, and nothing else should
	 * wait on the network to read the subscription list. */
	sub_t snap[MAX_SUBS];
	pthread_mutex_lock(&g_mu);
	memcpy(snap, g_subs, sizeof snap);
	pthread_mutex_unlock(&g_mu);

	for (int i = 0; i < MAX_SUBS; i++) {
		if (!snap[i].used) continue;
		unsigned char out[1024];
		size_t len = 0;
		if (encrypt_for(snap[i].ua_public, snap[i].auth, (const unsigned char *)json, strlen(json), out, sizeof out, &len) != 0) {
			LOGW(TAG, "could not encrypt for a subscription");
			continue;
		}
		long st = post_one(&snap[i], out, len);
		/* 404 and 410 are the push service saying the subscription is finished -- the app was
		 * deleted, or the browser threw it away. Keeping it means failing for ever. */
		if (st == 404 || st == 410) {
			LOGI(TAG, "a device's subscription has expired; forgetting it");
			pf_webpush_unsubscribe(snap[i].endpoint);
		} else if (st < 200 || st >= 300) {
			LOGW(TAG, "push rejected with %ld", st);
		}
	}
}

#endif
