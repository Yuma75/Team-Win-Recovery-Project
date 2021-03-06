/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>


extern "C" {
#include "../common.h"
#include "../roots.h"
#include "../ddftw.h"
#include "../minuitwrp/minui.h"
#include "../recovery_ui.h"
#include "../minzip/Zip.h"
#include <pixelflinger/pixelflinger.h>
}

#include "rapidxml.hpp"
#include "objects.hpp"
#include "../data.hpp"

#include "curtain.h"

const static int CURTAIN_FADE = 32;

using namespace rapidxml;

// Global values
static gr_surface gCurtain = NULL;
static int gGuiInitialized = 0;
static int gGuiConsoleRunning = 0;
static int gGuiConsoleTerminate = 0;
static int gForceRender = 0;
static int gNoAnimation = 0;

// Needed by pages.cpp too
int gGuiRunning = 0;

static int gRecorder = -1;

extern "C" void gr_write_frame_to_file(int fd);

void flip(void)
{
    if (gRecorder != -1)
    {
        timespec time;
        clock_gettime(CLOCK_MONOTONIC, &time);
        write(gRecorder, &time, sizeof(timespec));
        gr_write_frame_to_file(gRecorder);
    }
    gr_flip();
    return;
}

void rapidxml::parse_error_handler(const char *what, void *where)
{
    fprintf(stderr, "Parser error: %s\n", what);
    fprintf(stderr, "  Start of string: %s\n", (char*) where);
    abort();
}

static void curtainSet()
{
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
    gr_blit(gCurtain, 0, 0, gr_get_width(gCurtain), gr_get_height(gCurtain), 0, 0);
    gr_flip();
    return;
}

static void curtainRaise(gr_surface surface)
{
	int sy = 0;
    int h = gr_get_height(gCurtain) - 1;
    int w = gr_get_width(gCurtain);
    int fy = 1;

    int msw = gr_get_width(surface);
    int msh = gr_get_height(surface);
	int CURTAIN_RATE = msh / 30;

    if (gNoAnimation == 0)
    {
        for (; h > 0; h -= CURTAIN_RATE, sy += CURTAIN_RATE, fy += CURTAIN_RATE)
        {
            gr_blit(surface, 0, 0, msw, msh, 0, 0);
            gr_blit(gCurtain, 0, sy, w, h, 0, 0);
            gr_flip();
        }
    }
    gr_blit(surface, 0, 0, msw, msh, 0, 0);
    flip();
    return;
}

void curtainClose()
{
#if 0
    int w = gr_get_width(gCurtain);
    int h = 1;
    int sy = gr_get_height(gCurtain) - 1;
    int fbh = gr_fb_height();
	int CURTAIN_RATE = fbh / 30;

    if (gNoAnimation == 0)
    {
        for (; h < fbh; h += CURTAIN_RATE, sy -= CURTAIN_RATE)
        {
            gr_blit(gCurtain, 0, sy, w, h, 0, 0);
            gr_flip();
        }
        gr_blit(gCurtain, 0, 0, gr_get_width(gCurtain), gr_get_height(gCurtain), 0, 0);
        gr_flip();

        if (gRecorder != -1)
            close(gRecorder);

        int fade;
        for (fade = 16; fade < 255; fade += CURTAIN_FADE)
        {
            gr_blit(gCurtain, 0, 0, gr_get_width(gCurtain), gr_get_height(gCurtain), 0, 0);
            gr_color(0, 0, 0, fade);
            gr_fill(0, 0, gr_fb_width(), gr_fb_height());
            gr_flip();
        }
        gr_color(0, 0, 0, 255);
        gr_fill(0, 0, gr_fb_width(), gr_fb_height());
        gr_flip();
    }
#else
    gr_blit(gCurtain, 0, 0, gr_get_width(gCurtain), gr_get_height(gCurtain), 0, 0);
    gr_flip();
#endif
    return;
}

timespec timespec_diff(timespec& start, timespec& end)
{
	timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}

