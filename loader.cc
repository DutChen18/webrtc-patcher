#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <funchook.h>
#include <libelf.h>
#include <gelf.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <node/node_api.h>
#include <cstdio>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

using napi_register_module_v1_t = napi_value(*)(napi_env, napi_value);
using CreateDesktopCapturer_t = std::unique_ptr<webrtc::DesktopCapturer>(*)(const webrtc::DesktopCaptureOptions&);

static link_map* handle;
static FILE* file;

static napi_register_module_v1_t napi_register_module_v1_func;
static CreateDesktopCapturer_t CreateRawWindowCapturer_func;

namespace {
	static EGLint egl_config_attrib_list[] = {
		EGL_NONE,
	};

	static EGLint egl_context_attrib_list[] = {
		EGL_CONTEXT_MAJOR_VERSION, 2,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_NONE,
	};

	static PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback;
	static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
	static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
	static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
	static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

	template<class T>
	static void EGLGetProcAddress(T* proc, const char* name) {
		*proc = reinterpret_cast<T>(eglGetProcAddress(name));
	}

	static void MessageCallback(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*) {
		if (type == GL_DEBUG_TYPE_ERROR) {
			std::fprintf(file, "gl error = %s\n", message);
		}
	}

	class WindowCapturer : public webrtc::DesktopCapturer {
	public:
		void Start(Callback* callback) override {
			callback_ = callback;
		}

		void CaptureFrame() override {
			PollEvents();

			if (window_) {
				eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);

				if (update_) {
					Window root;
					int x, y;
					unsigned int border_width, depth;

					update_ = false;
					Release();
					pixmap_ = XCompositeNameWindowPixmap(display_, window_);
					XGetGeometry(display_, pixmap_, &root, &x, &y, &width_, &height_, &border_width, &depth);

					egl_image_ = eglCreateImage(egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer) pixmap_, nullptr);
					glBindTexture(GL_TEXTURE_2D, gl_tex_);
					glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image_);

					std::fprintf(file, "created new image\n");
					std::fprintf(file, "window size = %ux%u\n", width_, height_);
				}

				webrtc::DesktopSize size(width_, height_);
				std::unique_ptr<webrtc::DesktopFrame> frame(new webrtc::BasicDesktopFrame(size));

				glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo_);
				glReadPixels(0, 0, width_, height_, GL_BGRA, GL_UNSIGNED_BYTE, frame->data());
				eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

				callback_->OnCaptureResult(Result::SUCCESS, std::move(frame));
			} else {
				callback_->OnCaptureResult(Result::ERROR_PERMANENT, nullptr);
			}
		}

		bool SelectSource(SourceId id) override {
			std::fprintf(file, "WindowCapturer::SelectSource(%ld)\n", id);
			std::fprintf(file, "old window = %lu\n", window_);

			PollEvents();

			if (window_) {
				XSelectInput(display_, window_, NoEventMask);
				XCompositeUnredirectWindow(display_, window_, CompositeRedirectAutomatic);
			}

			window_ = id;
			update_ = true;
			XSelectInput(display_, window_, StructureNotifyMask | VisibilityChangeMask);
			XCompositeRedirectWindow(display_, window_, CompositeRedirectAutomatic);

			return true;
		}

		static std::unique_ptr<webrtc::DesktopCapturer> CreateRawWindowCapturer(const webrtc::DesktopCaptureOptions& options) {
			std::fprintf(file, "WindowCapturer::CreateRawWindowCapturer()\n");
			return std::unique_ptr<webrtc::DesktopCapturer>(new WindowCapturer(options));
		}

	private:
		WindowCapturer(const webrtc::DesktopCaptureOptions& options) {
			EGLConfig egl_config;
			EGLint egl_num_config;
			EGLint egl_major, egl_minor;

			std::fprintf(file, "WindowCapturer::WindowCapturer()\n");

			display_ = XOpenDisplay(DisplayString(options.x_display()->display()));
			egl_display_ = eglGetDisplay(display_);
			eglInitialize(egl_display_, &egl_major, &egl_minor);
			eglChooseConfig(egl_display_, egl_config_attrib_list, &egl_config, 1, &egl_num_config);
			egl_context_ = eglCreateContext(egl_display_, egl_config, EGL_NO_CONTEXT, egl_context_attrib_list);

			std::fprintf(file, "egl version = %d.%d\n", egl_major, egl_minor);
			std::fprintf(file, "egl context = %p\n", egl_context_);

			EGLGetProcAddress(&glDebugMessageCallback, "glDebugMessageCallback");
			EGLGetProcAddress(&glGenFramebuffers, "glGenFramebuffers");
			EGLGetProcAddress(&glDeleteFramebuffers, "glDeleteFramebuffers");
			EGLGetProcAddress(&glBindFramebuffer, "glBindFramebuffer");
			EGLGetProcAddress(&glFramebufferTexture2D, "glFramebufferTexture2D");
			EGLGetProcAddress(&glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");

			eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
			glEnable(GL_DEBUG_OUTPUT);
			glDebugMessageCallback(MessageCallback, this);
			glGenTextures(1, &gl_tex_);
			glGenFramebuffers(1, &gl_fbo_);
			glBindTexture(GL_TEXTURE_2D, gl_tex_);
			glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo_);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_tex_, 0);
			eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}

		~WindowCapturer() {
			std::fprintf(file, "~WindowCapturer::WindowCapturer()\n");

			Release();
			glDeleteFramebuffers(1, &gl_fbo_);
			glDeleteTextures(1, &gl_tex_);
			eglDestroyContext(egl_display_, egl_context_);
			eglTerminate(egl_display_);
			XCloseDisplay(display_);
		}

		void Release() {
			if (pixmap_) {
				XFreePixmap(display_, pixmap_);
				eglDestroyImage(egl_display_, egl_image_);
			}
		}

		void PollEvents() {
			XEvent event;

			while (XPending(display_)) {
				XNextEvent(display_, &event);

				if (event.xany.window == window_) {
					switch (event.type) {
					case DestroyNotify:
						std::fprintf(file, "window destroyed\n");
						window_ = None;
					case ConfigureNotify:
					case MapNotify:
					case VisibilityNotify:
						std::fprintf(file, "update required\n");
						update_ = true;
					default:
						break;
					}
				}
			}
		}

		Callback* callback_;
		Display* display_;
		Window window_ = None;
		Pixmap pixmap_ = None;
		EGLDisplay egl_display_;
		EGLContext egl_context_;
		EGLImage egl_image_;
		GLuint gl_tex_;
		GLuint gl_fbo_;
		unsigned int width_;
		unsigned int height_;
		bool update_ = false;
	};
}

