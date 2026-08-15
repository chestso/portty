/*
 * portty — Physical display and DPI information collection
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "display_info.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#else
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#endif

const char *display_info_get_physical(void)
{
    static char buf[512];

#if defined(_WIN32)
    // Windows: use GetMonitorInfo + GetDeviceCaps for physical mm
    HDC dc = GetDC(NULL);
    if (dc) {
        int w = GetDeviceCaps(dc, HORZRES);
        int h = GetDeviceCaps(dc, VERTRES);
        int wmm = GetDeviceCaps(dc, HORZSIZE);
        int hmm = GetDeviceCaps(dc, VERTSIZE);
        ReleaseDC(NULL, dc);
        if (w > 0 && h > 0) {
            if (wmm > 0 && hmm > 0)
                snprintf(buf, sizeof(buf), "%dx%d, %dx%d mm", w, h, wmm, hmm);
            else
                snprintf(buf, sizeof(buf), "%dx%d, mm unavailable", w, h);
            return buf;
        }
    }
    return NULL;

#elif defined(__APPLE__)
    // macOS: CGDisplayScreenSize returns physical size in mm
    CGDirectDisplayID display = CGMainDisplayID();
    CGSize size = CGDisplayScreenSize(display);
    size_t w = CGDisplayPixelsWide(display);
    size_t h = CGDisplayPixelsHigh(display);
    if (w > 0 && h > 0) {
        if (size.width > 0 && size.height > 0) {
            float dpi_x = (float)w * 25.4f / (float)size.width;
            float dpi_y = (float)h * 25.4f / (float)size.height;
            snprintf(buf, sizeof(buf), "%zux%zu, %.0fx%.0f mm, %.0fx%.0f DPI", w, h,
                     size.width, size.height, dpi_x, dpi_y);
        } else
            snprintf(buf, sizeof(buf), "%zux%zu, mm unavailable", w, h);
        return buf;
    }
    return NULL;

#else
    // Linux: scan /sys/class/drm/ for an enabled connector, read its
    // preferred mode from the modes file, and try to get physical mm
    // from the EDID blob in sysfs.
    DIR *dir = opendir("/sys/class/drm");
    if (!dir)
        return NULL;

    struct dirent *ent;
    int found_w = 0, found_h = 0;
    int found_mm_w = 0, found_mm_h = 0;
    char found_name[256] = "";

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0)
            continue;
        // Must contain a '-' (connector entry, not the card itself)
        if (!strchr(ent->d_name + 4, '-'))
            continue;

        char path[512];

        // Check if enabled
        snprintf(path, sizeof(path), "/sys/class/drm/%s/enabled", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char enabled[16] = "";
        if (fgets(enabled, sizeof(enabled), f)) {
            // Strip whitespace
            char *nl = strchr(enabled, '\n');
            if (nl)
                *nl = '\0';
        }
        fclose(f);
        if (strcmp(enabled, "enabled") != 0)
            continue;

        // Read modes (first line is usually the preferred/native mode)
        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", ent->d_name);
        f = fopen(path, "r");
        if (f) {
            char mode[32] = "";
            if (fgets(mode, sizeof(mode), f)) {
                char *nl = strchr(mode, '\n');
                if (nl)
                    *nl = '\0';
                int w, h;
                if (sscanf(mode, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    found_w = w;
                    found_h = h;
                    snprintf(found_name, sizeof(found_name), "%s", ent->d_name);
                }
            }
            fclose(f);
        }

        // Try reading EDID for physical mm
        snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", ent->d_name);
        f = fopen(path, "rb");
        if (f) {
            unsigned char edid[128];
            size_t n = fread(edid, 1, sizeof(edid), f);
            fclose(f);
            if (n >= 23 && edid[0] == 0x00 && edid[1] == 0xff) {
                int mm_w = edid[21] * 10;
                int mm_h = edid[22] * 10;
                if (mm_w > 0 && mm_h > 0) {
                    found_mm_w = mm_w;
                    found_mm_h = mm_h;
                }
            }
        }

        if (found_w > 0)
            break;
    }
    closedir(dir);

    if (found_w > 0 && found_h > 0) {
        if (found_mm_w > 0 && found_mm_h > 0) {
            float dpi_x = (float)found_w * 25.4f / (float)found_mm_w;
            float dpi_y = (float)found_h * 25.4f / (float)found_mm_h;
            snprintf(buf, sizeof(buf), "%dx%d, %dx%d mm, %.0fx%.0f DPI (%s)",
                     found_w, found_h, found_mm_w, found_mm_h,
                     dpi_x, dpi_y, found_name);
        } else
            snprintf(buf, sizeof(buf), "%dx%d, mm unavailable (%s)",
                     found_w, found_h, found_name);
        return buf;
    }
    return NULL;
#endif
}
