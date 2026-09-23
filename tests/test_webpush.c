/* Web push encryption, checked against the worked example in RFC 8291 section 5.
 *
 * This is the one part of the feature that cannot be judged by reading it or by looking at the
 * result: a single wrong byte produces a message the phone discards in silence, with no error
 * anywhere. The specification publishes the keys, the salt, the plaintext and the exact body they
 * must produce, so the encryption is compared with that rather than taken on trust. */
#include "features/webpush.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* RFC 8291, section 5 */
static const char PLAINTEXT[]   = "When I grow up, I want to be a watermelon";
static const char UA_PUBLIC[]   = "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
static const char UA_AUTH[]     = "BTBZMqHH6r4Tts7J_aSIgg";
static const char AS_PRIVATE[]  = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
static const char SALT[]        = "DGv6ra1nlYgDCS1FRnbzlw";
static const char EXPECTED[]    =
	"DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml"
	"mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPT"
	"pK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";

static void test_the_encryption_matches_the_specification(void)
{
	if (!pf_webpush_available()) { TEST_IGNORE_MESSAGE("this build has no web push support"); return; }
	char out[1024] = "";
	int rc = pf_webpush_seal_for_test(UA_PUBLIC, UA_AUTH, AS_PRIVATE, SALT, PLAINTEXT, out, sizeof out);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "sealing the example message should succeed");
	printf("produced: %.64s...\n", out);
	printf("expected: %.64s...\n", EXPECTED);
	/* The header carries the salt, the record size, and the ephemeral public key derived from the
	   private key the specification gives, so a mismatch anywhere shows up here. */
	TEST_ASSERT_EQUAL_STRING_MESSAGE(EXPECTED, out, "the sealed body must match RFC 8291 exactly");
}

/* A subscription that is not one is refused rather than stored half-formed: a record with the
   wrong key length would fail on every send afterwards, with nothing to say why. */
static void test_a_malformed_subscription_is_refused(void)
{
	if (!pf_webpush_available()) { TEST_IGNORE_MESSAGE("this build has no web push support"); return; }
	char err[160];
	cJSON *bad = cJSON_Parse("{\"endpoint\":\"https://example.com/x\",\"keys\":{\"p256dh\":\"aGk\",\"auth\":\"aGk\"}}");
	TEST_ASSERT_EQUAL_INT(-1, pf_webpush_subscribe(bad, err, sizeof err));
	printf("refused: %s\n", err);
	cJSON_Delete(bad);

	cJSON *none = cJSON_Parse("{\"endpoint\":\"\"}");
	TEST_ASSERT_EQUAL_INT(-1, pf_webpush_subscribe(none, err, sizeof err));
	cJSON_Delete(none);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_the_encryption_matches_the_specification);
	RUN_TEST(test_a_malformed_subscription_is_refused);
	return UNITY_END();
}
