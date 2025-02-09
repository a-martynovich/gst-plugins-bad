/*
 * GStreamer
 * Copyright (C) 2013 Matthew Waters <ystreet00@gmail.com>
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

/**
 * SECTION:gstglcontext
 * @short_description: OpenGL context abstraction
 * @title: GstGLContext
 * @see_also: #GstGLDisplay, #GstGLWindow
 *
 * #GstGLContext wraps an OpenGL context object in a uniform API.  As a result
 * of the limitation on OpenGL context, this object is not thread safe unless
 * specified and must only be activated in a single thread.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#if defined(ANDROID) || defined(__ANDROID__)
/* Avoid a linker error with _isoc99_sscanf() when building a shared library
 * for android
 */
#define _GNU_SOURCE
#endif

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x0000821d
#endif

#include <gmodule.h>

#include "gl.h"
#include "gstglcontext.h"

#if GST_GL_HAVE_PLATFORM_GLX
#include "x11/gstglcontext_glx.h"
#endif
#if GST_GL_HAVE_PLATFORM_EGL
#include "egl/gstglcontext_egl.h"
#endif
#if GST_GL_HAVE_PLATFORM_CGL
#include "cocoa/gstglcontext_cocoa.h"
#endif
#if GST_GL_HAVE_PLATFORM_WGL
#include "win32/gstglcontext_wgl.h"
#endif
#if GST_GL_HAVE_PLATFORM_EAGL
#include "eagl/gstglcontext_eagl.h"
#endif

GST_DEBUG_CATEGORY_STATIC (gst_performance);
static GPrivate current_context_key;

static GModule *module_self;
static GOnce module_self_gonce = G_ONCE_INIT;

#if GST_GL_HAVE_OPENGL
static GOnce module_opengl_gonce = G_ONCE_INIT;
static GModule *module_opengl;

static gpointer
load_opengl_module (gpointer user_data)
{
#ifdef GST_GL_LIBGL_MODULE_NAME
  module_opengl = g_module_open (GST_GL_LIBGL_MODULE_NAME, G_MODULE_BIND_LAZY);
#else
  /* On Linux the .so is only in -dev packages, try with a real soname
   * Proper compilers will optimize away the strcmp */
  if (strcmp (G_MODULE_SUFFIX, "so") == 0)
    module_opengl = g_module_open ("libGL.so.1", G_MODULE_BIND_LAZY);

  /* This automatically handles the suffix and even .la files */
  if (!module_opengl)
    module_opengl = g_module_open ("libGL", G_MODULE_BIND_LAZY);
#endif

  return NULL;
}
#endif

#if GST_GL_HAVE_GLES2
static GOnce module_gles2_gonce = G_ONCE_INIT;
static GModule *module_gles2;

static gpointer
load_gles2_module (gpointer user_data)
{
#ifdef GST_GL_LIBGLESV2_MODULE_NAME
  module_gles2 =
      g_module_open (GST_GL_LIBGLESV2_MODULE_NAME, G_MODULE_BIND_LAZY);
#else
  /* On Linux the .so is only in -dev packages, try with a real soname
   * Proper compilers will optimize away the strcmp */
  if (strcmp (G_MODULE_SUFFIX, "so") == 0)
    module_gles2 = g_module_open ("libGLESv2.so.2", G_MODULE_BIND_LAZY);

  /* This automatically handles the suffix and even .la files */
  if (!module_gles2)
    module_gles2 = g_module_open ("libGLESv2", G_MODULE_BIND_LAZY);

#endif

  return NULL;
}
#endif

static gpointer
load_self_module (gpointer user_data)
{
  module_self = g_module_open (NULL, G_MODULE_BIND_LAZY);

  return NULL;
}

#if GST_GL_HAVE_GLES3
#error "Add module loading support for GLES3"
#endif

/* Context sharedness es tracked by a unique id stored in each context object
 * in order track complex creation/deletion scenarios.  As a result, sharedness
 * can only be successfully validated between two GstGLContext's where one is
 * not a wrapped context.
 *
 * As there is no API at the winsys level to tell whether two OpenGL contexts
 * can share GL resources, this is the next best thing.
 */
static volatile guint sharegroup_idx;

static guint
_new_sharegroup_id (void)
{
  guint current, ret;

  do {
    current = g_atomic_int_get (&sharegroup_idx);
    ret = current + 1;

    /* 0 is special */
    if (ret == 0)
      ret++;
  } while (!g_atomic_int_compare_and_exchange (&sharegroup_idx, current, ret));

  GST_TRACE ("generated new share group id %u", ret);

  return ret;
}

#define GST_CAT_DEFAULT gst_gl_context_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define gst_gl_context_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE (GstGLContext, gst_gl_context, GST_TYPE_OBJECT);

#define GST_GL_CONTEXT_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE((o), GST_GL_TYPE_CONTEXT, GstGLContextPrivate))

static void _init_debug (void);

static gpointer gst_gl_context_create_thread (GstGLContext * context);
static void gst_gl_context_finalize (GObject * object);

struct _GstGLContextPrivate
{
  GThread *gl_thread;
  GThread *active_thread;

  /* conditions */
  GMutex render_lock;
  GCond create_cond;
  GCond destroy_cond;

  gboolean created;
  gboolean alive;

  GWeakRef other_context_ref;
  guint sharegroup_id;
  GError **error;

  gint gl_major;
  gint gl_minor;

  gchar *gl_exts;
};

typedef struct
{
  GstGLContext parent;

  guintptr handle;
  GstGLPlatform platform;
  GstGLAPI available_apis;
} GstGLWrappedContext;

typedef struct
{
  GstGLContextClass parent;
} GstGLWrappedContextClass;

#define GST_GL_TYPE_WRAPPED_CONTEXT (gst_gl_wrapped_context_get_type())
GType gst_gl_wrapped_context_get_type (void);
G_DEFINE_TYPE (GstGLWrappedContext, gst_gl_wrapped_context,
    GST_GL_TYPE_CONTEXT);

#define GST_GL_WRAPPED_CONTEXT(o)           (G_TYPE_CHECK_INSTANCE_CAST((o), GST_GL_TYPE_WRAPPED_CONTEXT, GstGLWrappedContext))
#define GST_GL_WRAPPED_CONTEXT_CLASS(k)     (G_TYPE_CHECK_CLASS((k), GST_GL_TYPE_CONTEXT, GstGLContextClass))
#define GST_GL_IS_WRAPPED_CONTEXT(o)        (G_TYPE_CHECK_INSTANCE_TYPE((o), GST_GL_TYPE_WRAPPED_CONTEXT))
#define GST_GL_IS_WRAPPED_CONTEXT_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE((k), GST_GL_TYPE_WRAPPED_CONTEXT))
#define GST_GL_WRAPPED_CONTEXT_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), GST_GL_TYPE_WRAPPED_CONTEXT, GstGLWrappedContextClass))

