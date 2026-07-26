#include "portty_script.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Write content to a temp file and return the path (caller must free) */
static char *write_tmp_script(const char *content)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0)
        return NULL;
    char path[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "portty", 0, path) == 0)
        return NULL;
    FILE *fp = fopen(path, "w");
#else
    char path[] = "/tmp/portty_test_script_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
#endif
    if (!fp) {
#ifndef _WIN32
        close(fd);
#endif
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

/* ── Parser tests ── */

static void test_load_empty_file(void)
{
    char *path = write_tmp_script("");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 0);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_load_comments_and_blanks(void)
{
    char *path = write_tmp_script(
        "# This is a comment\n"
        "\n"
        "   # indented comment\n"
        "\n"
        "# another comment\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 0);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait(void)
{
    char *path = write_tmp_script("wait 3.5\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT);
    ASSERT_TRUE(cmd->wait_seconds > 3.4 && cmd->wait_seconds < 3.6);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_integer(void)
{
    char *path = write_tmp_script("wait 5\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT);
    ASSERT_TRUE(cmd->wait_seconds > 4.9 && cmd->wait_seconds < 5.1);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_send(void)
{
    char *path = write_tmp_script("send hello world\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SEND);
    ASSERT_STR_EQ(cmd->text, "hello world");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_send_escapes(void)
{
    char *path = write_tmp_script("send line1\\nline2\\t\\e\\\\\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SEND);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 14);
    ASSERT_EQ(cmd->text[0], 'l');
    ASSERT_EQ(cmd->text[5], '\n');
    ASSERT_EQ(cmd->text[11], '\t');
    ASSERT_EQ(cmd->text[12], '\x1b');
    ASSERT_EQ(cmd->text[13], '\\');

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_send_quoted(void)
{
    char *path = write_tmp_script("send \"hello world\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SEND);
    ASSERT_STR_EQ(cmd->text, "hello world");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_raw(void)
{
    char *path = write_tmp_script("raw 1b 5b 6d\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_RAW);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 3);
    ASSERT_EQ((uint8_t)cmd->text[0], 0x1b);
    ASSERT_EQ((uint8_t)cmd->text[1], 0x5b);
    ASSERT_EQ((uint8_t)cmd->text[2], 0x6d);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_assert_contains(void)
{
    char *path = write_tmp_script("assert-contains hello crush\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_ASSERT_CONTAINS);
    ASSERT_STR_EQ(cmd->text, "hello crush");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_assert_not_contains(void)
{
    char *path = write_tmp_script("assert-not-contains error\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_ASSERT_NOT_CONTAINS);
    ASSERT_STR_EQ(cmd->text, "error");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_screendump(void)
{
    char *path = write_tmp_script("screendump /tmp/screenshot.png\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SCREENDUMP);
    ASSERT_STR_EQ(cmd->path, "/tmp/screenshot.png");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_dumprow(void)
{
    char *path = write_tmp_script("dumprow 5\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_DUMPROW);
    ASSERT_EQ(cmd->row, 5);
    ASSERT_EQ(cmd->col_start, -1);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_dumpcells(void)
{
    char *path = write_tmp_script("dumpcells 5 3 10\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_DUMPCELLS);
    ASSERT_EQ(cmd->row, 5);
    ASSERT_EQ(cmd->col_start, 3);
    ASSERT_EQ(cmd->col_end, 10);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_dumpverts(void)
{
    char *path = write_tmp_script("dumpverts 5 3 6\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_DUMPVERTS);
    ASSERT_EQ(cmd->row, 5);
    ASSERT_EQ(cmd->col_start, 3);
    ASSERT_EQ(cmd->col_end, 6);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_verifybuf(void)
{
    char *path = write_tmp_script("verifybuf 5 3 6\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_VERIFYBUF);
    ASSERT_EQ(cmd->row, 5);
    ASSERT_EQ(cmd->col_start, 3);
    ASSERT_EQ(cmd->col_end, 6);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_quit(void)
{
    char *path = write_tmp_script("quit\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_QUIT);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_multi_command(void)
{
    char *path = write_tmp_script(
        "wait 2\n"
        "send hello\\n\n"
        "assert-contains hello\n"
        "screendump /tmp/out.png\n"
        "quit\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 5);

    ASSERT_EQ(portty_script_get(s, 0)->type, SCRIPT_CMD_WAIT);
    ASSERT_EQ(portty_script_get(s, 1)->type, SCRIPT_CMD_SEND);
    ASSERT_EQ(portty_script_get(s, 2)->type, SCRIPT_CMD_ASSERT_CONTAINS);
    ASSERT_EQ(portty_script_get(s, 3)->type, SCRIPT_CMD_SCREENDUMP);
    ASSERT_EQ(portty_script_get(s, 4)->type, SCRIPT_CMD_QUIT);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_unknown_command(void)
{
    char *path = write_tmp_script("frobnicate foo\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    // Now returns a struct with error
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(portty_script_error(s));
    portty_script_free(s);

    cleanup_tmp(path);
}

static void test_parse_missing_file(void)
{
    PorttyScript *s = portty_script_load("/nonexistent/path/to/script");
    // Now returns a struct with error instead of NULL
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(portty_script_error(s));
    portty_script_free(s);
}

static void test_get_out_of_range(void)
{
    char *path = write_tmp_script("quit\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    ASSERT_NULL(portty_script_get(s, 1));
    ASSERT_NULL(portty_script_get(s, -1));

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_raw_no_args(void)
{
    char *path = write_tmp_script("raw\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_RAW);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 0);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_send_no_args(void)
{
    char *path = write_tmp_script("send\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SEND);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 0);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_sendln(void)
{
    char *path = write_tmp_script("sendln hello world\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SENDLN);
    ASSERT_STR_EQ(cmd->text, "hello world");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_dumpcells_missing_args(void)
{
    char *path = write_tmp_script("dumpcells 5\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    // Now returns a struct with error
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(portty_script_error(s));
    portty_script_free(s);

    cleanup_tmp(path);
}

static void test_parse_screendump_long_path(void)
{
    char longpath[600];
    memset(longpath, 'A', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';

    char script[700];
    snprintf(script, sizeof(script), "screendump %s\n", longpath);

    char *path = write_tmp_script(script);
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SCREENDUMP);
    /* Path should be truncated to fit in cmd->path[512] */
    ASSERT_TRUE(strlen(cmd->path) < sizeof(cmd->path));

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_error_message(void)
{
    PorttyScript *s = portty_script_load("/nonexistent/path");
    // Now returns a struct with error
    ASSERT_NOT_NULL(s);
    const char *err = portty_script_error(s);
    ASSERT_NOT_NULL(err);
    ASSERT_TRUE(strstr(err, "cannot open file") != NULL);
    portty_script_free(s);
}

static void test_parse_trailing_whitespace(void)
{
    char *path = write_tmp_script("wait 3.0   \n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT);
    ASSERT_TRUE(cmd->wait_seconds > 2.9 && cmd->wait_seconds < 3.1);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_send_carriage_return(void)
{
    char *path = write_tmp_script("send hello\\rworld\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_SEND);
    ASSERT_EQ(cmd->text[5], '\r');

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_emit(void)
{
    char *path = write_tmp_script("emit \"hello world\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_EMIT);
    ASSERT_STR_EQ(cmd->text, "hello world");

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_emit_escapes(void)
{
    char *path = write_tmp_script("emit \\e[4mSingle\\e[0m\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_EMIT);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 14);
    ASSERT_EQ(cmd->text[0], '\x1b');
    ASSERT_EQ(cmd->text[2], '4');
    ASSERT_EQ(cmd->text[10], '\x1b');
    ASSERT_EQ(cmd->text[12], '0');
    ASSERT_EQ(cmd->text[13], 'm');

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_emit_raw(void)
{
    char *path = write_tmp_script("emit-raw 1b 5b 34 6d 1b 5b 30 6d\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_EMIT_RAW);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ((int)strlen(cmd->text), 8);
    ASSERT_EQ((uint8_t)cmd->text[0], 0x1b);
    ASSERT_EQ((uint8_t)cmd->text[2], 0x34);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_x_escape(void)
{
    char *path = write_tmp_script("emit \\x1b[4mSingle\\x1b[0m\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_EMIT);
    ASSERT_NOT_NULL(cmd->text);
    ASSERT_EQ(cmd->text[0], '\x1b');
    ASSERT_EQ(cmd->text[2], '4');
    ASSERT_EQ(cmd->text[10], '\x1b');
    ASSERT_EQ(cmd->text[12], '0');
    ASSERT_EQ(cmd->text[13], 'm');

    portty_script_free(s);
    cleanup_tmp(path);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_load_empty_file);
    RUN_TEST(test_load_comments_and_blanks);
    RUN_TEST(test_parse_wait);
    RUN_TEST(test_parse_wait_integer);
    RUN_TEST(test_parse_send);
    RUN_TEST(test_parse_send_escapes);
    RUN_TEST(test_parse_send_quoted);
    RUN_TEST(test_parse_send_carriage_return);
    RUN_TEST(test_parse_raw);
    RUN_TEST(test_parse_raw_no_args);
    RUN_TEST(test_parse_send_no_args);
    RUN_TEST(test_parse_sendln);
    RUN_TEST(test_parse_emit);
    RUN_TEST(test_parse_emit_escapes);
    RUN_TEST(test_parse_emit_raw);
    RUN_TEST(test_parse_x_escape);
    RUN_TEST(test_parse_assert_contains);
    RUN_TEST(test_parse_assert_not_contains);
    RUN_TEST(test_parse_screendump);
    RUN_TEST(test_parse_dumprow);
    RUN_TEST(test_parse_dumpcells);
    RUN_TEST(test_parse_dumpverts);
    RUN_TEST(test_parse_verifybuf);
    RUN_TEST(test_parse_quit);
    RUN_TEST(test_parse_multi_command);
    RUN_TEST(test_parse_unknown_command);
    RUN_TEST(test_parse_missing_file);
    RUN_TEST(test_get_out_of_range);
    RUN_TEST(test_parse_dumpcells_missing_args);
    RUN_TEST(test_parse_screendump_long_path);
    RUN_TEST(test_parse_trailing_whitespace);
    RUN_TEST(test_error_message);

    TEST_SUMMARY();
}
