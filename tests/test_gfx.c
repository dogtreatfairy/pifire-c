/* Renders the TFT screens from a canned status into PPM files so the layout can be inspected. */
#include "display/gfx.h"
#include "display/screens.h"
#include "unity.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static const char *status_json =
"{\"units\":\"F\",\"mode\":\"Hold\",\"setpoint\":225,\"mode_elapsed\":812,\"s_plus\":false,\"lid_open\":false,\"hopper_pct\":62,"
"\"outputs\":{\"power\":true,\"fan\":true,\"auger\":true,\"igniter\":false,\"fan_pct\":100},"
"\"cycle\":{\"u_raw\":0.41,\"u_applied\":0.41},\"timers\":{\"startup_duration\":240,\"shutdown_duration\":240},"
"\"safety\":{\"error_code\":\"\",\"error_msg\":\"\"},\"coldstart\":{\"active\":false,\"reached\":false,\"remaining\":0},"
"\"probes\":[{\"label\":\"Grill\",\"name\":\"Grill\",\"role\":\"Primary\",\"enabled\":true,\"valid\":true,\"temp\":227,\"target\":0},"
"{\"label\":\"Probe1\",\"name\":\"Brisket\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":164,\"target\":203},"
"{\"label\":\"Probe2\",\"name\":\"Ribs\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":195,\"target\":195},"
"{\"label\":\"Probe3\",\"name\":\"Probe-3\",\"role\":\"Food\",\"enabled\":true,\"valid\":false,\"temp\":null,\"target\":0}]}";

static void test_render_screens(void)
{
	pf_gfx g;
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_init(&g, 320, 240));
	cJSON *st = cJSON_Parse(status_json);
	TEST_ASSERT_NOT_NULL(st);
	pf_ui_state ui = { .screen = PF_SCR_MAIN };
	pf_screens_render(&g, st, &ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(&g, "/tmp/pf_screen_main.ppm"));
	ui.screen = PF_SCR_MENU; ui.menu_index = 1;
	pf_screens_render(&g, st, &ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(&g, "/tmp/pf_screen_menu.ppm"));
	ui.screen = PF_SCR_SETPOINT; ui.edit_setpoint = 250;
	pf_screens_render(&g, st, &ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(&g, "/tmp/pf_screen_setpoint.ppm"));
	/* startup with a countdown, light (outdoor) theme */
	pf_gfx_set_theme(&g, "light");
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Startup"));
	cJSON_AddNumberToObject(cJSON_GetObjectItem(st, "timers"), "mode_remaining", 187);
	cJSON_AddNumberToObject(cJSON_GetObjectItem(st, "timers"), "startup_exit_temp", 120);
	ui.screen = PF_SCR_MAIN;
	pf_screens_render(&g, st, &ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(&g, "/tmp/pf_screen_startup_light.ppm"));
	pf_gfx_free(&g);
	/* portrait */
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_init(&g, 240, 320));
	pf_screens_render(&g, st, &ui);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_write_ppm(&g, "/tmp/pf_screen_portrait.ppm"));
	cJSON_Delete(st);
	pf_gfx_free(&g);
}

static void test_text_metrics(void)
{
	TEST_ASSERT_EQUAL_INT(8 * 3 * 4, pf_gfx_text_width("225F", 3));
	pf_gfx g;
	pf_gfx_init(&g, 64, 16);
	pf_gfx_clear(&g, g.th.bg);
	pf_gfx_text(&g, 0, 0, "A", 1, g.th.text);
	int lit = 0;
	for (int i = 0; i < 64 * 16; i++) if (g.px[i] != g.px[63]) lit++;
	TEST_ASSERT_TRUE(lit > 10 && lit < 40);
	pf_gfx_free(&g);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_render_screens);
	RUN_TEST(test_text_metrics);
	return UNITY_END();
}
