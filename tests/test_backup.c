/* The backup round trip: what goes into the archive, that it can be read back, and that a staged
 * restore lands in place before the daemon opens anything. */
#include "core/db.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/backup.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[256], cfg[300], db[300];

void setUp(void)
{
	snprintf(dir, sizeof dir, "/tmp/pf_bak_%d", (int)getpid());
	char rm[300]; snprintf(rm, sizeof rm, "rm -rf %s", dir); if (system(rm)) { }
	pf_mkdir_p(dir);
	snprintf(cfg, sizeof cfg, "%s/settings.json", dir);
	snprintf(db, sizeof db, "%s/pifire.db", dir);
	pf_settings_init(cfg);
	pf_db_open(db);
	pf_backup_init(dir, cfg, true);
}

void tearDown(void)
{
	pf_db_close();
	char rm[300]; snprintf(rm, sizeof rm, "rm -rf %s", dir); if (system(rm)) { }
}

static void write_cook(int id, const char *body)
{
	char cdir[300], path[400];
	snprintf(cdir, sizeof cdir, "%s/cookfiles", dir); pf_mkdir_p(cdir);
	snprintf(path, sizeof path, "%s/cook-%d.json", cdir, id);
	pf_write_file_atomic(path, body, strlen(body));
}

static void test_archive_holds_the_picture_and_leaves_the_chart_out(void)
{
	pf_set_put_str("globals.grill_name", "Back Porch");
	pf_db_kv_put("learning", "anchors", "[{\"setpoint_c\":121,\"PB\":35}]");
	pf_db_exec("INSERT INTO history(ts,mode,setpoint,u_raw,u_applied,fan_pct,outputs) VALUES (1,6,121,0.3,0.3,100,1)");
	write_cook(1, "{\"name\":\"ribs\"}");

	char out[300], err[200] = "";
	snprintf(out, sizeof out, "%s/out.tar.gz", dir);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_make(out, err, sizeof err), err);
	struct stat st;
	TEST_ASSERT_EQUAL_INT(0, stat(out, &st));
	TEST_ASSERT_TRUE(st.st_size > 200);

	char stage[300];
	snprintf(stage, sizeof stage, "%s/unpacked", dir);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_stage(out, stage, err, sizeof err), err);
	char p[400];
	snprintf(p, sizeof p, "%s/settings.json", stage); TEST_ASSERT_TRUE(pf_file_exists(p));
	snprintf(p, sizeof p, "%s/cookfiles/cook-1.json", stage); TEST_ASSERT_TRUE(pf_file_exists(p));
	snprintf(p, sizeof p, "%s/manifest.json", stage);
	char *m = pf_read_file(p, NULL);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_NOT_NULL(strstr(m, "\"Back Porch\""));
	free(m);
	/* the copy carries the learning and not the rolling chart */
	snprintf(p, sizeof p, "%s/pifire.db", stage);
	char q[600];
	snprintf(q, sizeof q, "sqlite3 %s \"SELECT (SELECT count(*) FROM history) || ',' || (SELECT count(*) FROM kv WHERE ns='learning')\"", p);
	FILE *f = popen(q, "r");
	char line[64] = "";
	if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); }
	if (line[0]) TEST_ASSERT_EQUAL_STRING("0,1\n", line);   /* only when the sqlite3 CLI is about */
}

static void test_a_garbage_file_is_refused(void)
{
	char bad[300], err[200] = "", stage[300];
	snprintf(bad, sizeof bad, "%s/bad.tar.gz", dir);
	pf_write_file_atomic(bad, "not an archive", 14);
	snprintf(stage, sizeof stage, "%s/unpacked", dir);
	TEST_ASSERT_NOT_EQUAL(0, pf_backup_stage(bad, stage, err, sizeof err));
	TEST_ASSERT_TRUE(err[0] != 0);
	/* a real tar.gz that is not a PiFire backup */
	char other[300], cmd[900];
	snprintf(other, sizeof other, "%s/other", dir); pf_mkdir_p(other);
	snprintf(cmd, sizeof cmd, "%s/x.txt", other); pf_write_file_atomic(cmd, "x", 1);
	snprintf(bad, sizeof bad, "%s/other.tar.gz", dir);
	snprintf(cmd, sizeof cmd, "tar -C %s -czf %s .", other, bad); if (system(cmd)) { }
	TEST_ASSERT_NOT_EQUAL(0, pf_backup_stage(bad, stage, err, sizeof err));
	TEST_ASSERT_NOT_NULL(strstr(err, "manifest"));
}