GQuark
gst_gl_context_error_quark (void)
{
  return g_quark_from_static_string ("gst-gl-context-error-quark");
}

static void
_ensure_window (GstGLContext * context)
{
  GstGLWindow *window;

  if (context->window)
    return;

  window = gst_gl_window_new (context->display);

  gst_gl_context_set_window (context, window);

  gst_object_unref (window);
}

static void
gst_gl_context_init (GstGLContext * context)
{
  context->priv = GST_GL_CONTEXT_GET_PRIVATE (context);

  context->window = NULL;
  context->gl_vtable = g_slice_alloc0 (sizeof (GstGLFuncs));

  g_mutex_init (&context->priv->render_lock);

  g_cond_init (&context->priv->create_cond);
  g_cond_init (&context->priv->destroy_cond);
  context->priv->created = FALSE;

  g_weak_ref_init (&context->priv->other_context_ref, NULL);
}

static void
gst_gl_context_class_init (GstGLContextClass * klass)
{
  g_type_class_add_private (klass, sizeof (GstGLContextPrivate));

  klass->get_proc_address =
      GST_DEBUG_FUNCPTR (gst_gl_context_default_get_proc_address);

  G_OBJECT_CLASS (klass)->finalize = gst_gl_context_finalize;

  _init_debug ();
}

static void
_init_debug (void)
{
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_gl_context_debug, "glcontext", 0,
        "glcontext element");
    GST_DEBUG_CATEGORY_GET (gst_performance, "GST_PERFORMANCE");
    g_once_init_leave (&_init, 1);
  }
}

/**
 * gst_gl_context_new:
 * @display: a #GstGLDisplay
 *
 * Create a new #GstGLContext with the specified @display
 *
 * Returns: a new #GstGLContext
 *
 * Since: 1.4
 */
GstGLContext *
gst_gl_context_new (GstGLDisplay * display)
{
  GstGLContext *context = NULL;
  const gchar *user_choice;

  _init_debug ();

  user_choice = g_getenv ("GST_GL_PLATFORM");
  GST_INFO ("creating a context for display %" GST_PTR_FORMAT
      ", user choice:%s", display, user_choice);
#if GST_GL_HAVE_PLATFORM_CGL
  if (!context && (!user_choice || g_strstr_len (user_choice, 5, "cgl")))
    context = GST_GL_CONTEXT (gst_gl_context_cocoa_new ());
#endif
#if GST_GL_HAVE_PLATFORM_GLX
  if (!context && (!user_choice || g_strstr_len (user_choice, 3, "glx")))
    context = GST_GL_CONTEXT (gst_gl_context_glx_new ());
#endif
#if GST_GL_HAVE_PLATFORM_EGL
  if (!context && (!user_choice || g_strstr_len (user_choice, 7, "egl")))
    context = GST_GL_CONTEXT (gst_gl_context_egl_new ());
#endif
#if GST_GL_HAVE_PLATFORM_WGL
  if (!context && (!user_choice || g_strstr_len (user_choice, 3, "wgl"))) {
    context = GST_GL_CONTEXT (gst_gl_context_wgl_new ());
  }
#endif
#if GST_GL_HAVE_PLATFORM_EAGL
  if (!context && (!user_choice || g_strstr_len (user_choice, 5, "eagl")))
    context = GST_GL_CONTEXT (gst_gl_context_eagl_new ());
#endif

  if (!context) {
    /* subclass returned a NULL context */
    GST_WARNING ("Could not create context. user specified %s",
        user_choice ? user_choice : "(null)");

    return NULL;
  }

  context->display = gst_object_ref (display);

  GST_DEBUG_OBJECT (context,
      "Done creating context for display %" GST_PTR_FORMAT " (user_choice:%s)",
      display, user_choice);

  return context;
}

/**
 * gst_gl_context_new_wrapped:
 * @display: a #GstGLDisplay
 * @handle: the OpenGL context to wrap
 * @context_type: a #GstGLPlatform specifying the type of context in @handle
 * @available_apis: a #GstGLAPI containing the available OpenGL apis in @handle
 *
 * Wraps an existing OpenGL context into a #GstGLContext.  
 *
 * Returns: a #GstGLContext wrapping @handle
 *
 * Since: 1.4
 */
GstGLContext *
gst_gl_context_new_wrapped (GstGLDisplay * display, guintptr handle,
    GstGLPlatform context_type, GstGLAPI available_apis)
{
  GstGLContext *context;
  GstGLWrappedContext *context_wrap = NULL;
  GstGLContextClass *context_class;
  GstGLAPI display_api;

  _init_debug ();

  display_api = gst_gl_display_get_gl_api (display);
  g_return_val_if_fail ((display_api & available_apis) != GST_GL_API_NONE,
      NULL);

  context_wrap = g_object_new (GST_GL_TYPE_WRAPPED_CONTEXT, NULL);

  if (!context_wrap) {
    /* subclass returned a NULL context */
    GST_ERROR ("Could not wrap existing context");

    return NULL;
  }

  context = (GstGLContext *) context_wrap;

  context->display = gst_object_ref (display);
  context->priv->sharegroup_id = _new_sharegroup_id ();
  context_wrap->handle = handle;
  context_wrap->platform = context_type;
  context_wrap->available_apis = available_apis;

  context_class = GST_GL_CONTEXT_GET_CLASS (context);

#if GST_GL_HAVE_PLATFORM_GLX
  if (context_type == GST_GL_PLATFORM_GLX) {
    context_class->get_current_context = gst_gl_context_glx_get_current_context;
    context_class->get_proc_address = gst_gl_context_glx_get_proc_address;
  }
#endif
#if GST_GL_HAVE_PLATFORM_EGL
  if (context_type == GST_GL_PLATFORM_EGL) {
    context_class->get_current_context = gst_gl_context_egl_get_current_context;
    context_class->get_proc_address = gst_gl_context_egl_get_proc_address;
  }
#endif
#if GST_GL_HAVE_PLATFORM_CGL
  if (context_type == GST_GL_PLATFORM_CGL) {
    context_class->get_current_context =
        gst_gl_context_cocoa_get_current_context;
    context_class->get_proc_address = gst_gl_context_default_get_proc_address;
  }
#endif
#if GST_GL_HAVE_PLATFORM_WGL
  if (context_type == GST_GL_PLATFORM_WGL) {
    context_class->get_current_context = gst_gl_context_wgl_get_current_context;
    context_class->get_proc_address = gst_gl_context_wgl_get_proc_address;
  }
#endif
#if GST_GL_HAVE_PLATFORM_EAGL
  if (context_type == GST_GL_PLATFORM_EAGL) {
    context_class->get_current_context =
        gst_gl_context_eagl_get_current_context;
    context_class->get_proc_address = gst_gl_context_default_get_proc_address;
  }
#endif

  if (!context_class->get_current_context) {
    /* we don't have API support */
    gst_object_unref (context);
    return NULL;
  }

  return context;
}

