// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

#pragma once

#include "pal_types.h"

// inotify for QNX, which has none: pal_inotify_qnx.c.
intptr_t QnxINotifyInit(void);
int32_t QnxINotifyAddWatch(intptr_t fd, const char* pathName, uint32_t mask);
int32_t QnxINotifyRemoveWatch(intptr_t fd, int32_t wd);
