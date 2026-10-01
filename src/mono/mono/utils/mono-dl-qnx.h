/**
 * \file
 * QNX: AOT images loaded without dlopen (mono-dl-qnx.c).
 *
 * Copyright (c) Xia Zhongyang.
 * Licensed under the MIT License.
 */

#ifndef __MONO_UTILS_DL_QNX_H__
#define __MONO_UTILS_DL_QNX_H__

#ifdef HOST_QNX

typedef struct _MonoQnxImage MonoQnxImage;

/* Maps the image at path, or returns NULL if it has to be left to dlopen. */
MonoQnxImage *mono_qnx_image_open (const char *path);
void *mono_qnx_image_symbol (MonoQnxImage *image, const char *name);
void mono_qnx_image_close (MonoQnxImage *image);

#endif

#endif /* __MONO_UTILS_DL_QNX_H__ */