/**
 * gst_gl_context_get_current_gl_context:
 * @context_type: a #GstGLPlatform specifying the type of context to retrieve
 *
 * Returns: The OpenGL context handle current in the calling thread or %NULL
 *
 * Since: 1.6
 */
guintptr
gst_gl_context_get_current_gl_context (GstGLPlatform context_type)
{
  guintptr handle = 0;

  _init_debug ();

#if GST_GL_HAVE_PLATFORM_GLX
  if (!handle && (context_type & GST_GL_PLATFORM_GLX) != 0)
    handle = gst_gl_context_glx_get_current_context ();
#endif
#if GST_GL_HAVE_PLATFORM_EGL
  if (!handle && (context_type & GST_GL_PLATFORM_EGL) != 0)
    handle = gst_gl_context_egl_get_current_context ();
#endif
#if GST_GL_HAVE_PLATFORM_CGL
  if (!handle && (context_type & GST_GL_PLATFORM_CGL) != 0)
    handle = gst_gl_context_cocoa_get_current_context ();
#endif
#if GST_GL_HAVE_PLATFORM_WGL
  if (!handle && (context_type & GST_GL_PLATFORM_WGL) != 0)
    handle = gst_gl_context_wgl_get_current_context ();
#endif
#if GST_GL_HAVE_PLATFORM_EAGL
  if (!handle && (context_type & GST_GL_PLATFORM_EAGL) != 0)
    handle = gst_gl_context_eagl_get_current_context ();
#endif

  if (!handle)
    GST_WARNING ("Could not retrieve current context");

  return handle;
}

gpointer
gst_gl_context_get_proc_address_with_platform (GstGLPlatform context_type,
    GstGLAPI gl_api, const gchar * name)
{
  gpointer ret = NULL;

#if GST_GL_HAVE_PLATFORM_GLX
  if (!ret && (context_type & GST_GL_PLATFORM_GLX) != 0)
    ret = gst_gl_context_glx_get_proc_address (gl_api, name);
#endif
#if GST_GL_HAVE_PLATFORM_EGL
  if (!ret && (context_type & GST_GL_PLATFORM_EGL) != 0)
    ret = gst_gl_context_egl_get_proc_address (gl_api, name);
#endif
#if GST_GL_HAVE_PLATFORM_WGL
  if (!ret && (context_type & GST_GL_PLATFORM_WGL) != 0)
    ret = gst_gl_context_wgl_get_proc_address (gl_api, name);
#endif
  /* CGL and EAGL rely on the default impl */

  if (!ret)
    ret = gst_gl_context_default_get_proc_address (gl_api, name);

  return ret;
}

/**
 * gst_gl_context_get_current_gl_api:
 * @platform: the #GstGLPlatform to retreive the API for
 * @major: (out): (allow-none): the major version
 * @minor: (out): (allow-none): the minor version
 *
 * If an error occurs, @major and @minor aren't modified and %GST_GL_API_NONE is
 * returned.
 *
 * Returns: The version supported by the OpenGL context current in the calling
 *          thread or %GST_GL_API_NONE
 *
 * Since: 1.6
 */
GstGLAPI
gst_gl_context_get_current_gl_api (GstGLPlatform platform, guint * major,
    guint * minor)
{
  const GLubyte *(*GetString) (GLenum name);
#if GST_GL_HAVE_OPENGL
  void (*GetIntegerv) (GLenum name, GLuint * n);
#endif
  const gchar *version;
  gint maj, min, n;
  GstGLAPI ret = (1 << 31);

  _init_debug ();

  while (ret != GST_GL_API_NONE) {
    /* FIXME: attempt to delve into the platform specific GetProcAddress */
    GetString =
        gst_gl_context_get_proc_address_with_platform (platform, ret,
        "glGetString");
#if GST_GL_HAVE_OPENGL
    GetIntegerv =
        gst_gl_context_get_proc_address_with_platform (platform, ret,
        "glGetIntegerv");
#endif
    if (!GetString) {
      goto next;
    }

    version = (const gchar *) GetString (GL_VERSION);
    if (!version)
      goto next;

    /* strlen (x.x) == 3 */
    n = strlen (version);
    if (n < 3)
      goto next;

    if (g_strstr_len (version, 9, "OpenGL ES")) {
      /* strlen (OpenGL ES x.x) == 13 */
      if (n < 13)
        goto next;

      sscanf (&version[10], "%d.%d", &maj, &min);

      if (maj <= 0 || min < 0)
        goto next;

      if (maj == 1) {
        ret = GST_GL_API_GLES1;
        break;
      } else if (maj == 2 || maj == 3) {
        ret = GST_GL_API_GLES2;
        break;
      }

      goto next;
    } else {
      sscanf (version, "%d.%d", &maj, &min);

      if (maj <= 0 || min < 0)
        goto next;

#if GST_GL_HAVE_OPENGL
      if (GetIntegerv && (maj > 3 || (maj == 3 && min > 1))) {
        GLuint context_flags = 0;

        ret = GST_GL_API_NONE;
        GetIntegerv (GL_CONTEXT_PROFILE_MASK, &context_flags);
        if (context_flags & GL_CONTEXT_CORE_PROFILE_BIT)
          ret |= GST_GL_API_OPENGL3;
        if (context_flags & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT)
          ret |= GST_GL_API_OPENGL;
        break;
      }
#endif
      ret = GST_GL_API_OPENGL;
      break;
    }

  next:
    /* iterate through the apis */
    ret >>= 1;
  }

  if (ret == GST_GL_API_NONE)
    return GST_GL_API_NONE;

  if (major)
    *major = maj;
  if (minor)
    *minor = min;

  return ret;
}

static void
gst_gl_context_finalize (GObject * object)
{
  GstGLContext *context = GST_GL_CONTEXT (object);

  if (context->window) {
    gst_gl_window_set_resize_callback (context->window, NULL, NULL, NULL);
    gst_gl_window_set_draw_callback (context->window, NULL, NULL, NULL);

    if (context->priv->alive) {
      g_mutex_lock (&context->priv->render_lock);
      GST_INFO_OBJECT (context, "send quit gl window loop");
      gst_gl_window_quit (context->window);
      while (context->priv->alive) {
        g_cond_wait (&context->priv->destroy_cond, &context->priv->render_lock);
      }
      g_mutex_unlock (&context->priv->render_lock);
    }

    gst_gl_window_set_close_callback (context->window, NULL, NULL, NULL);

    if (context->priv->gl_thread) {
      gpointer ret = g_thread_join (context->priv->gl_thread);
      GST_INFO_OBJECT (context, "gl thread joined");
      if (ret != NULL)
        GST_ERROR_OBJECT (context, "gl thread returned a non-null pointer");
      context->priv->gl_thread = NULL;
    }

    gst_object_unref (context->window);
  }

  gst_object_unref (context->display);

  if (context->gl_vtable) {
    g_slice_free (GstGLFuncs, context->gl_vtable);
    context->gl_vtable = NULL;
  }

  g_mutex_clear (&context->priv->render_lock);

  g_cond_clear (&context->priv->destroy_cond);
  g_cond_clear (&context->priv->create_cond);

  g_free (context->priv->gl_exts);
  g_weak_ref_clear (&context->priv->other_context_ref);

  GST_DEBUG_OBJECT (context, "End of finalize");
  G_OBJECT_CLASS (gst_gl_context_parent_class)->finalize (object);
}

