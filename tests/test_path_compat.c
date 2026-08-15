/*
 * portty — MSYS2/Unix to Windows path conversion tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_path_compat.c — tests for MSYS2/Windows path conversion
 *
 * Pure C, no SDL/coffer dependency.
 */
#include "path_compat.h"
#include "test_helpers.h"
#include <string.h>

/* --- derive_msys_root tests --- */

static void test_root_ucrt64(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\msys64\\ucrt64\\bin\\portty.exe", out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64");
}

static void test_root_mingw64(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\msys64\\mingw64\\bin\\portty.exe", out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64");
}

static void test_root_clang64(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\msys64\\clang64\\bin\\portty.exe", out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64");
}

static void test_root_msys(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\msys64\\msys\\bin\\portty.exe", out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64");
}

static void test_root_scoop(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\Users\\alice\\scoop\\apps\\msys2\\current\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\Users\\alice\\scoop\\apps\\msys2\\current");
}

static void test_root_custom_drive(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "D:\\msys\\ucrt64\\bin\\portty.exe", out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "D:\\msys");
}

static void test_root_no_bin(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\Users\\alice\\portty.exe", out, sizeof(out));
    ASSERT_FALSE(ok);
}

static void test_root_bin_without_env_prefix(void)
{
    char out[1024];
    bool ok = path_compat_derive_msys_root(
        "C:\\some\\bin\\portty.exe", out, sizeof(out));
    ASSERT_FALSE(ok);
}

/* --- msys_to_win tests --- */

static void test_convert_drive_letter_slash(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/c/Users/foo", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\Users\\foo");
}

static void test_convert_uppercase_drive(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/D/tmp", "D:\\msys\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "D:\\tmp");
}

static void test_convert_already_native_forward(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "C:/Users/foo", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\Users\\foo");
}

static void test_convert_already_native_backslash(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "C:\\Users\\foo", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\Users\\foo");
}

static void test_convert_unix_home(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/home/thomasc", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64\\home\\thomasc");
}

static void test_convert_unix_tmp(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/tmp", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64\\tmp");
}

static void test_convert_unix_deep(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/home/thomasc/git/chest/portty",
        "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64\\home\\thomasc\\git\\chest\\portty");
}

static void test_convert_scoop_root(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/home/alice",
        "C:\\Users\\alice\\scoop\\apps\\msys2\\current\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out,
                  "C:\\Users\\alice\\scoop\\apps\\msys2\\current\\home\\alice");
}

static void test_convert_unix_no_msys_exe(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/home/thomasc", "C:\\Users\\thomasc\\portty.exe",
        out, sizeof(out));
    ASSERT_FALSE(ok);
}

static void test_convert_empty_path(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_FALSE(ok);
}

static void test_convert_just_slash(void)
{
    char out[1024];
    bool ok = path_compat_msys_to_win(
        "/", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_TRUE(ok);
    ASSERT_STR_EQ(out, "C:\\msys64");
}

static void test_convert_buffer_too_small(void)
{
    char out[4];
    bool ok = path_compat_msys_to_win(
        "/home/thomasc", "C:\\msys64\\ucrt64\\bin\\portty.exe",
        out, sizeof(out));
    ASSERT_FALSE(ok);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("path_compat_derive_msys_root:\n");
    RUN_TEST(test_root_ucrt64);
    RUN_TEST(test_root_mingw64);
    RUN_TEST(test_root_clang64);
    RUN_TEST(test_root_msys);
    RUN_TEST(test_root_scoop);
    RUN_TEST(test_root_custom_drive);
    RUN_TEST(test_root_no_bin);
    RUN_TEST(test_root_bin_without_env_prefix);

    printf("\npath_compat_msys_to_win:\n");
    RUN_TEST(test_convert_drive_letter_slash);
    RUN_TEST(test_convert_uppercase_drive);
    RUN_TEST(test_convert_already_native_forward);
    RUN_TEST(test_convert_already_native_backslash);
    RUN_TEST(test_convert_unix_home);
    RUN_TEST(test_convert_unix_tmp);
    RUN_TEST(test_convert_unix_deep);
    RUN_TEST(test_convert_scoop_root);
    RUN_TEST(test_convert_unix_no_msys_exe);
    RUN_TEST(test_convert_empty_path);
    RUN_TEST(test_convert_just_slash);
    RUN_TEST(test_convert_buffer_too_small);

    TEST_SUMMARY();
}
