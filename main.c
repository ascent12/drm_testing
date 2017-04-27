#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES3/gl3.h>

PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT;

struct {
	struct gbm_device *gbm;
	struct {
		EGLDisplay dpy;
		EGLConfig conf;
		EGLContext cntxt;
	} egl;
} render;

struct timespec last;
float colour[3] = { 1.0f, 0.0f, 0.0f };
int dec;

struct display {
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t width, height;
	drmModeModeInfo mode_info;

	uint32_t fb_id;

	bool pageflip_pending;
	bool vblank_pending;
	bool cleanup;

	EGLSurface egl;
	struct gbm_surface *gbm;

	drmModeCrtc *old_crtc;
} displays[8];
size_t num_displays;

void modeset(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "drmModeGetResources");
		exit(1);
	}

	uint32_t taken_crtcs = 0;

	for (int i = 0; i < res->count_connectors; ++i) {
		struct display *dpy = &displays[num_displays];
		drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(conn);
			continue;
		}

		/* Get mode */
		if (conn->count_modes == 0) {
			printf("No valid modes\n");
			continue;
		}

		memcpy(&dpy->mode_info, &conn->modes[0],
				sizeof dpy->mode_info);
		dpy->width = conn->modes[0].hdisplay;
		dpy->height = conn->modes[0].vdisplay;

		/* Save old CRTC */
		drmModeEncoder *curr_enc = drmModeGetEncoder(fd, conn->encoder_id);
		if (curr_enc) {
			dpy->old_crtc = drmModeGetCrtc(fd, curr_enc->crtc_id);
			drmModeFreeEncoder(curr_enc);
		}

		/* Get new CRTC */
		for (int j = 0; j < conn->count_encoders; ++j) {
			drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[j]);

			for (int k = 0; k < res->count_crtcs; ++k) {
				if ((enc->possible_crtcs & (1 << k)) == 0)
					continue;

				if ((taken_crtcs & (1 << k)) == 0) {
					taken_crtcs |= 1 << k;
					dpy->crtc_id = res->crtcs[k];

					drmModeFreeEncoder(enc);
					goto got_crtc;
				}
			}

			drmModeFreeEncoder(enc);
		}

		/* Failure */
		printf("Could not find crtc\n");
		drmModeFreeConnector(conn);
		continue;

got_crtc:
		dpy->connector_id = conn->connector_id;

		drmModeFreeConnector(conn);
		++num_displays;
	}

	drmModeFreeResources(res);
}

void egl_exts(void)
{
	eglGetPlatformDisplayEXT = 
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");

	eglCreatePlatformWindowSurfaceEXT =
		(PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)
		eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

	if (!eglGetPlatformDisplayEXT || !eglCreatePlatformWindowSurfaceEXT) {
		fprintf(stderr, "eglGetProcAddress");
		exit(1);
	}
}

EGLConfig egl_get_config(EGLDisplay dpy)
{
	EGLint count = 0, matched = 0, ret;

	ret = eglGetConfigs(dpy, NULL, 0, &count);
	if (ret == EGL_FALSE || count == 0) {
		fprintf(stderr, "eglGetConfigs");
		exit(1);
	}

	EGLConfig configs[count];

	ret = eglChooseConfig(dpy, NULL, configs, count, &matched);
	if (ret == EGL_FALSE) {
		fprintf(stderr, "eglChooseConfig");
		exit(1);
	}

	for (int i = 0; i < matched; ++i) {
		EGLint gbm_format;

		if (!eglGetConfigAttrib(dpy,
					configs[i],
					EGL_NATIVE_VISUAL_ID,
					&gbm_format))
			continue;

		if (gbm_format == GBM_FORMAT_XRGB8888) {
			return configs[i];
		}
	}

	fprintf(stderr, "Failed to get EGL config");
	exit(1);
}

void free_fb(struct gbm_bo *bo, void *data)
{
	uint32_t *id = data;

	if (id && *id != 0)
		drmModeRmFB(gbm_bo_get_fd(bo), *id);

	free(id);
}

uint32_t get_fb_for_bo(int fd, struct gbm_bo *bo)
{
	uint32_t *id = gbm_bo_get_user_data(bo);

	if (id)
		return *id;

	id = malloc(sizeof *id);
	if (!id) {
		fprintf(stderr, "malloc");
		exit(1);
	}

	drmModeAddFB(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
		     gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, id);

	gbm_bo_set_user_data(bo, id, free_fb);

	return *id;
}