/**
 * gst_gl_context_activate:
 * @context: a #GstGLContext
 * @activate: %TRUE to activate, %FALSE to deactivate
 *
 * (De)activate the OpenGL context represented by this @context.
 *
 * In OpenGL terms, calls eglMakeCurrent or similar with this context and the
 * currently set window.  See gst_gl_context_set_window() for details.
 *
 * Returns: Whether the activation succeeded
 *
 * Since: 1.4
 */
gboolean
gst_gl_context_activate (GstGLContext * context, gboolean activate)
{
  GstGLContextClass *context_class;
  gboolean result;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_val_if_fail (context_class->activate != NULL, FALSE);

  GST_DEBUG_OBJECT (context, "activate:%d", activate);

  GST_OBJECT_LOCK (context);
  result = context_class->activate (context, activate);

  if (result && activate) {
    context->priv->active_thread = g_thread_self ();
    g_private_set (&current_context_key, context);
  } else {
    context->priv->active_thread = NULL;
    g_private_set (&current_context_key, NULL);
  }
  GST_OBJECT_UNLOCK (context);

  return result;
}

/**
 * gst_gl_context_get_thread:
 * @context: a #GstGLContext
 *
 * Returns: (transfer full): The #GThread, @context is current in or NULL
 *
 * Since: 1.6
 */
GThread *
gst_gl_context_get_thread (GstGLContext * context)
{
  GThread *ret;

  GST_OBJECT_LOCK (context);
  ret = context->priv->active_thread;
  if (ret)
    g_thread_ref (ret);
  GST_OBJECT_UNLOCK (context);

  return ret;
}

/**
 * gst_gl_context_get_gl_api:
 * @context: a #GstGLContext
 *
 * Get the currently enabled OpenGL api.
 *
 * The currently available API may be limited by the #GstGLDisplay in use and/or
 * the #GstGLWindow chosen.
 *
 * Returns: the available OpenGL api
 *
 * Since: 1.4
 */
GstGLAPI
gst_gl_context_get_gl_api (GstGLContext * context)
{
  GstGLContextClass *context_class;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), GST_GL_API_NONE);
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_val_if_fail (context_class->get_gl_api != NULL, GST_GL_API_NONE);

  return context_class->get_gl_api (context);
}

/**
 * gst_gl_context_get_proc_address:
 * @context: a #GstGLContext
 * @name: an opengl function name
 *
 * Get a function pointer to a specified opengl function, @name.  If the the
 * specific function does not exist, NULL is returned instead.
 *
 * Platform specfic functions (names starting 'egl', 'glX', 'wgl', etc) can also
 * be retrieved using this method.
 *
 * Returns: a function pointer or NULL
 *
 * Since: 1.4
 */
gpointer
gst_gl_context_get_proc_address (GstGLContext * context, const gchar * name)
{
  gpointer ret;
  GstGLContextClass *context_class;
  GstGLAPI gl_api;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), NULL);
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_val_if_fail (context_class->get_proc_address != NULL, NULL);

  gl_api = gst_gl_context_get_gl_api (context);
  ret = context_class->get_proc_address (gl_api, name);

  return ret;
}

gpointer
gst_gl_context_default_get_proc_address (GstGLAPI gl_api, const gchar * name)
{
  gpointer ret = NULL;

  /* First try to load symbol from the selected GL API for this context */
#if GST_GL_HAVE_GLES2
  if (!ret && (gl_api & GST_GL_API_GLES2)) {
    g_once (&module_gles2_gonce, load_gles2_module, NULL);
    if (module_gles2)
      g_module_symbol (module_gles2, name, &ret);
  }
#endif

#if GST_GL_HAVE_OPENGL
  if (!ret && (gl_api & (GST_GL_API_OPENGL | GST_GL_API_OPENGL3))) {
    g_once (&module_opengl_gonce, load_opengl_module, NULL);
    if (module_opengl)
      g_module_symbol (module_opengl, name, &ret);
  }
#endif

  /* Otherwise fall back to the current module */
  g_once (&module_self_gonce, load_self_module, NULL);
  if (!ret)
    g_module_symbol (module_self, name, &ret);

  return ret;
}

/**
 * gst_gl_context_set_window:
 * @context: a #GstGLContext
 * @window: (transfer full): a #GstGLWindow
 *
 * Set's the current window on @context to @window.  The window can only be
 * changed before gst_gl_context_create() has been called and the @window is not
 * already running.
 *
 * Returns: Whether the window was successfully updated
 *
 * Since: 1.4
 */
gboolean
gst_gl_context_set_window (GstGLContext * context, GstGLWindow * window)
{
  g_return_val_if_fail (!GST_GL_IS_WRAPPED_CONTEXT (context), FALSE);

  GST_DEBUG_OBJECT (context, "window:%" GST_PTR_FORMAT, window);

  /* we can't change the window while we are running */
  if (context->priv->alive)
    return FALSE;

  if (window) {
    if (gst_gl_window_is_running (window))
      return FALSE;

    g_weak_ref_set (&window->context_ref, context);
  }

  if (context->window)
    gst_object_unref (context->window);

  context->window = window ? gst_object_ref (window) : NULL;

  return TRUE;
}

/**
 * gst_gl_context_get_window:
 * @context: a #GstGLContext
 *
 * Returns: the currently set window
 *
 * Since: 1.4
 */
GstGLWindow *
gst_gl_context_get_window (GstGLContext * context)
{
  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), NULL);

  if (GST_GL_IS_WRAPPED_CONTEXT (context)) {
    GST_WARNING_OBJECT (context, "context is not toplevel, returning NULL");
    return NULL;
  }

  _ensure_window (context);

  return gst_object_ref (context->window);
}

/**
 * gst_gl_context_can_share:
 * @context: a #GstGLContext
 * @other_context: another #GstGLContext
 *
 * Note: This will always fail for two wrapped #GstGLContext's
 *
 * Returns: whether @context and @other_context are able to share OpenGL
 *      resources.
 *
 * Since: 1.6
 */
