/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include "gtkgstglwidget.h"
#include <gst/video/video.h>

#if GST_GL_HAVE_WINDOW_X11 && defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#include <gst/gl/x11/gstgldisplay_x11.h>
#include <gst/gl/x11/gstglcontext_glx.h>
#endif

#if GST_GL_HAVE_WINDOW_WAYLAND && defined (GDK_WINDOWING_WAYLAND)
#include <gdk/gdkwayland.h>
#include <gst/gl/wayland/gstgldisplay_wayland.h>
#endif

/**
 * SECTION:gtkgstglwidget
 * @short_description: a #GtkGLArea that renders GStreamer video #GstBuffers
 * @see_also: #GtkGLArea, #GstBuffer
 *
 * #GtkGstGLWidget is an #GtkWidget that renders GStreamer video buffers.
 */

#define GST_CAT_DEFAULT gtk_gst_gl_widget_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

G_DEFINE_TYPE_WITH_CODE (GtkGstGLWidget, gtk_gst_gl_widget, GTK_TYPE_GL_AREA,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "gtkgstglwidget", 0,
        "Gtk Gst GL Widget");
    );

#define GTK_GST_GL_WIDGET_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    GTK_TYPE_GST_GL_WIDGET, GtkGstGLWidgetPrivate))

struct _GtkGstGLWidgetPrivate
{
  gboolean initted;
  GstGLDisplay *display;
  GdkGLContext *gdk_context;
  GstGLContext *other_context;
  GstGLContext *context;
  GstGLUpload *upload;
  GstGLShader *shader;
  GLuint vao;
  GLuint vertex_buffer;
  GLint attr_position;
  GLint attr_texture;
  GLuint current_tex;
};

static const GLfloat vertices[] = {
  1.0f, 1.0f, 0.0f, 1.0f, 0.0f,
  -1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
  -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
  1.0f, -1.0f, 0.0f, 1.0f, 1.0f
};

static void
gtk_gst_gl_widget_bind_buffer (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  const GstGLFuncs *gl = priv->context->gl_vtable;

  gl->BindBuffer (GL_ARRAY_BUFFER, priv->vertex_buffer);

  /* Load the vertex position */
  gl->VertexAttribPointer (priv->attr_position, 3, GL_FLOAT, GL_FALSE,
      5 * sizeof (GLfloat), (void *) 0);

  /* Load the texture coordinate */
  gl->VertexAttribPointer (priv->attr_texture, 2, GL_FLOAT, GL_FALSE,
      5 * sizeof (GLfloat), (void *) (3 * sizeof (GLfloat)));

  gl->EnableVertexAttribArray (priv->attr_position);
  gl->EnableVertexAttribArray (priv->attr_texture);
}

static void
gtk_gst_gl_widget_unbind_buffer (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  const GstGLFuncs *gl = priv->context->gl_vtable;

  gl->BindBuffer (GL_ARRAY_BUFFER, 0);

  gl->DisableVertexAttribArray (priv->attr_position);
  gl->DisableVertexAttribArray (priv->attr_texture);
}

static void
gtk_gst_gl_widget_init_redisplay (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  const GstGLFuncs *gl = priv->context->gl_vtable;

  priv->shader = gst_gl_shader_new (priv->context);

  gst_gl_shader_compile_with_default_vf_and_check (priv->shader,
      &priv->attr_position, &priv->attr_texture);

  if (gl->GenVertexArrays) {
    gl->GenVertexArrays (1, &priv->vao);
    gl->BindVertexArray (priv->vao);
  }

  gl->GenBuffers (1, &priv->vertex_buffer);
  gl->BindBuffer (GL_ARRAY_BUFFER, priv->vertex_buffer);
  gl->BufferData (GL_ARRAY_BUFFER, 4 * 5 * sizeof (GLfloat), vertices,
      GL_STATIC_DRAW);

  if (gl->GenVertexArrays) {
    gtk_gst_gl_widget_bind_buffer (gst_widget);
    gl->BindVertexArray (0);
  }

  gl->BindBuffer (GL_ARRAY_BUFFER, 0);

  priv->initted = TRUE;
}