static void *input_thread(void *cookie)
{
    int drag = 0;
	static int touch_and_hold = 0, dontwait = 0, touch_repeat = 0, x = 0, y = 0, lshift = 0, rshift = 0;
	static struct timeval touchStart;

    for (;;) {

        // wait for the next event
        struct input_event ev;
        int state = 0, ret = 0;

		ret = ev_get(&ev, dontwait);

		if (ret < 0) {
			struct timeval curTime;
			gettimeofday(&curTime, NULL);

			long mtime, seconds, useconds;

			seconds  = curTime.tv_sec  - touchStart.tv_sec;
			useconds = curTime.tv_usec - touchStart.tv_usec;

			mtime = ((seconds) * 1000 + useconds/1000.0) + 0.5;
			if (touch_and_hold && mtime > 500) {
				touch_and_hold = 0;
				touch_repeat = 1;
				gettimeofday(&touchStart, NULL);
#ifdef _EVENT_LOGGING
                LOGE("TOUCH_HOLD: %d,%d\n", x, y);
#endif
				PageManager::NotifyTouch(TOUCH_HOLD, x, y);
			}
			if (touch_repeat && mtime > 100) {
#ifdef _EVENT_LOGGING
                LOGE("TOUCH_REPEAT: %d,%d\n", x, y);
#endif
				gettimeofday(&touchStart, NULL);
				PageManager::NotifyTouch(TOUCH_REPEAT, x, y);
			}
		} else if (ev.type == EV_ABS) {

            x = ev.value >> 16;
            y = ev.value & 0xFFFF;

            if (ev.code == 0)
            {
                if (state == 0)
                {
#ifdef _EVENT_LOGGING
                    LOGE("TOUCH_RELEASE: %d,%d\n", x, y);
#endif
                    PageManager::NotifyTouch(TOUCH_RELEASE, x, y);
					touch_and_hold = 0;
					touch_repeat = 0;
					dontwait = 0;
                }
                state = 0;
                drag = 0;
            }
            else
            {
                if (!drag)
                {
#ifdef _EVENT_LOGGING
                    LOGE("TOUCH_START: %d,%d\n", x, y);
#endif
                    if (PageManager::NotifyTouch(TOUCH_START, x, y) > 0)
                        state = 1;
                    drag = 1;
					touch_and_hold = 1;
					dontwait = 1;
					gettimeofday(&touchStart, NULL);
                }
                else
                {
                    if (state == 0)
                    {
#ifdef _EVENT_LOGGING
                        LOGE("TOUCH_DRAG: %d,%d\n", x, y);
#endif
                        if (PageManager::NotifyTouch(TOUCH_DRAG, x, y) > 0)
                            state = 1;
                    }
                }
            }
        }
        else if (ev.type == EV_KEY)
        {
            // Handle key-press here
#ifdef _EVENT_LOGGING
            LOGE("TOUCH_KEY: %d\n", ev.code);
#endif
            if (ev.value != 0) {
				// Key down event
				if (ev.code == KEY_CAPSLOCK) {
					int capslock;

					DataManager::GetValue(TW_SHIFT_KEY, capslock);
					if (capslock && lshift == 0 && rshift == 0)
						DataManager::SetValue(TW_SHIFT_KEY, 0);
					else
						DataManager::SetValue(TW_SHIFT_KEY, 1);
				} else if (ev.code == KEY_LEFTSHIFT) {
					lshift = 1;
					DataManager::SetValue(TW_SHIFT_KEY, 1);
				} else if (ev.code == KEY_RIGHTSHIFT) {
					rshift = 1;
					DataManager::SetValue(TW_SHIFT_KEY, 1);
				} else {
					PageManager::NotifyKey(ev.code);
				}
			} else {
				// This is a key up event for releasing a shift key
				if (ev.code == KEY_LEFTSHIFT) {
					lshift = 0;
				} else if (ev.code == KEY_RIGHTSHIFT) {
					rshift = 0;
				}
				if (lshift == 0 && rshift == 0)
					DataManager::SetValue(TW_SHIFT_KEY, 0);
			}
			touch_and_hold = 0;
			touch_repeat = 0;
			dontwait = 0;
        }
    }
    return NULL;
}