gboolean
gst_gl_context_can_share (GstGLContext * context, GstGLContext * other_context)
{
  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (GST_GL_IS_CONTEXT (other_context), FALSE);

  /* check if the contexts are descendants or the root nodes are the same */
  return context->priv->sharegroup_id != 0
      && context->priv->sharegroup_id == other_context->priv->sharegroup_id;
}

/**
 * gst_gl_context_create:
 * @context: a #GstGLContext:
 * @other_context: (allow-none): a #GstGLContext to share OpenGL objects with
 * @error: (allow-none): a #GError
 *
 * Creates an OpenGL context in the current thread with the specified
 * @other_context as a context to share shareable OpenGL objects with.  See the
 * OpenGL specification for what is shared between contexts.
 *
 * If an error occurs, and @error is not %NULL, then error will contain details
 * of the error and %FALSE will be returned.
 *
 * Should only be called once.
 *
 * Returns: whether the context could successfully be created
 *
 * Since: 1.4
 */
gboolean
gst_gl_context_create (GstGLContext * context,
    GstGLContext * other_context, GError ** error)
{
  gboolean alive = FALSE;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (!GST_GL_IS_WRAPPED_CONTEXT (context), FALSE);

  GST_DEBUG_OBJECT (context, " other_context:%" GST_PTR_FORMAT, other_context);

  _ensure_window (context);

  g_mutex_lock (&context->priv->render_lock);

  if (!context->priv->created) {
    g_weak_ref_set (&context->priv->other_context_ref, other_context);
    context->priv->error = error;
    if (other_context == NULL)
      context->priv->sharegroup_id = _new_sharegroup_id ();
    else
      context->priv->sharegroup_id = other_context->priv->sharegroup_id;

    context->priv->gl_thread = g_thread_new ("gstglcontext",
        (GThreadFunc) gst_gl_context_create_thread, context);

    g_cond_wait (&context->priv->create_cond, &context->priv->render_lock);

    context->priv->created = TRUE;

    GST_INFO_OBJECT (context, "gl thread created");
  }

  alive = context->priv->alive;

  g_mutex_unlock (&context->priv->render_lock);

  return alive;
}

#ifndef GL_DEBUG_TYPE_ERROR
#define GL_DEBUG_TYPE_ERROR 0x824C
#endif
#ifndef GL_DEBUG_TYPE_DEPRECATED_BEHAVIOUR
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOUR 0x824D
#endif
#ifndef GL_DEBUG_TYPE_UNDEFINED_BEHAVIOUR
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOUR 0x824E
#endif
#ifndef GL_DEBUG_TYPE_PORTABILITY
#define GL_DEBUG_TYPE_PORTABILITY 0x824F
#endif
#ifndef GL_DEBUG_TYPE_PERFORMANCE
#define GL_DEBUG_TYPE_PERFORMANCE 0x8250
#endif
#ifndef GL_DEBUG_TYPE_MARKER
#define GL_DEBUG_TYPE_MARKER 0x8268
#endif
#ifndef GL_DEBUG_TYPE_OTHER
#define GL_DEBUG_TYPE_OTHER 0x8251
#endif

#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#endif
#ifndef GL_DEBUG_SEVERITY_LOW
#define GL_DEBUG_SEVERITY_LOW 0x9148
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#endif

#ifndef GL_DEBUG_SOURCE_API
#define GL_DEBUG_SOURCE_API 0x8246
#endif
#ifndef GL_DEBUG_SOURCE_WINDOW_SYSTEM
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM 0x8247
#endif
#ifndef GL_DEBUG_SOURCE_SHADER_COMPILER
#define GL_DEBUG_SOURCE_SHADER_COMPILER 0x8248
#endif
#ifndef GL_DEBUG_SOURCE_THIRD_PARTY
#define GL_DEBUG_SOURCE_THIRD_PARTY 0x8249
#endif
#ifndef GL_DEBUG_SOURCE_APPLICATION
#define GL_DEBUG_SOURCE_APPLICATION 0x824A
#endif
#ifndef GL_DEBUG_SOURCE_OTHER
#define GL_DEBUG_SOURCE_OTHER 0x824B
#endif

#if !defined(GST_DISABLE_GST_DEBUG)
static inline const gchar *
_debug_severity_to_string (GLenum severity)
{
  switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:
      return "high";
    case GL_DEBUG_SEVERITY_MEDIUM:
      return "medium";
    case GL_DEBUG_SEVERITY_LOW:
      return "low";
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      return "notification";
    default:
      return "invalid";
  }
}

static inline const gchar *
_debug_source_to_string (GLenum source)
{
  switch (source) {
    case GL_DEBUG_SOURCE_API:
      return "API";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
      return "winsys";
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
      return "shader compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:
      return "third party";
    case GL_DEBUG_SOURCE_APPLICATION:
      return "application";
    case GL_DEBUG_SOURCE_OTHER:
      return "other";
    default:
      return "invalid";
  }
}

static inline const gchar *
_debug_type_to_string (GLenum type)
{
  switch (type) {
    case GL_DEBUG_TYPE_ERROR:
      return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOUR:
      return "deprecated";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOUR:
      return "undefined";
    case GL_DEBUG_TYPE_PORTABILITY:
      return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE:
      return "performance";
    case GL_DEBUG_TYPE_MARKER:
      return "debug marker";
    case GL_DEBUG_TYPE_OTHER:
      return "other";
    default:
      return "invalid";
  }
}

static void GSTGLAPI
_gst_gl_debug_callback (GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei length, const gchar * message, gpointer user_data)
{
  GstGLContext *context = user_data;
  const gchar *severity_str = _debug_severity_to_string (severity);
  const gchar *source_str = _debug_source_to_string (source);
  const gchar *type_str = _debug_type_to_string (type);

  switch (type) {
    case GL_DEBUG_TYPE_ERROR:
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOUR:
      GST_ERROR_OBJECT (context, "%s: GL %s from %s id:%u, %s", severity_str,
          type_str, source_str, id, message);
      break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOUR:
    case GL_DEBUG_TYPE_PORTABILITY:
      GST_FIXME_OBJECT (context, "%s: GL %s from %s id:%u, %s", severity_str,
          type_str, source_str, id, message);
      break;
    case GL_DEBUG_TYPE_PERFORMANCE:
      GST_CAT_DEBUG_OBJECT (gst_performance, context, "%s: GL %s from %s id:%u,"
          " %s", severity_str, type_str, source_str, id, message);
      break;
    default:
      GST_DEBUG_OBJECT (context, "%s: GL %s from %s id:%u, %s", severity_str,
          type_str, source_str, id, message);
      break;
  }
}
#endif

