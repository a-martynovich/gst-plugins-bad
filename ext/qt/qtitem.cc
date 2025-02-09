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

#include <gst/video/video.h>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include "qtitem.h"
#include "gstqsgtexture.h"

#if GST_GL_HAVE_WINDOW_X11 && defined (HAVE_QT_X11)
#include <QX11Info>
#include <gst/gl/x11/gstgldisplay_x11.h>
#include <gst/gl/x11/gstglcontext_glx.h>
#endif

#if GST_GL_HAVE_WINDOW_WAYLAND
#include <gst/gl/wayland/gstgldisplay_wayland.h>
#endif

/**
 * SECTION:gtkgstglwidget
 * @short_description: a #GtkGLArea that renders GStreamer video #GstBuffers
 * @see_also: #GtkGLArea, #GstBuffer
 *
 * #QtGLVideoItem is an #GtkWidget that renders GStreamer video buffers.
 */

#define GST_CAT_DEFAULT qt_item_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define GTK_GST_GL_WIDGET_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    GTK_TYPE_GST_GL_WIDGET, QtGLVideoItemPrivate))

#define DEFAULT_FORCE_ASPECT_RATIO  TRUE
#define DEFAULT_PAR_N               0
#define DEFAULT_PAR_D               1

enum
{
  PROP_0,
  PROP_FORCE_ASPECT_RATIO,
  PROP_PIXEL_ASPECT_RATIO,
};

struct _QtGLVideoItemPrivate
{
  GMutex lock;

  /* properties */
  gboolean force_aspect_ratio;
  gint par_n, par_d;

  gint display_width;
  gint display_height;

  gboolean negotiated;
  GstBuffer *buffer;
  GstCaps *caps;
  GstVideoInfo v_info;

  gboolean initted;
  GstGLDisplay *display;
  QOpenGLContext *qt_context;
  GstGLContext *other_context;
  GstGLContext *context;
};

QtGLVideoItem::QtGLVideoItem()
{
  QGuiApplication *app = dynamic_cast<QGuiApplication *> (QCoreApplication::instance ());
  static volatile gsize _debug;

  g_assert (app != NULL);

  if (g_once_init_enter (&_debug)) {
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "qtglwidget", 0, "Qt GL Widget");
    g_once_init_leave (&_debug, 1);
  }

  this->setFlag (QQuickItem::ItemHasContents, true);

  this->priv = g_new0 (QtGLVideoItemPrivate, 1);

  this->priv->force_aspect_ratio = DEFAULT_FORCE_ASPECT_RATIO;
  this->priv->par_n = DEFAULT_PAR_N;
  this->priv->par_d = DEFAULT_PAR_D;

  g_mutex_init (&this->priv->lock);

#if GST_GL_HAVE_WINDOW_X11 && defined (HAVE_QT_X11)
  if (QString::fromUtf8 ("xcb") == app->platformName())
    this->priv->display = (GstGLDisplay *)
        gst_gl_display_x11_new_with_display (QX11Info::display ());
#endif

  if (!this->priv->display)
    this->priv->display = gst_gl_display_new ();

  connect(this, SIGNAL(windowChanged(QQuickWindow*)), this,
          SLOT(handleWindowChanged(QQuickWindow*)));

  GST_DEBUG ("%p init Qt Video Item", this);
}

QtGLVideoItem::~QtGLVideoItem()
{
  g_mutex_clear (&this->priv->lock);

  g_free (this->priv);
  this->priv = NULL;
}

void
QtGLVideoItem::setDAR(gint num, gint den)
{
  this->priv->par_n = num;
  this->priv->par_d = den;
}

void
QtGLVideoItem::getDAR(gint * num, gint * den)
{
  if (num)
    *num = this->priv->par_n;
  if (den)
    *den = this->priv->par_d;
}

void
QtGLVideoItem::setForceAspectRatio(bool far)
{
  this->priv->force_aspect_ratio = far;
}

bool
QtGLVideoItem::getForceAspectRatio()
{
  return this->priv->force_aspect_ratio;
}

