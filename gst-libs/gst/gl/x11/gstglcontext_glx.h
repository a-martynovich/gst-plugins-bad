/*
 * GStreamer
 * Copyright (C) 2012 Matthew Waters <ystreet00@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_GL_CONTEXT_GLX_H__
#define __GST_GL_CONTEXT_GLX_H__

#include "gstglwindow_x11.h"

#include <GL/glx.h>

G_BEGIN_DECLS

#define GST_GL_TYPE_CONTEXT_GLX         (gst_gl_context_glx_get_type())
#define GST_GL_CONTEXT_GLX(o)           (G_TYPE_CHECK_INSTANCE_CAST((o), GST_GL_TYPE_CONTEXT_GLX, GstGLContextGLX))
#define GST_GL_CONTEXT_GLX_CLASS(k)     (G_TYPE_CHECK_CLASS((k), GST_GL_TYPE_CONTEXT_GLX, GstGLContextGLXClass))
#define GST_GL_IS_CONTEXT_GLX(o)        (G_TYPE_CHECK_INSTANCE_TYPE((o), GST_GL_TYPE_CONTEXT_GLX))
#define GST_GL_IS_CONTEXT_GLX_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE((k), GST_GL_TYPE_CONTEXT_GLX))
#define GST_GL_CONTEXT_GLX_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), GST_GL_TYPE_CONTEXT_GLX, GstGLContextGLX_Class))

typedef struct _GstGLContextGLX        GstGLContextGLX;
typedef struct _GstGLContextGLXClass   GstGLContextGLXClass;
typedef struct _GstGLContextGLXPrivate GstGLContextGLXPrivate;

struct _GstGLContextGLX {
  /*< private >*/
  GstGLContext parent;

  GLXContext glx_context;

  GstGLContextGLXPrivate *priv;

  gpointer _reserved[GST_PADDING];
};

struct _GstGLContextGLXClass {
  /*< private >*/
  GstGLContextClass parent_class;

  /*< private >*/
  gpointer _reserved[GST_PADDING];
};

GType gst_gl_context_glx_get_type     (void);

GstGLContextGLX *   gst_gl_context_glx_new                  (void);
guintptr            gst_gl_context_glx_get_current_context  (void);
gpointer            gst_gl_context_glx_get_proc_address     (GstGLAPI gl_api, const gchar * name);

G_END_DECLS

#endif /* __GST_GL_CONTEXT_H__ */