static gboolean
_create_context_info (GstGLContext * context, GstGLAPI gl_api, gint * gl_major,
    gint * gl_minor, GError ** error)
{
  const GstGLFuncs *gl;
  guint maj = 0, min = 0;
  GLenum gl_err = GL_NO_ERROR;
  const gchar *opengl_version = NULL;

  gl = context->gl_vtable;

  if (!gl->GetString || !gl->GetString (GL_VERSION)
      || !gl->GetString (GL_SHADING_LANGUAGE_VERSION)) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_FAILED,
        "glGetString not defined or returned invalid value");
    return FALSE;
  }

  GST_INFO_OBJECT (context, "GL_VERSION: %s",
      GST_STR_NULL ((const gchar *) gl->GetString (GL_VERSION)));
  GST_INFO_OBJECT (context, "GL_SHADING_LANGUAGE_VERSION: %s",
      GST_STR_NULL ((const gchar *)
          gl->GetString (GL_SHADING_LANGUAGE_VERSION)));
  GST_INFO_OBJECT (context, "GL_VENDOR: %s",
      GST_STR_NULL ((const gchar *) gl->GetString (GL_VENDOR)));
  GST_INFO_OBJECT (context, "GL_RENDERER: %s",
      GST_STR_NULL ((const gchar *) gl->GetString (GL_RENDERER)));

  gl_err = gl->GetError ();
  if (gl_err != GL_NO_ERROR) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_FAILED,
        "glGetString error: 0x%x", gl_err);
    return FALSE;
  }

  opengl_version = (const gchar *) gl->GetString (GL_VERSION);
  if (opengl_version && gl_api & GST_GL_API_GLES2)
    /* gles starts with "OpenGL ES " */
    opengl_version = &opengl_version[10];

  if (opengl_version)
    sscanf (opengl_version, "%d.%d", &maj, &min);

  /* OpenGL > 1.2.0 */
  if (gl_api & GST_GL_API_OPENGL || gl_api & GST_GL_API_OPENGL3) {
    if ((maj < 1) || (maj < 2 && maj >= 1 && min < 2)) {
      g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_OLD_LIBS,
          "OpenGL >= 1.2.0 required, found %u.%u", maj, min);
      return FALSE;
    }
  }

  if (gl_major)
    *gl_major = maj;
  if (gl_minor)
    *gl_minor = min;

  return TRUE;
}

static GstGLAPI
_compiled_api (void)
{
  GstGLAPI ret = GST_GL_API_NONE;

#if GST_GL_HAVE_OPENGL
  ret |= GST_GL_API_OPENGL | GST_GL_API_OPENGL3;
#endif
#if GST_GL_HAVE_GLES2
  ret |= GST_GL_API_GLES2;
#endif

  return ret;
}

static void
_unlock_create_thread (GstGLContext * context)
{
  g_mutex_unlock (&context->priv->render_lock);
}

static GString *
_build_extension_string (GstGLContext * context)
{
  const GstGLFuncs *gl = context->gl_vtable;
  GString *ext_g_str = g_string_sized_new (1024);
  const gchar *ext_const_c_str = NULL;
  GLint i = 0;
  GLint n = 0;

  gl->GetIntegerv (GL_NUM_EXTENSIONS, &n);

  for (i = 0; i < n; i++) {
    ext_const_c_str = (const gchar *) gl->GetStringi (GL_EXTENSIONS, i);
    if (ext_const_c_str)
      g_string_append_printf (ext_g_str, "%s ", ext_const_c_str);
  }

  return ext_g_str;
}

//gboolean
//gst_gl_context_create (GstGLContext * context, GstGLContext * other_context, GError ** error)
static gpointer
gst_gl_context_create_thread (GstGLContext * context)
{
  GstGLContextClass *context_class;
  GstGLWindowClass *window_class;
  GstGLFuncs *gl;
  GstGLAPI compiled_api, user_api, gl_api, display_api;
  gchar *api_string;
  gchar *compiled_api_s;
  gchar *user_api_s;
  gchar *display_api_s;
  const gchar *user_choice;
  GError **error;
  GstGLContext *other_context;

  g_mutex_lock (&context->priv->render_lock);

  GST_DEBUG_OBJECT (context, "Creating thread");

  error = context->priv->error;
  other_context = g_weak_ref_get (&context->priv->other_context_ref);

  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  window_class = GST_GL_WINDOW_GET_CLASS (context->window);

  display_api = gst_gl_display_get_gl_api_unlocked (context->display);
  if (display_api == GST_GL_API_NONE) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_WRONG_API,
        "Cannot create context with satisfying requested apis "
        "(display has no GL api!)");
    goto failure;
  }

  if (window_class->open) {
    if (!window_class->open (context->window, error)) {
      GST_WARNING_OBJECT (context, "Failed to open window");
      g_assert (error == NULL || *error != NULL);
      goto failure;
    }
  }

  gl = context->gl_vtable;
  compiled_api = _compiled_api ();
  compiled_api_s = gst_gl_api_to_string (compiled_api);

  user_choice = g_getenv ("GST_GL_API");
  user_api = gst_gl_api_from_string (user_choice);
  user_api_s = gst_gl_api_to_string (user_api);

  display_api_s = gst_gl_api_to_string (display_api);

  if ((user_api & compiled_api & display_api) == GST_GL_API_NONE) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_WRONG_API,
        "Cannot create context with the user requested api (%s).  "
        "We have support for (%s), display api (%s)", user_api_s,
        compiled_api_s, display_api_s);
    g_free (user_api_s);
    g_free (compiled_api_s);
    g_free (display_api_s);
    goto failure;
  }

  if (context_class->choose_format &&
      !context_class->choose_format (context, error)) {
    GST_WARNING_OBJECT (context, "Failed to choose format");
    g_assert (error == NULL || *error != NULL);
    g_free (compiled_api_s);
    g_free (user_api_s);
    g_free (display_api_s);
    goto failure;
  }

  GST_INFO_OBJECT (context,
      "Attempting to create opengl context. user chosen api(s) (%s), "
      "compiled api support (%s) display api (%s)", user_api_s, compiled_api_s,
      display_api_s);

  if (!context_class->create_context (context,
          compiled_api & user_api & display_api, other_context, error)) {
    GST_WARNING_OBJECT (context, "Failed to create context");
    g_assert (error == NULL || *error != NULL);
    g_free (compiled_api_s);
    g_free (user_api_s);
    g_free (display_api_s);
    goto failure;
  }
  GST_INFO_OBJECT (context, "created context");

  if (!gst_gl_context_activate (context, TRUE)) {
    g_set_error (error, GST_GL_CONTEXT_ERROR,
        GST_GL_CONTEXT_ERROR_RESOURCE_UNAVAILABLE,
        "Failed to activate the GL Context");
    g_free (compiled_api_s);
    g_free (user_api_s);
    g_free (display_api_s);
    goto failure;
  }

  gl_api = gst_gl_context_get_gl_api (context);
  g_assert (gl_api != GST_GL_API_NONE && gl_api != GST_GL_API_ANY);

  api_string = gst_gl_api_to_string (gl_api);
  GST_INFO_OBJECT (context, "available GL APIs: %s", api_string);

  if (((compiled_api & gl_api & display_api) & user_api) == GST_GL_API_NONE) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_WRONG_API,
        "failed to create context, context "
        "could not provide correct api. user (%s), compiled (%s), context (%s)",
        user_api_s, compiled_api_s, api_string);
    g_free (api_string);
    g_free (compiled_api_s);
    g_free (user_api_s);
    g_free (display_api_s);
    goto failure;
  }

  g_free (api_string);
  g_free (compiled_api_s);
  g_free (user_api_s);
  g_free (display_api_s);

  GST_DEBUG_OBJECT (context, "Filling info");
  if (!gst_gl_context_fill_info (context, error)) {
    g_assert (error == NULL || *error != NULL);
    goto failure;
  }

  context->priv->alive = TRUE;

  if (gl->DebugMessageCallback) {
#if !defined(GST_DISABLE_GST_DEBUG)
    GST_INFO_OBJECT (context, "Enabling GL context debugging");
    /* enable them all */
    gl->DebugMessageControl (GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, 0,
        GL_TRUE);
    gl->DebugMessageCallback (_gst_gl_debug_callback, context);
#endif
  }

  if (other_context) {
    GST_DEBUG_OBJECT (context, "Unreffing other_context %" GST_PTR_FORMAT,
        other_context);
    gst_object_unref (other_context);
  }

  g_cond_signal (&context->priv->create_cond);

