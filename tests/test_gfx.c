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
"\"net\":{\"ip\":\"10.0.0.5\",\"ssid\":\"Kitchen\",\"signal\":72,\"port\":80},"
"\"probes\":[{\"label\":\"Grill\",\"name\":\"Grill\",\"role\":\"Primary\",\"enabled\":true,\"valid\":true,\"temp\":227,\"target\":0},"
"{\"label\":\"Probe1\",\"name\":\"Probe 1\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":164,\"target\":203,\"eta_s\":4920,\"wireless\":true,\"rssi\":-67,\"signal\":3,\"battery\":80,\"ambient\":221,\"ambient_label\":\"Chef1Amb\"},"
"{\"label\":\"Chef1Amb\",\"name\":\"Chef iQ 1 Ambient\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":221,\"target\":0,\"wireless\":true,\"companion\":true},"
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
	g.vw = 320 - 16;   /* default right margin */
	cJSON *st = cJSON_Parse(status_json);
	TEST_ASSERT_NOT_NULL(st);
	pf_ui_state ui = { 0 };
	render_to(&g, st, &ui, "hold3");
	/* flash phases: probe 1 pushed 6 F over target (orange), probe 3 12 F over (red), probe 2 done (green) */
	cJSON *pr = cJSON_GetObjectItem(st, "probes");
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 1), "temp", cJSON_CreateNumber(209));
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 4), "temp", cJSON_CreateNumber(172));
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 4), "target", cJSON_CreateNumber(160));
	render_to(&g, st, &ui, "flash_on");
	ui.blink = true;
	render_to(&g, st, &ui, "flash_off");
	ui.blink = false;
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 1), "temp", cJSON_CreateNumber(164));
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 4), "temp", cJSON_CreateNull());
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(pr, 4), "target", cJSON_CreateNumber(0));

	/* the active menu, and the screens it leads to */
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_ROOT);
	pf_nav_top(&ui)->index = 1;
	render_to(&g, st, &ui, "menu_active");
	pf_nav_reset(&ui);
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_PROBE);
	render_to(&g, st, &ui, "menu_probe");
	pf_nav_reset(&ui);
	pf_nav_push(&ui, PF_SCR_NETINFO, 0);
	render_to(&g, st, &ui, "netinfo");
	pf_nav_reset(&ui);
	ui.temp_value = 250; ui.temp_focus = 1;
	snprintf(ui.temp_title, sizeof ui.temp_title, "%s", "STARTUP TO HOLD");
	snprintf(ui.temp_button, sizeof ui.temp_button, "%s", "Startup");
	pf_nav_push(&ui, PF_SCR_TEMP, 0);
	render_to(&g, st, &ui, "tempsel");
	ui.temp_focus = 0; ui.temp_editing = true; ui.blink = true;
	render_to(&g, st, &ui, "tempsel_edit");
	ui.temp_editing = false; ui.blink = false;
	pf_nav_reset(&ui);
	snprintf(ui.confirm_text, sizeof ui.confirm_text, "%s", "Emergency Stop?");
	snprintf(ui.confirm_yes, sizeof ui.confirm_yes, "%s", "Stop");
	ui.confirm_danger = true; ui.confirm_focus = 1;
	pf_nav_push(&ui, PF_SCR_CONFIRM, 0);
	render_to(&g, st, &ui, "confirm");
	pf_nav_reset(&ui);
	ui.manual_focus = 1;
	pf_nav_push(&ui, PF_SCR_MANUAL, 0);
	render_to(&g, st, &ui, "manual");
	pf_nav_reset(&ui);
	snprintf(ui.bt_label, sizeof ui.bt_label, "%s", "Chef iQ");
	ui.bt_n = 2;
	snprintf(ui.bt[0].name, sizeof ui.bt[0].name, "%s", "CQ60");
	ui.bt[0].bars = 3;
	snprintf(ui.bt[1].name, sizeof ui.bt[1].name, "%s", "CQ60");
	ui.bt[1].bars = 1;
	pf_nav_push(&ui, PF_SCR_BTSCAN, 0);
	render_to(&g, st, &ui, "btscan");
	pf_nav_reset(&ui);

	set_mode(st, "Startup", 187);
	render_to(&g, st, &ui, "startup3");
	/* fewer food probes: drop the last entries */
	cJSON *probes = cJSON_GetObjectItem(st, "probes");
	cJSON_DeleteItemFromArray(probes, 4);
	set_mode(st, "Smoke", 0);
	cJSON_ReplaceItemInObject(st, "s_plus", cJSON_CreateTrue());
	render_to(&g, st, &ui, "smoke2");
	cJSON_DeleteItemFromArray(probes, 3);
	set_mode(st, "Shutdown", 221);
	render_to(&g, st, &ui, "shutdown1");
	cJSON_DeleteItemFromArray(probes, 2);
	cJSON_DeleteItemFromArray(probes, 1);
	set_mode(st, "Stop", 0);
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_ROOT);
	render_to(&g, st, &ui, "menu_stopped");
	pf_nav_reset(&ui);
	set_mode(st, "Hold", 0);
	pf_gfx_set_theme(&g, "light");
	render_to(&g, st, &ui, "hold0_light");
	pf_gfx_free(&g);
	/* portrait, three probes again */
	cJSON_Delete(st);
	st = cJSON_Parse(status_json);
	TEST_ASSERT_EQUAL_INT(0, pf_gfx_init(&g, 240, 320));
	g.vw = 240 - 16;
	render_to(&g, st, &ui, "portrait");
	cJSON_Delete(st);
	pf_gfx_free(&g);
}

