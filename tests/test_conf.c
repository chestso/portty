#include "bloom_conf.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Write content to a temp file and return the path (caller must free) */
static char *write_tmp_conf(const char *content)
{
    char path[] = "/tmp/bloom_test_conf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return NULL;
    }
    fputs(content, fp);
    fclose(fp);
    return strdup(path);
}

static void cleanup_tmp(char *path)
{
    if (path) {
        remove(path);
        free(path);
    }
}

/* --- Tests --- */

static void test_init_defaults(void)
{
    BloomConf conf;
    bloom_conf_init(&conf);

    ASSERT_NULL(conf.font);
    ASSERT_EQ(conf.cols, 0);
    ASSERT_EQ(conf.rows, 0);
    ASSERT_EQ(conf.hinting, BLOOM_HINT_UNSET);
    ASSERT_EQ(conf.padding, -1);
    ASSERT_EQ(conf.verbose, -1);
    ASSERT_NULL(conf.word_chars);
    ASSERT_NULL(conf.platform);
    ASSERT_EQ(conf.scrollback, -1);

    bloom_conf_free(&conf);
}

static void test_parse_font(void)
{
    char *path = write_tmp_conf("[terminal]\nfont = monospace-14\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "monospace-14");

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_geometry(void)
{
    char *path = write_tmp_conf("[terminal]\ngeometry = 120x40\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cols, 120);
    ASSERT_EQ(conf.rows, 40);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_hinting(void)
{
    char *path = write_tmp_conf("[terminal]\nhinting = mono\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.hinting, BLOOM_HINT_MONO);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_booleans(void)
{
    char *path = write_tmp_conf("[terminal]\npadding = false\nverbose = yes\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.padding, 0);
    ASSERT_EQ(conf.verbose, 1);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_word_chars(void)
{
    char *path = write_tmp_conf("[terminal]\nword_chars = -_./~\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.word_chars, "-_./~");

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_platform(void)
{
    char *path = write_tmp_conf("[terminal]\nplatform = gtk4\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.platform, "gtk4");

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback(void)
{
    char *path = write_tmp_conf("[terminal]\nscrollback = 5000\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, 5000);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback_zero(void)
{
    char *path = write_tmp_conf("[terminal]\nscrollback = 0\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, 0);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback_invalid(void)
{
    char *path = write_tmp_conf("[terminal]\nscrollback = -5\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, -1);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_comments_and_blank_lines(void)
{
    char *path = write_tmp_conf(
        "# Top comment\n"
        "\n"
        "[terminal]\n"
        "; semicolon comment\n"
        "font = test-font\n"
        "\n"
        "# inline comment line\n"
        "padding = true\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "test-font");
    ASSERT_EQ(conf.padding, 1);

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_unknown_keys_ignored(void)
{
    char *path = write_tmp_conf("[terminal]\nunknown_key = whatever\nfont = ok\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "ok");

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_wrong_section_ignored(void)
{
    char *path = write_tmp_conf("[other]\nfont = ignored\n[terminal]\nfont = kept\n");
    ASSERT_NOT_NULL(path);

    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_TRUE(bloom_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "kept");

    bloom_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_nonexistent_file(void)
{
    BloomConf conf;
    bloom_conf_init(&conf);
    ASSERT_FALSE(bloom_conf_load_path(&conf, "/tmp/bloom_nonexistent_12345.conf"));
    bloom_conf_free(&conf);
}

static void test_all_hinting_modes(void)
{
    const char *modes[] = { "none", "light", "normal", "mono" };
    BloomHintMode expected[] = { BLOOM_HINT_NONE, BLOOM_HINT_LIGHT, BLOOM_HINT_NORMAL,
                                 BLOOM_HINT_MONO };

    for (int i = 0; i < 4; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[terminal]\nhinting = %s\n", modes[i]);
        char *path = write_tmp_conf(buf);
        ASSERT_NOT_NULL(path);

        BloomConf conf;
        bloom_conf_init(&conf);
        ASSERT_TRUE(bloom_conf_load_path(&conf, path));
        ASSERT_EQ(conf.hinting, expected[i]);

        bloom_conf_free(&conf);
        cleanup_tmp(path);
    }
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_conf\n");

    RUN_TEST(test_init_defaults);
    RUN_TEST(test_parse_font);
    RUN_TEST(test_parse_geometry);
    RUN_TEST(test_parse_hinting);
    RUN_TEST(test_parse_booleans);
    RUN_TEST(test_parse_word_chars);
    RUN_TEST(test_parse_platform);
    RUN_TEST(test_parse_scrollback);
    RUN_TEST(test_parse_scrollback_zero);
    RUN_TEST(test_parse_scrollback_invalid);
    RUN_TEST(test_comments_and_blank_lines);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_wrong_section_ignored);
    RUN_TEST(test_nonexistent_file);
    RUN_TEST(test_all_hinting_modes);

    TEST_SUMMARY();
}