//  g_mutex_unlock (&context->priv->render_lock);
  gst_gl_window_send_message_async (context->window,
      (GstGLWindowCB) _unlock_create_thread, context, NULL);

  gst_gl_window_run (context->window);

  GST_INFO_OBJECT (context, "loop exited");

  g_mutex_lock (&context->priv->render_lock);

  context->priv->alive = FALSE;

  gst_gl_context_activate (context, FALSE);

  context_class->destroy_context (context);

  /* User supplied callback */
  if (context->window->close)
    context->window->close (context->window->close_data);

  /* window specific shutdown */
  if (window_class->close) {
    window_class->close (context->window);
  }

  g_cond_signal (&context->priv->destroy_cond);

  g_mutex_unlock (&context->priv->render_lock);

  return NULL;

failure:
  {
    if (other_context)
      gst_object_unref (other_context);

    g_cond_signal (&context->priv->create_cond);
    g_mutex_unlock (&context->priv->render_lock);
    return NULL;
  }
}

/**
 * gst_gl_context_destroy:
 * @context: a #GstGLContext:
 *
 * Destroys an OpenGL context.
 *
 * Should only be called after gst_gl_context_create() has been successfully
 * called for this context.
 *
 * Since: 1.6
 */
void
gst_gl_context_destroy (GstGLContext * context)
{
  GstGLContextClass *context_class;

  g_return_if_fail (GST_GL_IS_CONTEXT (context));
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_if_fail (context_class->destroy_context != NULL);

  context_class->destroy_context (context);
}

/**
 * gst_gl_context_fill_info:
 * @context: a #GstGLContext:
 *
 * Fills @context's info (version, extensions, vtable, etc) from the GL
 * context in the current thread.  Typically used with wrapped contexts to
 * allow wrapped contexts to be used as regular #GstGLContext's.
 *
 * Since: 1.6
 */
gboolean
gst_gl_context_fill_info (GstGLContext * context, GError ** error)
{
  GstGLFuncs *gl;
  GString *ext_g_str = NULL;
  const gchar *ext_const_c_str = NULL;
  GstGLAPI gl_api;
  gboolean ret;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (context->priv->active_thread == g_thread_self (),
      FALSE);

  gl = context->gl_vtable;
  gl_api = gst_gl_context_get_gl_api (context);

  gl->GetError = gst_gl_context_get_proc_address (context, "glGetError");
  gl->GetString = gst_gl_context_get_proc_address (context, "glGetString");
  gl->GetStringi = gst_gl_context_get_proc_address (context, "glGetStringi");
  gl->GetIntegerv = gst_gl_context_get_proc_address (context, "glGetIntegerv");

  if (!gl->GetError || !gl->GetString) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_FAILED,
        "could not GetProcAddress core opengl functions");
    goto failure;
  }

  /* gl api specific code */
  ret = _create_context_info (context, gl_api, &context->priv->gl_major,
      &context->priv->gl_minor, error);

  if (!ret) {
    g_assert (error == NULL || *error != NULL);
    goto failure;
  }

  /* GL core contexts and GLES3 */
  if (gl->GetIntegerv && gl->GetStringi && context->priv->gl_major >= 3)
    ext_g_str = _build_extension_string (context);

  if (ext_g_str && ext_g_str->len) {
    GST_DEBUG_OBJECT (context, "GL_EXTENSIONS: %s", ext_g_str->str);
    _gst_gl_feature_check_ext_functions (context, context->priv->gl_major,
        context->priv->gl_minor, ext_g_str->str);

    context->priv->gl_exts = g_string_free (ext_g_str, FALSE);
  } else {
    ext_const_c_str = (const gchar *) gl->GetString (GL_EXTENSIONS);
    if (!ext_const_c_str)
      ext_const_c_str = "";

    GST_DEBUG_OBJECT (context, "GL_EXTENSIONS: %s", ext_const_c_str);
    _gst_gl_feature_check_ext_functions (context, context->priv->gl_major,
        context->priv->gl_minor, ext_const_c_str);

    context->priv->gl_exts = g_strdup (ext_const_c_str);
  }

  if (gl_api & GST_GL_API_OPENGL3
      && !gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL3, 4, 1)
      && !gst_gl_check_extension ("GL_ARB_ES2_compatibility",
          context->priv->gl_exts)) {
    g_set_error (error, GST_GL_CONTEXT_ERROR, GST_GL_CONTEXT_ERROR_FAILED,
        "An opengl3 context created but the required ES2 compatibility was not found");
    goto failure;
  }

  /* Does not implement OES_vertex_array_object properly, see
   * https://bugzilla.gnome.org/show_bug.cgi?id=750185 */
  if (g_strcmp0 ((const gchar *) gl->GetString (GL_VENDOR),
          "Imagination Technologies") == 0
      && g_strcmp0 ((const gchar *) gl->GetString (GL_RENDERER),
          "PowerVR SGX 544MP") == 0) {
    gl->GenVertexArrays = NULL;
    gl->DeleteVertexArrays = NULL;
    gl->BindVertexArray = NULL;
    gl->IsVertexArray = NULL;
  }

  return TRUE;

failure:
  return FALSE;
}

/**
 * gst_gl_context_get_gl_context:
 * @context: a #GstGLContext:
 *
 * Gets the backing OpenGL context used by @context.
 *
 * Returns: The platform specific backing OpenGL context
 *
 * Since: 1.4
 */
