#include "test_helpers.h"
#include "portty_panel.h"
#include <string.h>

static void test_panel_init(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);
    ASSERT_EQ(mgr.cell_w, 10);
    ASSERT_EQ(mgr.cell_h, 20);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 0);
}

static void test_panel_show_and_find(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    PanelState *p = panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "Test", "Body", PORTTY_NOTIFY_INFO, 0);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(p->active);
    ASSERT_EQ(p->id, 1);
    ASSERT_EQ(p->col, 5);
    ASSERT_EQ(p->row, 3);
    ASSERT_EQ(p->cols, 20);
    ASSERT_EQ(p->rows, 4);
    ASSERT_EQ(p->level, PORTTY_NOTIFY_INFO);
    ASSERT_STR_EQ(p->title, "Test");
    ASSERT_STR_EQ(p->body, "Body");
    ASSERT_FALSE(p->close_hover);

    PanelState *found = panel_mgr_find(&mgr, 1);
    ASSERT_EQ(found, p);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 1);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hide(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 0, 0, 10, 2, "Title", "Body", PORTTY_NOTIFY_WARNING, 0);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 1);

    panel_mgr_hide(&mgr, 1);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 0);
    ASSERT_NULL(panel_mgr_find(&mgr, 1));
}

static void test_panel_hide_all(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 0, 0, 10, 2, "A", "B", PORTTY_NOTIFY_INFO, 0);
    panel_mgr_show(&mgr, 2, 5, 5, 10, 2, "C", "D", PORTTY_NOTIFY_WARNING, 0);
    panel_mgr_show(&mgr, 3, 10, 10, 10, 2, "E", "F", PORTTY_NOTIFY_ERROR, 0);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 3);

    panel_mgr_hide_all(&mgr);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 0);
}

static void test_panel_max_limit(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        PanelState *p = panel_mgr_show(&mgr, i + 1, 0, 0, 10, 2, "T", "B", PORTTY_NOTIFY_INFO, 0);
        ASSERT_NOT_NULL(p);
    }
    ASSERT_EQ(panel_mgr_active_count(&mgr), PORTTY_PANEL_MAX);

    PanelState *overflow = panel_mgr_show(&mgr, 999, 0, 0, 10, 2, "X", "Y", PORTTY_NOTIFY_INFO, 0);
    ASSERT_NULL(overflow);
    ASSERT_EQ(panel_mgr_active_count(&mgr), PORTTY_PANEL_MAX);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_update_existing(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 0, 0, 10, 2, "Old", "Old", PORTTY_NOTIFY_INFO, 0);
    panel_mgr_show(&mgr, 1, 5, 5, 20, 4, "New", "New", PORTTY_NOTIFY_ERROR, 0);

    ASSERT_EQ(panel_mgr_active_count(&mgr), 1);
    PanelState *p = panel_mgr_find(&mgr, 1);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->col, 5);
    ASSERT_EQ(p->row, 5);
    ASSERT_EQ(p->cols, 20);
    ASSERT_EQ(p->rows, 4);
    ASSERT_EQ(p->level, PORTTY_NOTIFY_ERROR);
    ASSERT_STR_EQ(p->title, "New");
    ASSERT_STR_EQ(p->body, "New");

    panel_mgr_hide_all(&mgr);
}

