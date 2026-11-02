/*
 * The KHR-fence and ANDROID-native-fence entry points, for macOS.
 *
 * Mesa's libEGL exports the EGL 1.5 core names only (eglCreateSync, ...); the KHR aliases virglrenderer calls
 * under EGL_EGLEXT_PROTOTYPES are not in the export table, so the link fails on four symbols. EGL_KHR_fence_sync
 * is core in 1.5, so the first three forward; the ANDROID one has no equivalent on this platform and returns -1,
 * which is also what it would do as a null proc address -- the extension is not advertised, so vrend never calls it.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>

EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync);
EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync);

EGLSyncKHR
eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list)
{
	return eglCreateSync(dpy, type, (const EGLAttrib *)attrib_list);
}

EGLint
eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout)
{
	return eglClientWaitSync(dpy, sync, flags, timeout);
}

EGLBoolean
eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync)
{
	return eglDestroySync(dpy, sync);
}

EGLint
eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync)
{
	(void)dpy;
	(void)sync;
	return -1;
}