// This special function will return immediately the first time, but then
// always returns 1/30th of a second (or immediately if called later) from
// the last time it was called
static void loopTimer(void)
{
    static timespec lastCall;
    static int initialized = 0;

    if (!initialized)
    {
        clock_gettime(CLOCK_MONOTONIC, &lastCall);
        initialized = 1;
        return;
    }

    do
    {
        timespec curTime;
        clock_gettime(CLOCK_MONOTONIC, &curTime);

        timespec diff = timespec_diff(lastCall, curTime);

        // This is really 30 times per second
        if (diff.tv_sec || diff.tv_nsec > 33333333)
        {
            lastCall = curTime;
            return;
        }

        // We need to sleep some period time microseconds
        unsigned int sleepTime = 33333 - (diff.tv_nsec / 1000);
        usleep(sleepTime);
    } while(1);
    return;
}

static int runPages(void)
{
    // Raise the curtain
    if (gCurtain != NULL)
    {
        gr_surface surface;

        PageManager::Render();
        gr_get_surface(&surface);
        curtainRaise(surface);
        gr_free_surface(surface);
    }

    gGuiRunning = 1;

    DataManager::SetValue("tw_loaded", 1);

    for (;;)
    {
        loopTimer();

        if (!gForceRender)
        {
            int ret;

            ret = PageManager::Update();
            if (ret > 1)
                PageManager::Render();

            if (ret > 0)
                flip();
        }
        else
        {
            gForceRender = 0;
            PageManager::Render();
            flip();
        }
    }

    gGuiRunning = 0;
    return 0;
}

int gui_forceRender(void)
{
    gForceRender = 1;
    return 0;
}

int gui_changePage(std::string newPage)
{
    LOGI("Set page: '%s'\n", newPage.c_str());
    PageManager::ChangePage(newPage);
    gForceRender = 1;
    return 0;
}

int gui_changeOverlay(std::string overlay)
{
    PageManager::ChangeOverlay(overlay);
    gForceRender = 1;
    return 0;
}

int gui_changePackage(std::string newPackage)
{
    PageManager::SelectPackage(newPackage);
    gForceRender = 1;
    return 0;
}

std::string gui_parse_text(string inText)
{
	// Copied from std::string GUIText::parseText(void)
	// This function parses text for DataManager values encompassed by %value% in the XML
	static int counter = 0;
    std::string str = inText;
    size_t pos = 0;
    size_t next = 0, end = 0;

    while (1)
    {
        next = str.find('%', pos);
        if (next == std::string::npos)      return str;
        end = str.find('%', next + 1);
        if (end == std::string::npos)       return str;

        // We have a block of data
        std::string var = str.substr(next + 1, (end - next) - 1);
        str.erase(next, (end - next) + 1);

        if (next + 1 == end)
        {
            str.insert(next, 1, '%');
        }
        else
        {
            std::string value;
            if (DataManager::GetValue(var, value) == 0)
                str.insert(next, value);
        }

        pos = next + 1;
    }
}

extern "C" int gui_init()
{
    int fd;

	gr_init();

    // We need to write out the curtain blob
    if (sizeof(gCurtainBlob) > 32)
    {
		fd = open("/tmp/extract.jpg", O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0)
            return 0;
    
		write(fd, gCurtainBlob, sizeof(gCurtainBlob));
        close(fd);
    
		if (res_create_surface("/tmp/extract.jpg", &gCurtain))
        {
            return -1;
        }
    }
    else
    {
        gNoAnimation = 1;
        if (res_create_surface("bootup", &gCurtain))
            return 0;
    }
    unlink("/tmp/extract.png");

	curtainSet();

	ev_init();
    return 0;
}