void rendering_init(int fd)
{
	egl_exts();

	render.gbm = gbm_create_device(fd);
	if (!render.gbm) {
		fprintf(stderr, "gbm_create_device");
		exit(1);
	}

	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		fprintf(stderr, "eglBindAPI");
		exit(1);
	}

	render.egl.dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, render.gbm, NULL);
	if (render.egl.dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "eglGetPlatformDisplayEXT");
		exit(1);
	}

	if (eglInitialize(render.egl.dpy, NULL, NULL) == EGL_FALSE) {
		fprintf(stderr, "eglInitialize");
		exit(1);
	}

	render.egl.conf = egl_get_config(render.egl.dpy);

	const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	render.egl.cntxt = eglCreateContext(render.egl.dpy, render.egl.conf, EGL_NO_CONTEXT, attribs);
	if (render.egl.cntxt == EGL_NO_CONTEXT) {
		fprintf(stderr, "eglCreateContext");
		exit(1);
	}

	for (size_t i = 0; i < num_displays; ++i) {
		struct display *dpy = &displays[i];

		dpy->gbm = gbm_surface_create(render.gbm,
					      dpy->width, dpy->height,
					      GBM_FORMAT_XRGB8888,
					      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (!dpy->gbm) {
			fprintf(stderr, "gbm_surface_create");
			exit(1);
		}

		dpy->egl = eglCreatePlatformWindowSurfaceEXT(render.egl.dpy, render.egl.conf,
							     dpy->gbm, NULL);
		if (dpy->egl == EGL_NO_SURFACE) {
			fprintf(stderr, "eglCreatePlatformWindowSurfaceEXT");
			exit(1);
		}

		eglMakeCurrent(render.egl.dpy, dpy->egl, dpy->egl, render.egl.cntxt);

		glViewport(0, 0, dpy->width, dpy->height);
		glClearColor(colour[0], colour[1], colour[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		eglSwapBuffers(render.egl.dpy, dpy->egl);

		struct gbm_bo *bo = gbm_surface_lock_front_buffer(dpy->gbm);

		uint32_t fb_id = get_fb_for_bo(fd, bo);

		drmModeSetCrtc(fd, dpy->crtc_id, fb_id, 0, 0,
			       &dpy->connector_id, 1, &dpy->mode_info);
		drmModePageFlip(fd, dpy->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, dpy);

		gbm_surface_release_buffer(dpy->gbm, bo);
	}
}

void render_display(int fd, struct display *dpy)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
	ms %= 1000;

	int inc = (dec + 1) % 3;

	colour[dec] -= ms / 2000.0f;
	colour[inc] += ms / 2000.0f;

	if (colour[dec] < 0.0f) {
		colour[dec] = 0.0f;
		colour[inc] = 1.0f;

		dec = (dec + 1) % 3;
	}

	last = now;

	eglMakeCurrent(render.egl.dpy, dpy->egl, dpy->egl, render.egl.cntxt);

	glViewport(0, 0, dpy->width, dpy->height);
	glClearColor(colour[0], colour[1], colour[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(render.egl.dpy, dpy->egl);

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(dpy->gbm);

	uint32_t fb_id = get_fb_for_bo(fd, bo);

	drmModePageFlip(fd, dpy->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, dpy);

	gbm_surface_release_buffer(dpy->gbm, bo);

	dpy->pageflip_pending = true;
	dpy->vblank_pending = true;
}

void page_flip_handler(int fd,
		       unsigned seq,
		       unsigned tv_sec,
		       unsigned tv_usec,
		       void *user)
{
	struct display *dpy = user;

	dpy->pageflip_pending = false;
	if (!dpy->cleanup)
		render_display(fd, dpy);
}

void rendering_cleanup(int fd)
{
	for (size_t i = 0; i < num_displays; ++i) {
		struct display *dpy = &displays[i];

		dpy->cleanup = true;

		drmEventContext event = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			//.vblank_handler = vblank_handler,
			.page_flip_handler = page_flip_handler,
		};

		// Wait for pageflips to finish
		while (dpy->pageflip_pending) {
			drmHandleEvent(fd, &event);
		}

		drmModeCrtc *crtc = dpy->old_crtc;
		drmModeSetCrtc(fd, crtc->crtc_id, crtc->buffer_id,
			       crtc->x, crtc->y, &dpy->connector_id,
			       1, &crtc->mode);

		drmModeFreeCrtc(crtc);

		eglDestroySurface(render.egl.dpy, dpy->egl);
		gbm_surface_destroy(dpy->gbm);
	}

	eglDestroyContext(render.egl.dpy, render.egl.cntxt);
	eglTerminate(render.egl.dpy);
	eglReleaseThread();

	gbm_device_destroy(render.gbm);
}

int main()
{
	int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm == -1) {
		perror("/dev/dri/card0");
		exit(1);
	}

	clock_gettime(CLOCK_MONOTONIC, &last);

	modeset(drm);
	rendering_init(drm);

	struct pollfd pollfd[2] = {
		{ .fd = drm, .events = POLLIN },
		{ .fd = 0, .events = POLLIN },
	};
	drmEventContext event = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
	};

	while (1) {
		int ret = poll(pollfd, 2, -1);
		if (ret == -1) {
			exit(1);
		} else if (pollfd[0].revents) {
			drmHandleEvent(drm, &event);
		} else if (pollfd[1].revents) {
			break;
		}
	}

	rendering_cleanup(drm);

	close(drm);
}
