/*
 * portty — Debug script parser tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

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

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
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

static void test_parse_wait_for(void)
{
    char *path = write_tmp_script("wait-for \"hello crush\" 2.5\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "hello crush");
    ASSERT_TRUE(cmd->wait_seconds > 2.4 && cmd->wait_seconds < 2.6);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_default_timeout(void)
{
    char *path = write_tmp_script("wait-for \"END OF DEMO\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "END OF DEMO");
    ASSERT_FLOAT_NEAR(cmd->wait_seconds, 600, 0.001);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_unquoted_timeout(void)
{
    char *path = write_tmp_script("wait-for build succeeded 30\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "build succeeded");
    ASSERT_TRUE(cmd->wait_seconds > 29.9 && cmd->wait_seconds < 30.1);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_unquoted_no_timeout(void)
{
    char *path = write_tmp_script("wait-for END OF DEMO\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "END OF DEMO");
    ASSERT_FLOAT_NEAR(cmd->wait_seconds, 600, 0.001);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_quoted_dump(void)
{
    char *path = write_tmp_script("wait-for \"END OF DEMO\" 900 dump\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "END OF DEMO");
    ASSERT_TRUE(cmd->wait_seconds > 899.9 && cmd->wait_seconds < 900.1);
    ASSERT_TRUE(cmd->wait_dump);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_quoted_dump_no_timeout(void)
{
    char *path = write_tmp_script("wait-for \"marker\" dump\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "marker");
    ASSERT_FLOAT_NEAR(cmd->wait_seconds, 600, 0.001); /* default */
    ASSERT_TRUE(cmd->wait_dump);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_unquoted_dump(void)
{
    char *path = write_tmp_script("wait-for marker 30 dump\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "marker");
    ASSERT_TRUE(cmd->wait_seconds > 29.9 && cmd->wait_seconds < 30.1);
    ASSERT_TRUE(cmd->wait_dump);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_dump_in_text_not_keyword(void)
{
    /* "dump" as part of the text must not be stripped */
    char *path = write_tmp_script("wait-for \"core dump report\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_WAIT_FOR);
    ASSERT_STR_EQ(cmd->text, "core dump report");
    ASSERT_FALSE(cmd->wait_dump);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_missing_text(void)

{

    char *path = write_tmp_script("wait-for\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(portty_script_error(s));

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_wait_for_empty_quoted_text(void)
{
    char *path = write_tmp_script("wait-for \"\" 10\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(portty_script_error(s));

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

static void test_parse_panel(void)
{
    char *path = write_tmp_script("panel 1 0 0 40 3 \"Build Complete\" \"All tests passed\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_PANEL);
    ASSERT_EQ(cmd->panel_id, 1);
    ASSERT_EQ(cmd->panel_col, 0);
    ASSERT_EQ(cmd->panel_row, 0);
    ASSERT_EQ(cmd->panel_cols, 40);
    ASSERT_EQ(cmd->panel_rows, 3);
    ASSERT_STR_EQ(cmd->panel_title, "Build Complete");
    ASSERT_STR_EQ(cmd->panel_body, "All tests passed");
    ASSERT_EQ(cmd->panel_level, 0);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_parse_panel_with_level(void)
{
    char *path = write_tmp_script("panel 2 10 5 30 2 \"Warning\" \"Disk space low\" 1\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_count(s), 1);

    const ScriptCmd *cmd = portty_script_get(s, 0);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ(cmd->type, SCRIPT_CMD_PANEL);
    ASSERT_EQ(cmd->panel_id, 2);
    ASSERT_EQ(cmd->panel_col, 10);
    ASSERT_EQ(cmd->panel_row, 5);
    ASSERT_EQ(cmd->panel_cols, 30);
    ASSERT_EQ(cmd->panel_rows, 2);
    ASSERT_STR_EQ(cmd->panel_title, "Warning");
    ASSERT_STR_EQ(cmd->panel_body, "Disk space low");
    ASSERT_EQ(cmd->panel_level, 1);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_wait_remaining_ms_reports(void)
{
    char *path = write_tmp_script("wait 0.05\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);

    uint32_t before = portty_script_wait_remaining_ms(s, 0);
    ASSERT_TRUE(before > 0 && before <= 60);

    sleep_ms(20);
    uint32_t after = portty_script_wait_remaining_ms(s, 0);
    ASSERT_TRUE(after < before);
    ASSERT_TRUE(after <= 40);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_wait_remaining_ms_non_wait_returns_max(void)
{
    char *path = write_tmp_script("send \"hi\"\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_wait_remaining_ms(s, 0), UINT32_MAX);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_wait_remaining_ms_wait_for_reports_timeout(void)
{
    char *path = write_tmp_script("wait-for ready 0.05\n");
    ASSERT_NOT_NULL(path);

    PorttyScript *s = portty_script_load(path);
    ASSERT_NOT_NULL(s);

    uint32_t before = portty_script_wait_remaining_ms(s, 0);
    ASSERT_TRUE(before > 0 && before <= 60);

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
    RUN_TEST(test_parse_wait_for);
    RUN_TEST(test_parse_wait_for_default_timeout);
    RUN_TEST(test_parse_wait_for_unquoted_timeout);
    RUN_TEST(test_parse_wait_for_unquoted_no_timeout);
    RUN_TEST(test_parse_wait_for_quoted_dump);
    RUN_TEST(test_parse_wait_for_quoted_dump_no_timeout);
    RUN_TEST(test_parse_wait_for_unquoted_dump);
    RUN_TEST(test_parse_wait_for_dump_in_text_not_keyword);
    RUN_TEST(test_parse_wait_for_missing_text);
    RUN_TEST(test_parse_wait_for_empty_quoted_text);
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
    RUN_TEST(test_parse_quit);
    RUN_TEST(test_parse_multi_command);
    RUN_TEST(test_parse_unknown_command);
    RUN_TEST(test_parse_missing_file);
    RUN_TEST(test_get_out_of_range);
    RUN_TEST(test_parse_dumpcells_missing_args);
    RUN_TEST(test_parse_screendump_long_path);
    RUN_TEST(test_parse_trailing_whitespace);
    RUN_TEST(test_error_message);
    RUN_TEST(test_parse_panel);
    RUN_TEST(test_parse_panel_with_level);
    RUN_TEST(test_wait_remaining_ms_reports);
    RUN_TEST(test_wait_remaining_ms_non_wait_returns_max);
    RUN_TEST(test_wait_remaining_ms_wait_for_reports_timeout);

    TEST_SUMMARY();
}