static void
_redraw_texture (GtkGstGLWidget * gst_widget, guint tex)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  const GstGLFuncs *gl = priv->context->gl_vtable;
  const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

  if (gst_widget->base.force_aspect_ratio) {
    GstVideoRectangle src, dst, result;
    gint widget_width, widget_height, widget_scale;

    gl->ClearColor (0.0, 0.0, 0.0, 0.0);
    gl->Clear (GL_COLOR_BUFFER_BIT);

    widget_scale = gtk_widget_get_scale_factor ((GtkWidget *) gst_widget);
    widget_width = gtk_widget_get_allocated_width ((GtkWidget *) gst_widget);
    widget_height = gtk_widget_get_allocated_height ((GtkWidget *) gst_widget);

    src.x = 0;
    src.y = 0;
    src.w = gst_widget->base.display_width;
    src.h = gst_widget->base.display_height;

    dst.x = 0;
    dst.y = 0;
    dst.w = widget_width * widget_scale;
    dst.h = widget_height * widget_scale;

    gst_video_sink_center_rect (src, dst, &result, TRUE);

    gl->Viewport (result.x, result.y, result.w, result.h);
  }

  gst_gl_shader_use (priv->shader);

  if (gl->BindVertexArray)
    gl->BindVertexArray (priv->vao);
  else
    gtk_gst_gl_widget_bind_buffer (gst_widget);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, tex);
  gst_gl_shader_set_uniform_1i (priv->shader, "tex", 0);

  gl->DrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

  if (gl->BindVertexArray)
    gl->BindVertexArray (0);
  else
    gtk_gst_gl_widget_unbind_buffer (gst_widget);

  gl->BindTexture (GL_TEXTURE_2D, 0);
}

static gboolean
gtk_gst_gl_widget_render (GtkGLArea * widget, GdkGLContext * context)
{
  GtkGstGLWidgetPrivate *priv = GTK_GST_GL_WIDGET (widget)->priv;
  GtkGstBaseWidget *base_widget = GTK_GST_BASE_WIDGET (widget);

  GTK_GST_BASE_WIDGET_LOCK (widget);

  if (!priv->initted && priv->context)
    gtk_gst_gl_widget_init_redisplay (GTK_GST_GL_WIDGET (widget));

  if (priv->initted && base_widget->negotiated && base_widget->buffer) {
    GST_DEBUG ("rendering buffer %p with gdk context %p",
        base_widget->buffer, context);

    gst_gl_context_activate (priv->other_context, TRUE);

    if (base_widget->new_buffer || priv->current_tex == 0) {
      GstVideoFrame gl_frame;
      GstGLSyncMeta *sync_meta;

      if (!gst_video_frame_map (&gl_frame, &base_widget->v_info,
              base_widget->buffer, GST_MAP_READ | GST_MAP_GL)) {
        goto error;
      }

      sync_meta = gst_buffer_get_gl_sync_meta (base_widget->buffer);
      if (sync_meta) {
        gst_gl_sync_meta_set_sync_point (sync_meta, priv->context);
        gst_gl_sync_meta_wait (sync_meta, priv->other_context);
      }

      priv->current_tex = *(guint *) gl_frame.data[0];

      gst_video_frame_unmap (&gl_frame);
    }

    _redraw_texture (GTK_GST_GL_WIDGET (widget), priv->current_tex);
    base_widget->new_buffer = FALSE;
  } else {
  error:
    /* FIXME: nothing to display */
    glClearColor (0.0, 0.0, 0.0, 0.0);
    glClear (GL_COLOR_BUFFER_BIT);
  }

  if (priv->other_context)
    gst_gl_context_activate (priv->other_context, FALSE);

  GTK_GST_BASE_WIDGET_UNLOCK (widget);
  return FALSE;
}

typedef void (*ThreadFunc) (gpointer data);

struct invoke_context
{
  ThreadFunc func;
  gpointer data;
  GMutex lock;
  GCond cond;
  gboolean fired;
};

static gboolean
_invoke_func (struct invoke_context *info)
{
  g_mutex_lock (&info->lock);
  info->func (info->data);
  info->fired = TRUE;
  g_cond_signal (&info->cond);
  g_mutex_unlock (&info->lock);

  return G_SOURCE_REMOVE;
}

static void
_invoke_on_main (ThreadFunc func, gpointer data)
{
  GMainContext *main_context = g_main_context_default ();
  struct invoke_context info;

  g_mutex_init (&info.lock);
  g_cond_init (&info.cond);
  info.fired = FALSE;
  info.func = func;
  info.data = data;

  g_main_context_invoke (main_context, (GSourceFunc) _invoke_func, &info);

  g_mutex_lock (&info.lock);
  while (!info.fired)
    g_cond_wait (&info.cond, &info.lock);
  g_mutex_unlock (&info.lock);

  g_mutex_clear (&info.lock);
  g_cond_clear (&info.cond);
}