extern "C" void webrtc_patcher_cookie_66706761() {}

extern "C" napi_value napi_register_module_v1(napi_env env, napi_value exports) {
	file = std::fopen("/tmp/webrtc-patcher.log", "a");
	std::setvbuf(file, nullptr, _IOLBF, BUFSIZ);
	std::fprintf(file, "napi_register_module_v1()\n");

	Dl_info info;
	dladdr(reinterpret_cast<void*>(webrtc_patcher_cookie_66706761), &info);

	std::string loader_name = info.dli_fname;
	std::size_t slash = loader_name.find_last_of('/');
	std::string real_name = loader_name.substr(0, slash) + "/real_" + loader_name.substr(slash + 1);

	std::fprintf(file, "loader name = %s\n", loader_name.c_str());
	std::fprintf(file, "real name = %s\n", real_name.c_str());

	elf_version(EV_CURRENT);
	int fd = open(real_name.c_str(), O_RDONLY);
	Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
	Elf_Scn* scn = nullptr;
	GElf_Shdr shdr;

	while ((scn = elf_nextscn(elf, scn))) {
		gelf_getshdr(scn, &shdr);

		if (shdr.sh_type == SHT_SYMTAB) {
			break;
		}
	}

	Elf_Data* data = elf_getdata(scn, nullptr);
	GElf_Sym sym;
	GElf_Addr CreateRawWindowCapturer_offset = 0;

	for (GElf_Xword i = 0; i < shdr.sh_size - shdr.sh_entsize; i++) {
		gelf_getsym(data, i, &sym);
		const char* name = elf_strptr(elf, shdr.sh_link, sym.st_name);

		if (std::strcmp(name, "_ZN6webrtc17WindowCapturerX1123CreateRawWindowCapturerERKNS_21DesktopCaptureOptionsE") == 0) {
			CreateRawWindowCapturer_offset = sym.st_value;
		}
	}

	elf_end(elf);
	close(fd);

	std::fprintf(file, "CreateRawWindowCapturer offset = %lu\n", CreateRawWindowCapturer_offset);

	handle = static_cast<link_map*>(dlopen(real_name.c_str(), RTLD_NOW));
	napi_register_module_v1_func = reinterpret_cast<napi_register_module_v1_t>(dlsym(handle, "napi_register_module_v1"));
	CreateRawWindowCapturer_func = reinterpret_cast<CreateDesktopCapturer_t>(handle->l_addr + CreateRawWindowCapturer_offset);

	funchook_t* funchook = funchook_create();
	funchook_prepare(funchook, reinterpret_cast<void**>(&CreateRawWindowCapturer_func), reinterpret_cast<void*>(WindowCapturer::CreateRawWindowCapturer));
	funchook_install(funchook, 0);

	return napi_register_module_v1_func(env, exports);
}