static void test_panel_layout_computation(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    PanelState *p = panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "T", "B", PORTTY_NOTIFY_INFO, 0);
    ASSERT_EQ(p->px, 50);
    ASSERT_EQ(p->py, 60);
    ASSERT_EQ(p->pw, 200);
    ASSERT_EQ(p->ph, 80);
    ASSERT_EQ(p->close_size, 20);
    ASSERT_EQ(p->close_px, 230);
    ASSERT_EQ(p->close_py, 60);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_recompute_layout(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "T", "B", PORTTY_NOTIFY_INFO, 0);
    PanelState *p = panel_mgr_find(&mgr, 1);
    ASSERT_EQ(p->px, 50);

    panel_mgr_set_cell_size(&mgr, 15, 25);
    ASSERT_EQ(p->px, 75);
    ASSERT_EQ(p->py, 75);
    ASSERT_EQ(p->pw, 300);
    ASSERT_EQ(p->ph, 100);
    ASSERT_EQ(p->close_size, 25);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hit_test_body(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "T", "B", PORTTY_NOTIFY_INFO, 0);

    bool close_btn = false;
    int hit = panel_mgr_hit_test(&mgr, 100, 100, &close_btn);
    ASSERT_EQ(hit, 1);
    ASSERT_FALSE(close_btn);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hit_test_close_button(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "T", "B", PORTTY_NOTIFY_INFO, 0);
    PanelState *p = panel_mgr_find(&mgr, 1);

    bool close_btn = false;
    int hit = panel_mgr_hit_test(&mgr, p->close_px + 5, p->close_py + 5, &close_btn);
    ASSERT_EQ(hit, 1);
    ASSERT_TRUE(close_btn);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hit_test_outside(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 5, 3, 20, 4, "T", "B", PORTTY_NOTIFY_INFO, 0);

    bool close_btn = false;
    int hit = panel_mgr_hit_test(&mgr, 1000, 1000, &close_btn);
    ASSERT_EQ(hit, 0);
    ASSERT_FALSE(close_btn);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hit_test_multiple(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 0, 0, 10, 2, "A", "B", PORTTY_NOTIFY_INFO, 0);
    panel_mgr_show(&mgr, 2, 5, 3, 10, 2, "C", "D", PORTTY_NOTIFY_WARNING, 0);

    bool close_btn = false;
    int hit = panel_mgr_hit_test(&mgr, 60, 70, &close_btn);
    ASSERT_EQ(hit, 2);
    ASSERT_FALSE(close_btn);

    hit = panel_mgr_hit_test(&mgr, 10, 10, &close_btn);
    ASSERT_EQ(hit, 1);
    ASSERT_FALSE(close_btn);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_hover(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    panel_mgr_show(&mgr, 1, 0, 0, 10, 2, "T", "B", PORTTY_NOTIFY_INFO, 0);
    PanelState *p = panel_mgr_find(&mgr, 1);
    ASSERT_FALSE(p->close_hover);

    panel_mgr_set_hover(&mgr, 1, true);
    ASSERT_TRUE(p->close_hover);

    panel_mgr_set_hover(&mgr, 1, false);
    ASSERT_FALSE(p->close_hover);

    panel_mgr_hide_all(&mgr);
}

static void test_panel_grid_to_pixel(void)
{
    int px, py;
    panel_grid_to_pixel(10, 20, 5, 3, &px, &py);
    ASSERT_EQ(px, 50);
    ASSERT_EQ(py, 60);

    panel_grid_to_pixel(15, 25, 0, 0, &px, &py);
    ASSERT_EQ(px, 0);
    ASSERT_EQ(py, 0);
}

static void test_panel_pixel_to_grid(void)
{
    int col, row;
    panel_pixel_to_grid(10, 20, 50, 60, &col, &row);
    ASSERT_EQ(col, 5);
    ASSERT_EQ(row, 3);

    panel_pixel_to_grid(10, 20, 0, 0, &col, &row);
    ASSERT_EQ(col, 0);
    ASSERT_EQ(row, 0);

    panel_pixel_to_grid(10, 20, 99, 99, &col, &row);
    ASSERT_EQ(col, 9);
    ASSERT_EQ(row, 4);
}

static void test_panel_null_strings(void)
{
    PanelManager mgr;
    panel_mgr_init(&mgr, 10, 20);

    PanelState *p = panel_mgr_show(&mgr, 1, 0, 0, 10, 2, NULL, NULL, PORTTY_NOTIFY_INFO, 0);
    ASSERT_NOT_NULL(p);
    ASSERT_NULL(p->title);
    ASSERT_NULL(p->body);

    panel_mgr_hide(&mgr, 1);
    ASSERT_EQ(panel_mgr_active_count(&mgr), 0);

    panel_mgr_hide_all(&mgr);
}

int main(void)
{
    RUN_TEST(test_panel_init);
    RUN_TEST(test_panel_show_and_find);
    RUN_TEST(test_panel_hide);
    RUN_TEST(test_panel_hide_all);
    RUN_TEST(test_panel_max_limit);
    RUN_TEST(test_panel_update_existing);
    RUN_TEST(test_panel_layout_computation);
    RUN_TEST(test_panel_recompute_layout);
    RUN_TEST(test_panel_hit_test_body);
    RUN_TEST(test_panel_hit_test_close_button);
    RUN_TEST(test_panel_hit_test_outside);
    RUN_TEST(test_panel_hit_test_multiple);
    RUN_TEST(test_panel_hover);
    RUN_TEST(test_panel_grid_to_pixel);
    RUN_TEST(test_panel_pixel_to_grid);
    RUN_TEST(test_panel_null_strings);

    TEST_SUMMARY();
}