static void
_reset_gl (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  const GstGLFuncs *gl = priv->other_context->gl_vtable;

  if (!priv->gdk_context)
    priv->gdk_context = gtk_gl_area_get_context (GTK_GL_AREA (gst_widget));

  if (priv->gdk_context == NULL)
    return;

  gdk_gl_context_make_current (priv->gdk_context);
  gst_gl_context_activate (priv->other_context, TRUE);

  if (priv->vao) {
    gl->DeleteVertexArrays (1, &priv->vao);
    priv->vao = 0;
  }

  if (priv->vertex_buffer) {
    gl->DeleteBuffers (1, &priv->vertex_buffer);
    priv->vertex_buffer = 0;
  }

  if (priv->upload) {
    gst_object_unref (priv->upload);
    priv->upload = NULL;
  }

  if (priv->shader) {
    gst_object_unref (priv->shader);
    priv->shader = NULL;
  }

  gst_gl_context_activate (priv->other_context, FALSE);

  gst_object_unref (priv->other_context);
  priv->other_context = NULL;

  gdk_gl_context_clear_current ();

  g_object_unref (priv->gdk_context);
  priv->gdk_context = NULL;
}

static void
_reset (GtkGstBaseWidget * base_widget)
{
  GtkGstGLWidgetPrivate *priv = GTK_GST_GL_WIDGET (base_widget)->priv;

  priv->initted = FALSE;
  priv->vao = 0;
  priv->vertex_buffer = 0;
  priv->attr_position = 0;
  priv->attr_texture = 0;
  priv->current_tex = 0;

  gtk_gl_area_set_has_alpha (GTK_GL_AREA (base_widget),
      !base_widget->ignore_alpha);
}

/* called from main thread */
static void
gtk_gst_gl_widget_reset (GtkGstBaseWidget * base_widget)
{
  GtkGstGLWidgetPrivate *priv = GTK_GST_GL_WIDGET (base_widget)->priv;
  const GstGLFuncs *gl = priv->other_context->gl_vtable;

  _reset (base_widget);

  if (priv->vao) {
    gl->DeleteVertexArrays (1, &priv->vao);
    priv->vao = 0;
  }

  if (priv->vertex_buffer) {
    gl->DeleteBuffers (1, &priv->vertex_buffer);
    priv->vertex_buffer = 0;
  }
}

static void
gtk_gst_gl_widget_finalize (GObject * object)
{
  GtkGstGLWidgetPrivate *priv = GTK_GST_GL_WIDGET (object)->priv;
  GtkGstBaseWidget *base_widget = GTK_GST_BASE_WIDGET (object);

  _reset (base_widget);

  if (priv->other_context)
    _invoke_on_main ((ThreadFunc) _reset_gl, base_widget);

  if (priv->context)
    gst_object_unref (priv->context);

  if (priv->display)
    gst_object_unref (priv->display);

  gtk_gst_base_widget_finalize (object);
  G_OBJECT_CLASS (gtk_gst_gl_widget_parent_class)->finalize (object);
}

static void
gtk_gst_gl_widget_class_init (GtkGstGLWidgetClass * klass)
{
  GObjectClass *gobject_klass = (GObjectClass *) klass;
  GtkGLAreaClass *gl_widget_klass = (GtkGLAreaClass *) klass;
  GtkGstBaseWidget *base_widget_klass = (GtkGstBaseWidget *) klass;

  g_type_class_add_private (klass, sizeof (GtkGstGLWidgetPrivate));
  gtk_gst_base_widget_class_init (GTK_GST_BASE_WIDGET_CLASS (klass));

  gobject_klass->finalize = gtk_gst_gl_widget_finalize;
  gl_widget_klass->render = gtk_gst_gl_widget_render;
  base_widget_klass->reset = gtk_gst_gl_widget_reset;
}

static void
gtk_gst_gl_widget_init (GtkGstGLWidget * gst_widget)
{
  GtkGstBaseWidget *base_widget = GTK_GST_BASE_WIDGET (gst_widget);
  GdkDisplay *display;
  GtkGstGLWidgetPrivate *priv;

  gtk_gst_base_widget_init (base_widget);

  gst_widget->priv = priv = GTK_GST_GL_WIDGET_GET_PRIVATE (gst_widget);

  display = gdk_display_get_default ();

#if GST_GL_HAVE_WINDOW_X11 && defined (GDK_WINDOWING_X11)
  if (GDK_IS_X11_DISPLAY (display))
    priv->display = (GstGLDisplay *)
        gst_gl_display_x11_new_with_display (gdk_x11_display_get_xdisplay
        (display));
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND && defined (GDK_WINDOWING_WAYLAND)
  if (GDK_IS_WAYLAND_DISPLAY (display)) {
    struct wl_display *wayland_display =
        gdk_wayland_display_get_wl_display (display);
    priv->display = (GstGLDisplay *)
        gst_gl_display_wayland_new_with_display (wayland_display);
  }
#endif

  (void) display;

  if (!priv->display)
    priv->display = gst_gl_display_new ();

  gtk_gl_area_set_has_alpha (GTK_GL_AREA (gst_widget),
      !base_widget->ignore_alpha);
}



