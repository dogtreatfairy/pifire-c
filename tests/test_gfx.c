/* Renders the TFT screens from canned status into PPM files so the layout can be inspected. */
#include "display/gfx.h"
#include "display/screens.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *status_json =
"{\"units\":\"F\",\"mode\":\"Hold\",\"next_mode\":\"Hold\",\"setpoint\":225,\"mode_elapsed\":812,\"cook_elapsed\":5023,\"s_plus\":false,\"lid_open\":false,\"hopper_pct\":62,"
"\"outputs\":{\"power\":true,\"fan\":true,\"auger\":true,\"igniter\":false,\"fan_pct\":100},"
"\"cycle\":{\"u_raw\":0.41,\"u_applied\":0.41},\"timers\":{\"startup_duration\":240,\"shutdown_duration\":240,\"mode_remaining\":0,\"startup_exit_temp\":0,\"prime_amount\":0},"
"\"timer\":{\"running\":false,\"remaining\":0},\"coldstart\":{\"active\":false,\"reached\":false,\"remaining\":0},"
"\"safety\":{\"error_code\":\"\",\"error_msg\":\"\"},"
"\"probes\":[{\"label\":\"Grill\",\"name\":\"Grill\",\"role\":\"Primary\",\"enabled\":true,\"valid\":true,\"temp\":227,\"target\":0},"
"{\"label\":\"Probe1\",\"name\":\"Probe 1\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":164,\"target\":203},"
"{\"label\":\"Probe2\",\"name\":\"Probe 2\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":195,\"target\":195},"
"{\"label\":\"Probe3\",\"name\":\"Probe 3\",\"role\":\"Food\",\"enabled\":true,\"valid\":false,\"temp\":null,\"target\":0}]}";

static void render_to(pf_gfx *g, cJSON *st, pf_ui_state *ui, const char *name)
{
	char path[64];
	snprintf(path, sizeof path, "/tmp/pf_screen_%s.ppm", name);
	pf_screens_render(g, st, ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(g, path));
}

static void set_mode(cJSON *st, const char *mode, double remaining)
{
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString(mode));
	cJSON_ReplaceItemInObject(cJSON_GetObjectItem(st, "timers"), "mode_remaining", cJSON_CreateNumber(remaining));
}

static void test_render_screens(void)
{
	pf_gfx g;
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_init(&g, 320, 240));
	cJSON *st = cJSON_Parse(status_json);
	TEST_ASSERT_NOT_NULL(st);
	pf_ui_state ui = { .screen = PF_SCR_MAIN };
	render_to(&g, st, &ui, "hold3");
	ui.screen = PF_SCR_MENU; ui.menu_index = 1;
	render_to(&g, st, &ui, "menu");
	ui.screen = PF_SCR_SETPOINT; ui.edit_setpoint = 250; ui.edit_is_change = true;
	render_to(&g, st, &ui, "setpoint");
	ui.screen = PF_SCR_MAIN;
	set_mode(st, "Startup", 187);
	render_to(&g, st, &ui, "startup3");
	/* fewer food probes: drop the last entries */
	cJSON *probes = cJSON_GetObjectItem(st, "probes");
	cJSON_DeleteItemFromArray(probes, 3);
	set_mode(st, "Smoke", 0);
	cJSON_ReplaceItemInObject(st, "s_plus", cJSON_CreateTrue());
	render_to(&g, st, &ui, "smoke2");
	cJSON_DeleteItemFromArray(probes, 2);
	set_mode(st, "Shutdown", 221);
	render_to(&g, st, &ui, "shutdown1");
	cJSON_DeleteItemFromArray(probes, 1);
	set_mode(st, "Hold", 0);
	pf_gfx_set_theme(&g, "light");
	render_to(&g, st, &ui, "hold0_light");
	ui.screen = PF_SCR_MENU; ui.menu_index = 0;
	render_to(&g, st, &ui, "menu_light");
	pf_gfx_free(&g);
	/* portrait, three probes again */
	cJSON_Delete(st);
	st = cJSON_Parse(status_json);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_init(&g, 240, 320));
	ui.screen = PF_SCR_MAIN;
	render_to(&g, st, &ui, "portrait");
	cJSON_Delete(st);
	pf_gfx_free(&g);
}

static void test_menu_by_mode(void)
{
	pf_menu_item items[PF_MENU_MAX];
	cJSON *st = cJSON_Parse("{\"mode\":\"Smoke\",\"s_plus\":false}");
	int n = pf_menu_build(st, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_INT(PF_MI_HOLD, items[0].id);
	TEST_ASSERT_EQUAL_INT(PF_MI_SMOKE_PLUS, items[1].id);
	TEST_ASSERT_EQUAL_STRING("Smoke+ on", items[1].label);
	cJSON_Delete(st);
	st = cJSON_Parse("{\"mode\":\"Hold\"}");
	n = pf_menu_build(st, items, PF_MENU_MAX);
	for (int i = 0; i < n; i++) TEST_ASSERT_NOT_EQUAL(PF_MI_SMOKE_PLUS, items[i].id);  /* Smoke+ only in Smoke */
	TEST_ASSERT_EQUAL_INT(PF_MI_HOLD, items[0].id);
	cJSON_Delete(st);
}

static void test_text_metrics(void)
{
	int w1 = pf_gfx_text_width(PF_FONT_SEMIBOLD, 20, "225"), w2 = pf_gfx_text_width(PF_FONT_SEMIBOLD, 40, "225");
	TEST_ASSERT_TRUE(w1 > 20 && w2 > w1 * 1.8 && w2 < w1 * 2.2);
	pf_gfx g;
	pf_gfx_init(&g, 64, 32);
	pf_gfx_clear(&g, g.th.bg);
	pf_gfx_text(&g, PF_FONT_REGULAR, 20, 2, 2, "A", g.th.text);
	int lit = 0, partial = 0;
	for (int i = 0; i < 64 * 32; i++) { if (g.px[i] != g.px[64 * 32 - 1]) lit++; if (g.px[i] != g.px[64 * 32 - 1] && g.px[i] != 0xFFFF) partial++; }
	TEST_ASSERT_TRUE(lit > 30);
	TEST_ASSERT_TRUE(partial > 5);  /* anti-aliased edges */
	pf_gfx_free(&g);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_render_screens);
	RUN_TEST(test_menu_by_mode);
	RUN_TEST(test_text_metrics);
	return UNITY_END();
}