static void test_a_staged_restore_lands_before_the_database_opens(void)
{
	/* the grill as it was */
	pf_set_put_str("globals.grill_name", "Before");
	pf_db_kv_put("learning", "anchors", "[1]");
	write_cook(7, "{\"name\":\"old\"}");
	char out[300], err[200] = "";
	snprintf(out, sizeof out, "%s/out.tar.gz", dir);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_make(out, err, sizeof err), err);

	/* then it changed */
	pf_set_put_str("globals.grill_name", "After");
	pf_db_kv_put("learning", "anchors", "[2]");
	char cpath[400]; snprintf(cpath, sizeof cpath, "%s/cookfiles/cook-7.json", dir); unlink(cpath);
	write_cook(8, "{\"name\":\"newer\"}");

	/* a restore is staged and marked, as the API does */
	char stage[300], pending[300];
	snprintf(stage, sizeof stage, "%s/backup/restore", dir);
	snprintf(pending, sizeof pending, "%s/backup/restore.pending", dir);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_stage(out, stage, err, sizeof err), err);
	pf_write_file_atomic(pending, stage, strlen(stage));

	/* the next start: apply before opening */
	pf_db_close();
	TEST_ASSERT_EQUAL_INT(1, pf_backup_apply_staged(dir, cfg));
	TEST_ASSERT_FALSE(pf_file_exists(pending));
	pf_settings_init(cfg);
	pf_db_open(db);
	char name[64] = "";
	pf_set_str("globals.grill_name", name, sizeof name, "");
	TEST_ASSERT_EQUAL_STRING("Before", name);
	char v[64] = "";
	TEST_ASSERT_EQUAL_INT(0, pf_db_kv_get("learning", "anchors", v, sizeof v));
	TEST_ASSERT_EQUAL_STRING("[1]", v);
	TEST_ASSERT_TRUE(pf_file_exists(cpath));
	snprintf(cpath, sizeof cpath, "%s/cookfiles/cook-8.json", dir);
	TEST_ASSERT_FALSE_MESSAGE(pf_file_exists(cpath), "the backup's set of cooks replaces what was there");
	/* and the daemon is told, once */
	pf_backup_init(dir, cfg, true);
	char done[300]; snprintf(done, sizeof done, "%s/backup/restored", dir);
	TEST_ASSERT_FALSE(pf_file_exists(done));
	/* a second start does nothing */
	TEST_ASSERT_EQUAL_INT(0, pf_backup_apply_staged(dir, cfg));
}

static void test_the_folder_destination_round_trips_and_prunes(void)
{
	char dest[300];
	snprintf(dest, sizeof dest, "%s/dest", dir);
	pf_set_put_str("backup.destination", "folder");
	pf_set_put_str("backup.folder.path", dest);
	pf_set_put_num("backup.keep", 2);
	/* three archives with distinct minutes in their names, the way a schedule would leave them */
	pf_mkdir_p(dest);
	const char *old[] = { "pifire-grill-20260101-0300.tar.gz", "pifire-grill-20260102-0300.tar.gz", "pifire-grill-20260103-0300.tar.gz" };
	for (int i = 0; i < 3; i++) { char p[400]; snprintf(p, sizeof p, "%s/%s", dest, old[i]); pf_write_file_atomic(p, "x", 1); }
	char err[200] = "";
	cJSON *l = pf_backup_list(err, sizeof err);
	TEST_ASSERT_NOT_NULL_MESSAGE(l, err);
	TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(l));
	TEST_ASSERT_EQUAL_STRING(old[2], pf_json_str(cJSON_GetArrayItem(l, 0), "name", ""));   /* newest first */
	cJSON_Delete(l);
	char msg[240];
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_test(msg, sizeof msg), msg);
	/* a real run: makes, sends, prunes to the newest two */
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, pf_backup_run(err, sizeof err), err);
	for (int i = 0; i < 200; i++) {
		cJSON *st = pf_backup_status_json();
		bool busy = cJSON_IsTrue(cJSON_GetObjectItem(st, "busy"));
		cJSON_Delete(st);
		if (!busy) break;
		pf_sleep_ms(50);
	}
	cJSON *st = pf_backup_status_json();
	TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "last"), "ok")), pf_json_str(st, "message", ""));
	cJSON_Delete(st);
	l = pf_backup_list(err, sizeof err);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_EQUAL_INT_MESSAGE(2, cJSON_GetArraySize(l), "pruned to keep");
	TEST_ASSERT_NOT_NULL(strstr(pf_json_str(cJSON_GetArrayItem(l, 0), "name", ""), "pifire-"));
	TEST_ASSERT_EQUAL_STRING(old[2], pf_json_str(cJSON_GetArrayItem(l, 1), "name", ""));
	cJSON_Delete(l);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_archive_holds_the_picture_and_leaves_the_chart_out);
	RUN_TEST(test_a_garbage_file_is_refused);
	RUN_TEST(test_a_staged_restore_lands_before_the_database_opens);
	RUN_TEST(test_the_folder_destination_round_trips_and_prunes);
	return UNITY_END();
}