/* the menus name modes and actions, and their shape follows the running mode */
static void test_menus_by_mode(void)
{
	pf_menu_item items[PF_MENU_MAX];
	pf_ui_state ui = { 0 };
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_ROOT);

	cJSON *st = cJSON_Parse("{\"mode\":\"Stop\"}");
	int n = pf_menu_build(st, &ui, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_STRING("Startup", items[0].label);
	TEST_ASSERT_EQUAL_STRING("Monitor", items[1].label);
	TEST_ASSERT_EQUAL_STRING("Network Info", items[2].label);
	TEST_ASSERT_EQUAL_STRING("Power", items[3].label);
	TEST_ASSERT_EQUAL_STRING("Back", items[4].label);
	cJSON_Delete(st);

	/* the active menu offers the opposite mode, ending the cook and an emergency stop */
	st = cJSON_Parse("{\"mode\":\"Hold\"}");
	n = pf_menu_build(st, &ui, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_STRING("Smoke Mode", items[0].label);
	TEST_ASSERT_EQUAL_STRING("End Cook", items[1].label);
	TEST_ASSERT_EQUAL_STRING("Emergency Stop", items[n - 2].label);
	TEST_ASSERT_TRUE(items[n - 2].danger);
	cJSON_Delete(st);
	st = cJSON_Parse("{\"mode\":\"Smoke\"}");
	n = pf_menu_build(st, &ui, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_STRING("Hold Mode", items[0].label);
	cJSON_Delete(st);

	st = cJSON_Parse("{\"mode\":\"Monitor\"}");
	n = pf_menu_build(st, &ui, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_STRING("Control", items[0].label);
	TEST_ASSERT_EQUAL_STRING("Startup", items[1].label);
	TEST_ASSERT_EQUAL_STRING("Stop", items[2].label);
	cJSON_Delete(st);

	/* the startup menu picks the mode startup runs into */
	pf_nav_reset(&ui);
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_STARTUP);
	st = cJSON_Parse("{\"mode\":\"Stop\"}");
	n = pf_menu_build(st, &ui, items, PF_MENU_MAX);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_INT(PF_ACT_STARTUP_HOLD, items[0].act);
	TEST_ASSERT_EQUAL_INT(PF_ACT_STARTUP_SMOKE, items[1].act);
	TEST_ASSERT_EQUAL_INT(PF_ACT_BACK, items[2].act);
	cJSON_Delete(st);
}

/* the stack unwinds one screen at a time, and a long press returns to the grill */
static void test_navigation_stack(void)
{
	pf_ui_state ui = { 0 };
	TEST_ASSERT_EQUAL_INT(PF_SCR_MAIN, pf_nav_screen(&ui));
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_ROOT);
	pf_nav_push(&ui, PF_SCR_LIST, PF_LIST_STARTUP);
	pf_nav_push(&ui, PF_SCR_TEMP, 0);
	TEST_ASSERT_EQUAL_INT(PF_SCR_TEMP, pf_nav_screen(&ui));
	pf_nav_pop(&ui);
	TEST_ASSERT_EQUAL_INT(PF_LIST_STARTUP, pf_nav_top(&ui)->list);
	pf_nav_pop(&ui);
	TEST_ASSERT_EQUAL_INT(PF_LIST_ROOT, pf_nav_top(&ui)->list);
	pf_nav_reset(&ui);
	TEST_ASSERT_EQUAL_INT(PF_SCR_MAIN, pf_nav_screen(&ui));
	pf_nav_pop(&ui);   /* popping past the root is harmless */
	TEST_ASSERT_EQUAL_INT(PF_SCR_MAIN, pf_nav_screen(&ui));
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
	RUN_TEST(test_menus_by_mode);
	RUN_TEST(test_navigation_stack);
	RUN_TEST(test_text_metrics);
	return UNITY_END();
}