extern "C" int gui_loadResources()
{
    // Make sure the sdcard is mounted before we continue
//#ifdef RECOVERY_SDCARD_ON_DATA
    /*mkdir("/mnt/data-sdc", 0777);
    mount(dat.blk, "/mnt/data-sdc", dat.fst, 0, NULL);
    if (symlink("/mnt/data-sdc/media", "/sdcard"))
    {
        LOGE("Unable to symlink (errno %d)\n", errno);
    }*/
//#else
//#endif

//    unlink("/sdcard/video.last");
//    rename("/sdcard/video.bin", "/sdcard/video.last");
//    gRecorder = open("/sdcard/video.bin", O_CREAT | O_WRONLY);

	int check = 0;
	DataManager::GetValue(TW_HAS_CRYPTO, check);
	if (check) {
		if (ensure_path_mounted("/data") < 0) {
			// Data failed to mount - probably encrypted
			DataManager::SetValue(TW_IS_ENCRYPTED, 1);
			DataManager::SetValue(TW_CRYPTO_PASSWORD, "");
			DataManager::SetValue("tw_crypto_display", "");
			if (PageManager::LoadPackage("TWRP", "/res/ui.xml", "decrypt"))
			{
				LOGE("Failed to load base packages.\n");
				goto error;
			} else
				check = 1;
		} else
			check = 0; // Data mounted, not ecrypted, keep going like normal
	}
	if (check == 0 && PageManager::LoadPackage("TWRP", "/script/ui.xml", "main")) {
		std::string theme_path;

		theme_path = DataManager::GetSettingsStoragePath();
		if (ensure_path_mounted(theme_path.c_str()) < 0) {
			int retry_count = 5;
			while (retry_count > 0 && (ensure_path_mounted(theme_path.c_str()) < 0)) {
				usleep(500000);
				ensure_path_mounted(theme_path.c_str());
				retry_count--;
			}
			if (ensure_path_mounted(theme_path.c_str()) < 0) {
				LOGE("Unable to mount %s during GUI startup.\n", theme_path.c_str());
				check = 1;
			}
		}

		theme_path += "/TWRP/theme/ui.zip";
		if (check || PageManager::LoadPackage("TWRP", theme_path, "main"))
		{
			if (PageManager::LoadPackage("TWRP", "/res/ui.xml", "main"))
			{
				LOGE("Failed to load base packages.\n");
				goto error;
			}
		}
	}

    // Set the default package
    PageManager::SelectPackage("TWRP");

    gGuiInitialized = 1;
    return 0;

error:
    LOGE("An internal error has occurred.\n");
    gGuiInitialized = 0;
    return -1;
}

extern "C" int gui_start()
{
    if (!gGuiInitialized)   return -1;

    gGuiConsoleTerminate = 1;
    while (gGuiConsoleRunning)  loopTimer();

    // Set the default package
    PageManager::SelectPackage("TWRP");

    // Start by spinning off an input handler.
    pthread_t t;
    pthread_create(&t, NULL, input_thread, NULL);

    return runPages();
}

static void *console_thread(void *cookie)
{
    PageManager::SwitchToConsole();

    while (!gGuiConsoleTerminate)
    {
        loopTimer();

        if (!gForceRender)
        {
            int ret;

            ret = PageManager::Update();
            if (ret > 1)
                PageManager::Render();

            if (ret > 0)
                flip();

            if (ret < 0)
                LOGE("An update request has failed.\n");
        }
        else
        {
            gForceRender = 0;
            PageManager::Render();
            flip();
        }
    }
    gGuiConsoleRunning = 0;
    return NULL;
}

extern "C" int gui_console_only()
{
    if (!gGuiInitialized)   return -1;

    gGuiConsoleTerminate = 0;
    gGuiConsoleRunning = 1;

    // Start by spinning off an input handler.
    pthread_t t;
    pthread_create(&t, NULL, console_thread, NULL);

    return 0;
}