QSGNode *
QtGLVideoItem::updatePaintNode(QSGNode * oldNode,
    UpdatePaintNodeData * updatePaintNodeData)
{
  QSGSimpleTextureNode *texNode = static_cast<QSGSimpleTextureNode *> (oldNode);
  GstVideoRectangle src, dst, result;
  GstQSGTexture *tex;

  g_mutex_lock (&this->priv->lock);

  GST_TRACE ("%p updatePaintNode", this);

  if (!this->priv->caps) {
    g_mutex_unlock (&this->priv->lock);
    return NULL;
  }

  if (!texNode) {
    texNode = new QSGSimpleTextureNode ();
    tex = new GstQSGTexture ();
    texNode->setTexture (tex);
  } else {
    tex = static_cast<GstQSGTexture *> (texNode->texture());
  }

  tex->setCaps (this->priv->caps);
  tex->setBuffer (this->priv->buffer);

  if (this->priv->force_aspect_ratio) {
    src.w = this->priv->display_width;
    src.h = this->priv->display_height;

    dst.x = boundingRect().x();
    dst.y = boundingRect().y();
    dst.w = boundingRect().width();
    dst.h = boundingRect().height();

    gst_video_sink_center_rect (src, dst, &result, TRUE);
  } else {
    result.x = boundingRect().x();
    result.y = boundingRect().y();
    result.w = boundingRect().width();
    result.h = boundingRect().height();
  }

  texNode->setRect (QRectF (result.x, result.y, result.w, result.h));

  g_mutex_unlock (&this->priv->lock);

  return texNode;
}

static void
_reset (QtGLVideoItem * qt_item)
{
  gst_buffer_replace (&qt_item->priv->buffer, NULL);

  gst_caps_replace (&qt_item->priv->caps, NULL);

  qt_item->priv->negotiated = FALSE;
  qt_item->priv->initted = FALSE;
}

void
qt_item_set_buffer (QtGLVideoItem * widget, GstBuffer * buffer)
{
  g_return_if_fail (widget != NULL);
  g_return_if_fail (widget->priv->negotiated);

  g_mutex_lock (&widget->priv->lock);

  gst_buffer_replace (&widget->priv->buffer, buffer);

  QMetaObject::invokeMethod(widget, "update", Qt::QueuedConnection);

  g_mutex_unlock (&widget->priv->lock);
}

void
QtGLVideoItem::onSceneGraphInitialized ()
{
  GstGLPlatform platform;
  GstGLAPI gl_api;
  guintptr gl_handle;

  GST_DEBUG ("scene graph initialization with Qt GL context %p",
      this->window()->openglContext ());

  if (this->priv->qt_context == this->window()->openglContext ())
    return;

  this->priv->qt_context = this->window()->openglContext ();
  if (this->priv->qt_context == NULL) {
    g_assert_not_reached ();
    return;
  }

#if GST_GL_HAVE_WINDOW_X11 && defined (HAVE_QT_X11)
  if (GST_IS_GL_DISPLAY_X11 (this->priv->display)) {
    platform = GST_GL_PLATFORM_GLX;
    gl_api = gst_gl_context_get_current_gl_api (platform, NULL, NULL);
    gl_handle = gst_gl_context_get_current_gl_context (platform);
    if (gl_handle)
      this->priv->other_context =
          gst_gl_context_new_wrapped (this->priv->display, gl_handle,
          platform, gl_api);
  }
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND
  if (GST_IS_GL_DISPLAY_WAYLAND (this->priv->display)) {
    platform = GST_GL_PLATFORM_EGL;
    gl_api = gst_gl_context_get_current_gl_api (platform, NULL, NULL);
    gl_handle = gst_gl_context_get_current_gl_context (platform);
    if (gl_handle)
      this->priv->other_context =
          gst_gl_context_new_wrapped (this->priv->display, gl_handle,
          platform, gl_api);
  }
#endif

  (void) platform;
  (void) gl_api;
  (void) gl_handle;

  if (this->priv->other_context) {
    GError *error = NULL;

    gst_gl_context_activate (this->priv->other_context, TRUE);
    if (!gst_gl_context_fill_info (this->priv->other_context, &error)) {
      GST_ERROR ("%p failed to retreive qt context info: %s", this, error->message);
      g_object_unref (this->priv->other_context);
      this->priv->other_context = NULL;
    } else {
      gst_gl_display_filter_gl_api (this->priv->display, gst_gl_context_get_gl_api (this->priv->other_context));
      gst_gl_context_activate (this->priv->other_context, FALSE);
    }
  }

  GST_DEBUG ("%p created wrapped GL context %" GST_PTR_FORMAT, this,
      this->priv->other_context);
}

void
QtGLVideoItem::onSceneGraphInvalidated ()
{
  GST_FIXME ("%p scene graph invalidated", this);
}

