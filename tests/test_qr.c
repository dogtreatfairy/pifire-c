/* QR encoder: golden matrices verified module-for-module against an independent encoder
 * (python-qrcode) across 131 strings, plus the structural invariants of a valid symbol. */
#include "display/qr.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *const G_V1[] = {
	"#######..#....#######",
	"#.....#.#..#..#.....#",
	"#.###.#.#.#.#.#.###.#",
	"#.###.#..#....#.###.#",
	"#.###.#.####..#.###.#",
	"#.....#.###...#.....#",
	"#######.#.#.#.#######",
	"........#..##........",
	"##.#..##..#...###.##.",
	".##.#...#.####..#...#",
	"#...####....#.#...#.#",
	"####.#..#.#..##.##.##",
	"###.####.#..##.#.#...",
	"........###.#..#....#",
	"#######.#..##...####.",
	"#.....#..#####..#..#.",
	"#.###.#...##.#.###.##",
	"#.###.#.##..#..##...#",
	"#.###.#..#..#...#.#.#",
	"#.....#.##.#...#.#...",
	"#######.#######....#.",
};

static const char *const G_V2[] = {
	"#######.##.#.#..#.#######",
	"#.....#...##..#.#.#.....#",
	"#.###.#.#..####.#.#.###.#",
	"#.###.#.#.######..#.###.#",
	"#.###.#.#.#.#####.#.###.#",
	"#.....#......##.#.#.....#",
	"#######.#.#.#.#.#.#######",
	"............#.#.#........",
	"####..#.####.....#..###.#",
	".##..#.....##.#.#..#...#.",
	"....#.#.#####....##.#....",
	".###.......#.#.#.#.#.##..",
	"####.###.#...##..####.###",
	".#.###......#.##.####...#",
	".#...##......##..#..#.##.",
	"#..##..###.....##..##...#",
	"...#.###..####..#########",
	"........#..###..#...#.#.#",
	"#######..##.###.#.#.#.###",
	"#.....#.....#.###...#..#.",
	"#.###.#...##....######...",
	"#.###.#.#..##.#####.###..",
	"#.###.#.##.###...##.#.##.",
	"#.....#.#...####..#.#.#..",
	"#######.#....#...########",
};

static void check(const char *text, const char *const *golden, int size)
{
	pf_qr q;
	TEST_ASSERT_TRUE(pf_qr_encode(text, &q));
	TEST_ASSERT_EQUAL_INT(size, q.size);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			char got = q.m[y][x] ? '#' : '.';
			if (got != golden[y][x]) {
				char msg[80];
				snprintf(msg, sizeof msg, "module %d,%d", x, y);
				TEST_FAIL_MESSAGE(msg);
			}
		}
}

static void test_golden_matrices(void)
{
	check("http://10.0.0.5/", G_V1, 21);
	check("https://pifire.tail4df75.ts.net/", G_V2, 25);
}

/* the parts every symbol must have, whatever the payload */
static void test_structure(void)
{
	pf_qr q;
	TEST_ASSERT_TRUE(pf_qr_encode("http://pifire.local/", &q));
	int n = q.size;
	/* three finder patterns: a 3x3 dark core inside a light ring inside a dark ring */
	const int ox[3] = { 0, 0, 1 }, oy[3] = { 0, 1, 0 };
	for (int k = 0; k < 3; k++) {
		int bx = ox[k] ? n - 7 : 0, by = oy[k] ? n - 7 : 0;
		for (int dy = 0; dy < 7; dy++)
			for (int dx = 0; dx < 7; dx++) {
				int ring = dx == 0 || dx == 6 || dy == 0 || dy == 6;
				int core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
				TEST_ASSERT_EQUAL_INT(ring || core, q.m[by + dy][bx + dx]);
			}
	}
	/* timing patterns alternate along row and column 6 */
	for (int i = 8; i < n - 8; i++) {
		TEST_ASSERT_EQUAL_INT(!(i % 2), q.m[i][6]);
		TEST_ASSERT_EQUAL_INT(!(i % 2), q.m[6][i]);
	}
	TEST_ASSERT_EQUAL_INT(1, q.m[n - 8][8]);   /* the always-dark module */
}

static void test_version_selection_and_limits(void)
{
	pf_qr q;
	char buf[200];
	memset(buf, 'a', sizeof buf);
	const struct { int len, size; } cases[] = { { 1, 21 }, { 17, 21 }, { 18, 25 }, { 32, 25 }, { 33, 29 }, { 53, 29 }, { 54, 33 }, { 78, 33 }, { 79, 37 }, { 106, 37 } };
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		buf[cases[i].len] = 0;
		TEST_ASSERT_TRUE(pf_qr_encode(buf, &q));
		TEST_ASSERT_EQUAL_INT(cases[i].size, q.size);
		buf[cases[i].len] = 'a';
	}
	buf[107] = 0;   /* one byte past what version 5 at level L holds */
	TEST_ASSERT_FALSE(pf_qr_encode(buf, &q));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_golden_matrices);
	RUN_TEST(test_structure);
	RUN_TEST(test_version_selection_and_limits);
	return UNITY_END();
}