static void
_get_gl_context (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;
  GstGLPlatform platform;
  GstGLAPI gl_api;
  guintptr gl_handle;

  gtk_widget_realize (GTK_WIDGET (gst_widget));

  if (priv->gdk_context)
    g_object_unref (priv->gdk_context);
  priv->gdk_context = gtk_gl_area_get_context (GTK_GL_AREA (gst_widget));
  if (priv->gdk_context == NULL) {
    GError *error = gtk_gl_area_get_error (GTK_GL_AREA (gst_widget));

    GST_ERROR_OBJECT (gst_widget, "Error creating GdkGLContext : %s",
        error ? error->message : "No error set by Gdk");
    g_assert_not_reached ();
    return;
  }

  g_object_ref (priv->gdk_context);

  gdk_gl_context_make_current (priv->gdk_context);

#if GST_GL_HAVE_WINDOW_X11 && defined (GDK_WINDOWING_X11)
  if (GST_IS_GL_DISPLAY_X11 (priv->display)) {
    platform = GST_GL_PLATFORM_GLX;
    gl_api = gst_gl_context_get_current_gl_api (platform, NULL, NULL);
    gl_handle = gst_gl_context_get_current_gl_context (platform);
    if (gl_handle)
      priv->other_context =
          gst_gl_context_new_wrapped (priv->display, gl_handle,
          platform, gl_api);
  }
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND && defined (GDK_WINDOWING_WAYLAND)
  if (GST_IS_GL_DISPLAY_WAYLAND (priv->display)) {
    platform = GST_GL_PLATFORM_EGL;
    gl_api = gst_gl_context_get_current_gl_api (platform, NULL, NULL);
    gl_handle = gst_gl_context_get_current_gl_context (platform);
    if (gl_handle)
      priv->other_context =
          gst_gl_context_new_wrapped (priv->display, gl_handle,
          platform, gl_api);
  }
#endif

  (void) platform;
  (void) gl_api;
  (void) gl_handle;

  if (priv->other_context) {
    GError *error = NULL;

    gst_gl_context_activate (priv->other_context, TRUE);
    if (!gst_gl_context_fill_info (priv->other_context, &error)) {
      GST_ERROR ("failed to retreive gdk context info: %s", error->message);
      g_object_unref (priv->other_context);
      priv->other_context = NULL;
    } else {
      gst_gl_context_activate (priv->other_context, FALSE);
    }
  }
}

GtkWidget *
gtk_gst_gl_widget_new (void)
{
  return (GtkWidget *) g_object_new (GTK_TYPE_GST_GL_WIDGET, NULL);
}

gboolean
gtk_gst_gl_widget_init_winsys (GtkGstGLWidget * gst_widget)
{
  GtkGstGLWidgetPrivate *priv = gst_widget->priv;

  g_return_val_if_fail (GTK_IS_GST_GL_WIDGET (gst_widget), FALSE);

  GTK_GST_BASE_WIDGET_LOCK (gst_widget);

  if (priv->display && priv->gdk_context && priv->other_context) {
    GTK_GST_BASE_WIDGET_UNLOCK (gst_widget);
    return TRUE;
  }

  if (!priv->other_context) {
    GTK_GST_BASE_WIDGET_UNLOCK (gst_widget);
    _invoke_on_main ((ThreadFunc) _get_gl_context, gst_widget);
    GTK_GST_BASE_WIDGET_LOCK (gst_widget);
  }

  if (!GST_GL_IS_CONTEXT (priv->other_context)) {
    GTK_GST_BASE_WIDGET_UNLOCK (gst_widget);
    return FALSE;
  }

  priv->context = gst_gl_context_new (priv->display);

  if (!priv->context) {
    GTK_GST_BASE_WIDGET_UNLOCK (gst_widget);
    return FALSE;
  }

  gst_gl_context_create (priv->context, priv->other_context, NULL);

  GTK_GST_BASE_WIDGET_UNLOCK (gst_widget);
  return TRUE;
}

GstGLContext *
gtk_gst_gl_widget_get_gtk_context (GtkGstGLWidget * gst_widget)
{
  if (!gst_widget->priv->other_context)
    return NULL;

  return gst_object_ref (gst_widget->priv->other_context);
}

GstGLContext *
gtk_gst_gl_widget_get_context (GtkGstGLWidget * gst_widget)
{
  if (!gst_widget->priv->context)
    return NULL;

  return gst_object_ref (gst_widget->priv->context);
}

GstGLDisplay *
gtk_gst_gl_widget_get_display (GtkGstGLWidget * gst_widget)
{
  if (!gst_widget->priv->display)
    return NULL;

  return gst_object_ref (gst_widget->priv->display);
}