gboolean
qt_item_init_winsys (QtGLVideoItem * widget)
{
  g_return_val_if_fail (widget != NULL, FALSE);

  g_mutex_lock (&widget->priv->lock);

  if (widget->priv->display && widget->priv->qt_context
      && widget->priv->other_context && widget->priv->context) {
    /* already have the necessary state */
    g_mutex_unlock (&widget->priv->lock);
    return TRUE;
  }

  if (!GST_IS_GL_DISPLAY (widget->priv->display)) {
    GST_ERROR ("%p failed to retreive display connection %" GST_PTR_FORMAT,
        widget, widget->priv->display);
    g_mutex_unlock (&widget->priv->lock);
    return FALSE;
  }

  if (!GST_GL_IS_CONTEXT (widget->priv->other_context)) {
    GST_ERROR ("%p failed to retreive wrapped context %" GST_PTR_FORMAT, widget,
        widget->priv->other_context);
    g_mutex_unlock (&widget->priv->lock);
    return FALSE;
  }

  widget->priv->context = gst_gl_context_new (widget->priv->display);

  if (!widget->priv->context) {
    g_mutex_unlock (&widget->priv->lock);
    return FALSE;
  }

  gst_gl_context_create (widget->priv->context, widget->priv->other_context,
      NULL);

  g_mutex_unlock (&widget->priv->lock);
  return TRUE;
}

void
QtGLVideoItem::handleWindowChanged(QQuickWindow *win)
{
  if (win) {
    connect(win, SIGNAL(sceneGraphInitialized()), this, SLOT(onSceneGraphInitialized()), Qt::DirectConnection);
    connect(win, SIGNAL(sceneGraphInvalidated()), this, SLOT(onSceneGraphInvalidated()), Qt::DirectConnection);
  } else {
    this->priv->qt_context = NULL;
  }
}

static gboolean
_calculate_par (QtGLVideoItem * widget, GstVideoInfo * info)
{
  gboolean ok;
  gint width, height;
  gint par_n, par_d;
  gint display_par_n, display_par_d;
  guint display_ratio_num, display_ratio_den;

  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  par_n = GST_VIDEO_INFO_PAR_N (info);
  par_d = GST_VIDEO_INFO_PAR_D (info);

  if (!par_n)
    par_n = 1;

  /* get display's PAR */
  if (widget->priv->par_n != 0 && widget->priv->par_d != 0) {
    display_par_n = widget->priv->par_n;
    display_par_d = widget->priv->par_d;
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  ok = gst_video_calculate_display_ratio (&display_ratio_num,
      &display_ratio_den, width, height, par_n, par_d, display_par_n,
      display_par_d);

  if (!ok)
    return FALSE;

  GST_LOG ("PAR: %u/%u DAR:%u/%u", par_n, par_d, display_par_n, display_par_d);

  if (height % display_ratio_den == 0) {
    GST_DEBUG ("keeping video height");
    widget->priv->display_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    widget->priv->display_height = height;
  } else if (width % display_ratio_num == 0) {
    GST_DEBUG ("keeping video width");
    widget->priv->display_width = width;
    widget->priv->display_height = (guint)
        gst_util_uint64_scale_int (width, display_ratio_den, display_ratio_num);
  } else {
    GST_DEBUG ("approximating while keeping video height");
    widget->priv->display_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    widget->priv->display_height = height;
  }
  GST_DEBUG ("scaling to %dx%d", widget->priv->display_width,
      widget->priv->display_height);

  return TRUE;
}

gboolean
qt_item_set_caps (QtGLVideoItem * widget, GstCaps * caps)
{
  GstVideoInfo v_info;

  g_return_val_if_fail (widget != NULL, FALSE);
  g_return_val_if_fail (GST_IS_CAPS (caps), FALSE);
  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);

  if (widget->priv->caps && gst_caps_is_equal_fixed (widget->priv->caps, caps))
    return TRUE;

  if (!gst_video_info_from_caps (&v_info, caps))
    return FALSE;

  g_mutex_lock (&widget->priv->lock);

  _reset (widget);

  gst_caps_replace (&widget->priv->caps, caps);

  if (!_calculate_par (widget, &v_info)) {
    g_mutex_unlock (&widget->priv->lock);
    return FALSE;
  }

  widget->priv->v_info = v_info;
  widget->priv->negotiated = TRUE;

  g_mutex_unlock (&widget->priv->lock);

  return TRUE;
}

GstGLContext *
qt_item_get_qt_context (QtGLVideoItem * qt_item)
{
  g_return_val_if_fail (qt_item != NULL, NULL);

  if (!qt_item->priv->other_context)
    return NULL;

  return (GstGLContext *) gst_object_ref (qt_item->priv->other_context);
}

GstGLContext *
qt_item_get_context (QtGLVideoItem * qt_item)
{
  g_return_val_if_fail (qt_item != NULL, NULL);

  if (!qt_item->priv->context)
    return NULL;

  return (GstGLContext *) gst_object_ref (qt_item->priv->context);
}

GstGLDisplay *
qt_item_get_display (QtGLVideoItem * qt_item)
{
  g_return_val_if_fail (qt_item != NULL, NULL);

  if (!qt_item->priv->display)
    return NULL;

  return (GstGLDisplay *) gst_object_ref (qt_item->priv->display);
}