guintptr
gst_gl_context_get_gl_context (GstGLContext * context)
{
  GstGLContextClass *context_class;
  guintptr result;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), 0);
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_val_if_fail (context_class->get_gl_context != NULL, 0);

  result = context_class->get_gl_context (context);

  return result;
}

/**
 * gst_gl_context_get_gl_platform:
 * @context: a #GstGLContext:
 *
 * Gets the OpenGL platform that used by @context.
 *
 * Returns: The platform specific backing OpenGL context
 *
 * Since: 1.4
 */
GstGLPlatform
gst_gl_context_get_gl_platform (GstGLContext * context)
{
  GstGLContextClass *context_class;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), 0);
  context_class = GST_GL_CONTEXT_GET_CLASS (context);
  g_return_val_if_fail (context_class->get_gl_platform != NULL, 0);

  return context_class->get_gl_platform (context);
}

/**
 * gst_gl_context_get_display:
 * @context: a #GstGLContext:
 *
 * Returns: the #GstGLDisplay associated with this @context
 *
 * Since: 1.4
 */
GstGLDisplay *
gst_gl_context_get_display (GstGLContext * context)
{
  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), NULL);

  return gst_object_ref (context->display);
}

typedef struct
{
  GstGLContext *context;
  GstGLContextThreadFunc func;
  gpointer data;
} RunGenericData;

static void
_gst_gl_context_thread_run_generic (RunGenericData * data)
{
  GST_TRACE_OBJECT (data->context, "running function:%p data:%p", data->func,
      data->data);

  data->func (data->context, data->data);
}

/**
 * gst_gl_context_thread_add:
 * @context: a #GstGLContext
 * @func: a #GstGLContextThreadFunc
 * @data: (closure): user data to call @func with
 *
 * Execute @func in the OpenGL thread of @context with @data
 *
 * MT-safe
 *
 * Since: 1.4
 */
void
gst_gl_context_thread_add (GstGLContext * context,
    GstGLContextThreadFunc func, gpointer data)
{
  GstGLWindow *window;
  RunGenericData rdata;

  g_return_if_fail (GST_GL_IS_CONTEXT (context));
  g_return_if_fail (func != NULL);

  if (GST_GL_IS_WRAPPED_CONTEXT (context)) {
    g_return_if_fail (context->priv->active_thread == g_thread_self ());
    func (context, data);
    return;
  }

  rdata.context = context;
  rdata.data = data;
  rdata.func = func;

  window = gst_gl_context_get_window (context);

  gst_gl_window_send_message (window,
      GST_GL_WINDOW_CB (_gst_gl_context_thread_run_generic), &rdata);

  gst_object_unref (window);
}

/**
 * gst_gl_context_get_gl_version:
 * @context: a #GstGLContext
 * @maj: (out): resulting major version
 * @min: (out): resulting minor version
 *
 * Returns the OpenGL version implemented by @context.  See
 * gst_gl_context_get_gl_api() for retreiving the OpenGL api implemented by
 * @context.
 *
 * Since: 1.4
 */
void
gst_gl_context_get_gl_version (GstGLContext * context, gint * maj, gint * min)
{
  g_return_if_fail (GST_GL_IS_CONTEXT (context));
  g_return_if_fail (maj != NULL && min != NULL);

  if (maj)
    *maj = context->priv->gl_major;

  if (min)
    *min = context->priv->gl_minor;
}

/**
 * gst_gl_context_check_gl_version:
 * @context: a #GstGLContext
 * @api: api type required
 * @maj: major version required
 * @min: minor version required
 *
 * Returns: whether OpenGL context implements the required api and specified
 * version.
 *
 * Since: 1.4
 */
gboolean
gst_gl_context_check_gl_version (GstGLContext * context, GstGLAPI api,
    gint maj, gint min)
{
  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);

  if (maj > context->priv->gl_major)
    return FALSE;

  if ((gst_gl_context_get_gl_api (context) & api) == GST_GL_API_NONE)
    return FALSE;

  if (maj < context->priv->gl_major)
    return TRUE;

  if (min > context->priv->gl_minor)
    return FALSE;

  return TRUE;
}

/**
 * gst_gl_context_check_feature:
 * @context: a #GstGLContext
 * @feature: a platform specific feature
 *
 * Some features require that the context be created before it is possible to
 * determine their existence and so will fail if that is not the case.
 *
 * Returns: Whether @feature is supported by @context
 *
 * Since: 1.4
 */
gboolean
gst_gl_context_check_feature (GstGLContext * context, const gchar * feature)
{
  GstGLContextClass *context_class;

  g_return_val_if_fail (GST_GL_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (feature != NULL, FALSE);

  context_class = GST_GL_CONTEXT_GET_CLASS (context);

  if (g_strstr_len (feature, 3, "GL_"))
    return gst_gl_check_extension (feature, context->priv->gl_exts);

  if (!context_class->check_feature)
    return FALSE;

  return context_class->check_feature (context, feature);
}

/**
 * gst_gl_context_get_current:
 *
 * See also gst_gl_context_activate().
 *
 * Returns: the #GstGLContext active in the current thread or %NULL
 *
 * Since: 1.6
 */
GstGLContext *
gst_gl_context_get_current (void)
{
  return g_private_get (&current_context_key);
}

static GstGLAPI
gst_gl_wrapped_context_get_gl_api (GstGLContext * context)
{
  GstGLWrappedContext *context_wrap = GST_GL_WRAPPED_CONTEXT (context);

  return context_wrap->available_apis;
}

static guintptr
gst_gl_wrapped_context_get_gl_context (GstGLContext * context)
{
  GstGLWrappedContext *context_wrap = GST_GL_WRAPPED_CONTEXT (context);

  return context_wrap->handle;
}

static GstGLPlatform
gst_gl_wrapped_context_get_gl_platform (GstGLContext * context)
{
  GstGLWrappedContext *context_wrap = GST_GL_WRAPPED_CONTEXT (context);

  return context_wrap->platform;
}

static gboolean
gst_gl_wrapped_context_activate (GstGLContext * context, gboolean activate)
{
  if (activate)
    context->priv->gl_thread = g_thread_self ();
  else
    context->priv->gl_thread = NULL;

  return TRUE;
}

static void
gst_gl_wrapped_context_class_init (GstGLWrappedContextClass * klass)
{
  GstGLContextClass *context_class = (GstGLContextClass *) klass;

  context_class->get_gl_context =
      GST_DEBUG_FUNCPTR (gst_gl_wrapped_context_get_gl_context);
  context_class->get_gl_api =
      GST_DEBUG_FUNCPTR (gst_gl_wrapped_context_get_gl_api);
  context_class->get_gl_platform =
      GST_DEBUG_FUNCPTR (gst_gl_wrapped_context_get_gl_platform);
  context_class->activate = GST_DEBUG_FUNCPTR (gst_gl_wrapped_context_activate);
}

static void
gst_gl_wrapped_context_init (GstGLWrappedContext * context)
{
}
