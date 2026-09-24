// opengl-render-qnx-stream-player — pipelined multithreaded stream renderer
//
// Dual-Target Unified Architecture:
// - QNX ARM: OpenKODE (libdisplayinit.so) + GLES2 hardware texture mapping (Tegra 3 proven)
// - x86/Linux: GLFW + GLES2 hardware texture mapping
//
// Features:
// - 6-slot lock-free frame pool (zero runtime heap allocations inside 30 FPS loop)
// - Monotonic target clock pacer locked to 30.00 FPS
// - Direct fast YUV420p -> RGBA CPU conversion with macroblock stride compensation
// - 4-line centered HUD overlay rendered directly into RGBA buffer (immune to GL state bugs)
// - Seamless dual-mode: local file replay (--loop) and direct socket streaming (tcp://...)
// - Factory-safe DMDT display context activation (Context 3) and clean restoration (Context 33)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <stddef.h>

#ifdef __QNX__
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// ---------------- Configuration & Performance Knobs ----------------
static const int kBufferPoolSize    = 16;     // 16-slot frame pool for jitter smoothing & zero contention
static int   g_ffmpegThreadCount    = 2;      // Default: 2 worker threads for multi-core Tegra 3
/*
 * Zero frame delay by default. Frame threading holds each decoded frame until
 * the next packet arrives, and the render ACK the phone is waiting for only
 * comes after that frame is shown. When the phone is short of window credit it
 * sends the next packet only after that ACK, so every frame waits for a
 * phone-side timeout of about 300 ms: 3.3 fps with 313 ms ACK latency, seen on
 * the car 2026-09-16 (run 3). AV_CODEC_FLAG_LOW_DELAY makes FFmpeg skip frame
 * threading altogether; slice threading is declared too so the startup log
 * does not claim otherwise. Decode at 800x480 measured about 1 ms a frame, so
 * frame-parallel decode bought nothing. Not configurable: this is a fix, and
 * the old GAL_PLAYER_THREAD_TYPE / GAL_PLAYER_LOW_DELAY knobs (conf, env and
 * --thread-type / --low-delay flags) could only put the stall back.
 */
static const int  g_ffmpegThreadType = FF_THREAD_SLICE;
static const bool g_ffmpegLowDelay   = true;
static bool  g_ffmpegFastDecode     = true;
static int   g_ffmpegSkipFrame      = AVDISCARD_DEFAULT;
static int   g_ffmpegSkipLoopFilter = AVDISCARD_NONREF; // Default: noref eliminates macroblock prediction blur during movement
static int   g_idleMaxBytes         = 0;      // Stationary frame threshold (default: 0 = disabled for pure 30 FPS)
static int   g_idleHeartbeatHz      = 0;      // Stationary heartbeat keepalive rate in Hz (default: 0 = disabled)
static bool  g_loopFile             = false;
static bool  g_verbose              = true;
#ifndef __QNX__
static char  g_dumpVideoPath[512]   = {0};
static int   g_dumpMaxFrames        = 0;
static int   g_dumpFrameCount       = 0;
static FILE* g_dumpVideoPipe        = NULL;
#endif

// ---------------- Logging ----------------
#define LOG(fmt, ...) do {     printf("[stream-player] " fmt "\n", ##__VA_ARGS__);     fflush(stdout); } while(0)

#define LOG_STATS(fmt, ...) do {     printf("[stream-player STATS] " fmt "\n", ##__VA_ARGS__);     fflush(stdout); } while(0)

#define LOGE(fmt, ...) do {     fprintf(stderr, "[stream-player ERROR] " fmt "\n", ##__VA_ARGS__);     fflush(stderr); } while(0)

static inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static double us_to_ms(uint64_t us) {
    return (double)us / 1000.0;
}

static double get_process_memory_mb() {
#ifdef __QNX__
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/as", getpid());
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) {
            close(fd);
            return (double)st.st_size / (1024.0 * 1024.0);
        }
        close(fd);
    }
    return 0.0;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long total_pages = 0, rss_pages = 0;
        if (fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2) {
            fclose(f);
            long page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
            return (double)(rss_pages * page_size_kb) / 1024.0;
        }
        fclose(f);
    }
    return 0.0;
#endif
}

// ---------------- Frame Buffer Structures ----------------
struct RGBABuffer {
    uint8_t* pixels;
    int width;
    int height;
    size_t capacity;
    int packetBytes;
    bool isKeyframe;

    RGBABuffer() : pixels(NULL), width(0), height(0), capacity(0), packetBytes(0), isKeyframe(false) {}

    void ensureCapacity(int w, int h) {
        size_t total = (size_t)w * h + 2 * ((size_t)(w/2) * (h/2));
        if (capacity < total) {
            free(pixels);
            pixels = (uint8_t*)malloc(total);
            capacity = total;
        }
        width = w;
        height = h;
    }

    void release() { free(pixels); pixels = NULL; capacity = 0; }
};

// 6-Slot Ring Buffer Pool
struct FramePool {
    RGBABuffer slots[kBufferPoolSize];
    int writeSlot;
    int readySlot;
    int readSlot;

    FramePool() : writeSlot(0), readySlot(-1), readSlot(-1) {}

    RGBABuffer* getWriteBuffer(int w, int h) {
        slots[writeSlot].ensureCapacity(w, h);
        return &slots[writeSlot];
    }

    void publishWrite() {
        readySlot = writeSlot;
        writeSlot = (writeSlot + 1) % kBufferPoolSize;
        if (writeSlot == readSlot) {
            writeSlot = (writeSlot + 1) % kBufferPoolSize;
        }
    }

    RGBABuffer* getDisplayBuffer() {
        if (readySlot >= 0) {
            readSlot = readySlot;
            readySlot = -1;
        }
        if (readSlot >= 0) return &slots[readSlot];
        return NULL;
    }
};

static FramePool g_framePool;
static pthread_mutex_t g_frameMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frameCond  = PTHREAD_COND_INITIALIZER;

static volatile bool g_running = true;
static volatile int g_streamSock = -1;
static volatile bool g_newFrameReady = false;
static uint64_t g_decodedFrameCount = 0;
static uint64_t g_lastDecodeDurationUs = 0;
static char g_videoSource[512] = "unix:///tmp/gal_video.sock";
static bool g_isSocket = false;

int windowWidth  = 800;
int windowHeight = 480;

static GLfloat backgroundColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// Full-screen Quad Geometry (Screen dimensions: 800x480)
static const GLfloat landscapeVertices[] = {
    -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f
};

static const GLfloat landscapeTexCoords[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f
};

#ifdef __QNX__
static void* g_displayInitHandle = NULL;
static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;
#else
static GLFWwindow* g_glfwWindow = NULL;
#endif

#ifdef __QNX__
typedef void (*PFNGLDRAWTEXTURENVPROC)(GLuint texture, GLuint sampler,
                                       GLfloat x0, GLfloat y0,
                                       GLfloat x1, GLfloat y1,
                                       GLfloat z,
                                       GLfloat s0, GLfloat t0,
                                       GLfloat s1, GLfloat t1);
static PFNGLDRAWTEXTURENVPROC g_pfnDrawTextureNV = NULL;
#endif
static bool g_useShader = false;

static GLuint g_programObject = 0;
static GLint  g_posAttr = -1;
static GLint  g_texAttr = -1;
static GLint  g_texLocY = -1;
static GLint  g_texLocU = -1;
static GLint  g_texLocV = -1;
static GLuint g_texY = 0;
static GLuint g_texU = 0;
static GLuint g_texV = 0;

// Standard GLES2 RGBA Shaders (Proven 100% compatible with Tegra 3)
static const char* vertexShaderSource =
    "attribute vec2 position;    \n"
    "attribute vec2 texCoord;     \n"
    "varying vec2 v_texCoord;     \n"
    "void main()                  \n"
    "{                            \n"
    "   gl_Position = vec4(position, 0.0, 1.0); \n"
    "   v_texCoord = texCoord;   \n"
    "}                            \n";

static const char* fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texU;\n"
    "uniform sampler2D texV;\n"
    "void main()\n"
    "{\n"
    "    float y = texture2D(texY, v_texCoord).r;\n"
    "    float u = texture2D(texU, v_texCoord).r - 0.5;\n"
    "    float v = texture2D(texV, v_texCoord).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.34414 * u - 0.71414 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// ---------------- QNX DMDT Context Switching ----------------
/*
 * The cluster shows this player when context 70 points at displayable 3 (and
 * display 4 shows context 70); the stock Kombi map is displayable 33. Both
 * are set with dmdt, a separate process that took about 0.3 s a run on the
 * car. Android Auto allows 500 ms of video setup latency, on-screen
 * transition included, and 50 ms of output latency (HUIG 1.3 p.36).
 *
 * So the switch starts the moment the decoder has the phone's first keyframe
 * -- the hook grants the stream only once the Kombi map is ready, so nothing
 * is probed or waited for here -- and runs on its own threads, both commands
 * at once, while the render loop keeps drawing and ACKing. It used to wait
 * for 30 decoded frames (1 s), probe `dmdt gs` for the Kombi map, then run
 * the two commands one after the other on the render thread: about 2 s before
 * the cluster changed, with render ACKs held back 0.3-1.25 s at the switch
 * (car, 2026-09-16). Timestamps are CLOCK_MONOTONIC, the hook log's clock.
 */
#define T_ARGS(us) (unsigned long long)((us) / 1000000ULL), (unsigned long long)((us) / 1000ULL % 1000ULL)
static pid_t g_parentPid = 0;
#ifdef __QNX__
struct DmdtRun { const char* args; int status; };

static void* dmdt_run_thread(void* arg) {
    DmdtRun* run = (DmdtRun*)arg;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt %s 2>/dev/null",
             run->args);
    run->status = system(cmd);
    return NULL;
}

/*
 * Run the two dmdt commands sequentially: dc rewrites the context table,
 * and sc commits the displayable to the MOST encoder. sc must not run before dc.
 * Because display_switch_thread is already off the render thread, running
 * them sequentially does not block rendering or hold back frame ACKs.
 */
static void dmdt_pair(const char* a, const char* b, const char* what) {
    DmdtRun runs[2] = { { a, -1 }, { b, -1 } };
    uint64_t start = now_us();
    dmdt_run_thread(&runs[0]);
    dmdt_run_thread(&runs[1]);
    uint64_t end = now_us();
    LOG("dmdt: %s t=%llu.%03llu took=%llums ('dmdt %s'=%d, 'dmdt %s'=%d)", what, T_ARGS(end),
        (unsigned long long)((end - start) / 1000ULL), a, runs[0].status, b, runs[1].status);
}

static pthread_mutex_t g_switchLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_switchThread;
static bool g_switchStarted = false;   /* g_switchLock */
static bool g_switchClosed = false;    /* g_switchLock: shutting down */

static void* display_switch_thread(void* arg) {
    (void)arg;
    dmdt_pair("dc 70 3", "sc 4 70", "cluster switched to the player");
    return NULL;
}

/* Decoder thread, at the phone's first keyframe; once per player. */
static void request_display_switch() {
    pthread_mutex_lock(&g_switchLock);
    if (!g_switchStarted && !g_switchClosed) {
        LOG("dmdt: switching the cluster at the first keyframe t=%llu.%03llu", T_ARGS(now_us()));
        if (pthread_create(&g_switchThread, NULL, display_switch_thread, NULL) == 0)
            g_switchStarted = true;
        else
            LOGE("dmdt: could not start the switch thread");
    }
    pthread_mutex_unlock(&g_switchLock);
}

/*
 * Main thread, on the way out: let a switch in progress finish, then put the
 * stock map back -- only if this player switched it. A player that never
 * got a keyframe has nothing to undo, and runs no dmdt at all.
 */
static void execute_final_commands() {
    pthread_mutex_lock(&g_switchLock);
    bool started = g_switchStarted;
    g_switchClosed = true;
    pthread_mutex_unlock(&g_switchLock);
    if (!started) return;
    pthread_join(g_switchThread, NULL);
    dmdt_pair("dc 70 33", "sc 4 70", "stock Kombi map restored");
}
#else
static void request_display_switch() {}
static void execute_final_commands() {}
#endif

static void crash_signal_handler(int sig) {
    fprintf(stderr, "\n[stream-player CRASH] Caught fatal signal %d! Emergency restoring display context 33...\n", sig);
#ifdef __QNX__
    system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 33 2>/dev/null");
    system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70 2>/dev/null");
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    pthread_cond_broadcast(&g_frameCond);
    int s = g_streamSock;
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
    }
#ifndef __QNX__
    if (g_dumpVideoPipe) {
        pclose(g_dumpVideoPipe);
        g_dumpVideoPipe = NULL;
    }
#endif
}

// ---------------- Shader Compilation & Setup ----------------
static void check_shader(GLuint shader, const char* name) {
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log)-1, NULL, log);
        fprintf(stderr, "GL: shader '%s' compile failed: %s\n", name, log);
    } else {
        LOG("GL: shader '%s' compiled ok", name);
    }
}

static void check_program(GLuint prog, const char* name) {
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log)-1, NULL, log);
        fprintf(stderr, "GL: program '%s' link failed: %s\n", name, log);
    } else {
        LOG("GL: program '%s' linked ok", name);
    }
}

void InitGL() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, NULL);
    glCompileShader(vs);
    check_shader(vs, "vert-main");

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, NULL);
    glCompileShader(fs);
    check_shader(fs, "frag-main");

    g_programObject = glCreateProgram();
    glAttachShader(g_programObject, vs);
    glAttachShader(g_programObject, fs);
    glLinkProgram(g_programObject);
    check_program(g_programObject, "main");

    g_posAttr = glGetAttribLocation(g_programObject, "position");
    g_texAttr = glGetAttribLocation(g_programObject, "texCoord");
    g_texLocY = glGetUniformLocation(g_programObject, "texY");
    g_texLocU = glGetUniformLocation(g_programObject, "texU");
    g_texLocV = glGetUniformLocation(g_programObject, "texV");
    LOG("GL: attrib locations: pos=%d tex=%d", g_posAttr, g_texAttr);

    GLint linkStatus = 0;
    glGetProgramiv(g_programObject, GL_LINK_STATUS, &linkStatus);
    if (linkStatus) {
        g_useShader = true;
        LOG("GL: GLES2 shader pipeline ACTIVE");
    } else {
        LOGE("GL: shader link failed, probing for hardware blit extension...");
    }

#ifdef __QNX__
    g_pfnDrawTextureNV = (PFNGLDRAWTEXTURENVPROC)eglGetProcAddress("glDrawTextureNV");
    if (g_pfnDrawTextureNV) {
        LOG("GL: GL_NV_draw_texture hardware blitter available at %p", (void*)g_pfnDrawTextureNV);
    } else {
        LOG("GL: GL_NV_draw_texture NOT available");
    }

    if (!g_useShader && !g_pfnDrawTextureNV) {
        LOGE("FATAL: no rendering path (shader link failed AND glDrawTextureNV not found)");
        GLint numBinFmt = 0;
        glGetIntegerv(0x8DF9 /* GL_NUM_SHADER_BINARY_FORMATS */, &numBinFmt);
        LOGE("FATAL: GL_NUM_SHADER_BINARY_FORMATS = %d", numBinFmt);
    }
#endif

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLuint texs[3];
    glGenTextures(3, texs);
    g_texY = texs[0]; g_texU = texs[1]; g_texV = texs[2];
    for (int i=0; i<3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glViewport(0, 0, windowWidth, windowHeight);
    glClearColor(backgroundColor[0], backgroundColor[1], backgroundColor[2], backgroundColor[3]);
    LOG("GL: hardware RGBA texture & shaders initialized successfully");
}

// ---------------- H.264 Video Decoder Thread ----------------
/*
 * The hook opens its socket before it starts the player, so the first connect
 * succeeded for every player on the car. A retry is for the case where it
 * does not: once a second, and only while gal is still our parent -- a player
 * whose gal is gone has nobody to connect to.
 */
static bool connect_retry_allowed() {
    if (!g_running) return false;
    if (g_parentPid > 1 && getppid() != g_parentPid) {
        LOG("socket: gal (pid %ld) is gone; exiting", (long)g_parentPid);
        g_running = false;
        pthread_cond_broadcast(&g_frameCond);
        return false;
    }
    sleep(1);
    return g_running;
}

static void* DecoderThreadFunc(void* arg) {
    (void)arg;
    LOG("decoder: background worker thread active");

    bool is_socket = g_isSocket;
    bool is_unix = (strncmp(g_videoSource, "unix://", 7) == 0 ||
                    (strncmp(g_videoSource, "/tmp/", 5) == 0 && strstr(g_videoSource, ".sock") != NULL));
    char unix_path[512] = "/tmp/gal_video.sock";
    char host[512] = "127.0.0.1";
    int  port = 12346;

    if (is_unix) {
        if (strncmp(g_videoSource, "unix://", 7) == 0) {
            snprintf(unix_path, sizeof(unix_path), "%s", g_videoSource + 7);
        } else {
            snprintf(unix_path, sizeof(unix_path), "%s", g_videoSource);
        }
        LOG("network: target stream unix://%s", unix_path);
    } else if (g_isSocket) {
        const char* p = g_videoSource + 6;
        char* colon = (char*)strchr(p, ':');
        if (colon) {
            size_t host_len = colon - p;
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, p, host_len);
            host[host_len] = '\0';
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", p);
        }
        LOG("network: target stream tcp://%s:%d", host, port);
    } else {
        LOG("file: reading local H.264 file '%s' (loop=%d)", g_videoSource, g_loopFile ? 1 : 0);
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { LOGE("avcodec: H.264 decoder not found"); return NULL; }

    AVCodecParserContext* parser = av_parser_init(codec->id);
    if (!parser) { LOGE("avcodec: parser init failed"); return NULL; }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { LOGE("avcodec: context alloc failed"); av_parser_close(parser); return NULL; }

    codecCtx->thread_count = g_ffmpegThreadCount;
    codecCtx->thread_type = g_ffmpegThreadType;
    if (g_ffmpegLowDelay) {
        codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (g_ffmpegFastDecode) codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    codecCtx->skip_frame = (AVDiscard)g_ffmpegSkipFrame;
    codecCtx->skip_loop_filter = (AVDiscard)g_ffmpegSkipLoopFilter;

    const char* deblock_name = "DEFAULT";
    if (g_ffmpegSkipLoopFilter == AVDISCARD_ALL) deblock_name = "ALL (Deblocking DISABLED, saves ~25-30% CPU)";
    else if (g_ffmpegSkipLoopFilter == AVDISCARD_NONE) deblock_name = "NONE (Deblocking ENABLED)";
    else if (g_ffmpegSkipLoopFilter == AVDISCARD_NONREF) deblock_name = "NONREF (Deblocking skipped on non-reference frames)";

    const char* thread_type_name = "FRAME (Multi-core parallel)";
    if (g_ffmpegThreadType == FF_THREAD_SLICE) thread_type_name = "SLICE (Single-slice fallback)";
    else if (g_ffmpegThreadType == (FF_THREAD_FRAME | FF_THREAD_SLICE)) thread_type_name = "FRAME+SLICE";

    LOG("decoder: configuring FFmpeg (threads: %d, type: %s, low_delay: %d, fast: %d, skip_loop_filter: %s, idle_bytes: %d, heartbeat: %dHz)",
        g_ffmpegThreadCount, thread_type_name, g_ffmpegLowDelay ? 1 : 0, g_ffmpegFastDecode ? 1 : 0, deblock_name,
        g_idleMaxBytes, g_idleHeartbeatHz);

    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        LOGE("avcodec: open failed");
        avcodec_free_context(&codecCtx);
        av_parser_close(parser);
        return NULL;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();
    uint8_t* inbuf = (uint8_t*)av_malloc(262144 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!inbuf) {
        LOGE("avcodec: inbuf allocation failed");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        av_parser_close(parser);
        return NULL;
    }

    bool hasDecodedKeyframe = false;
    bool hasRenderedWarmupFrame = false;
    bool gotData = false;

    while (g_running) {
        int sock = -1;
        FILE* infile = NULL;

        if (is_socket) {
            if (is_unix) {
                sock = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sock < 0) { usleep(100000); continue; }

                struct sockaddr_un serv_un;
                memset(&serv_un, 0, sizeof(serv_un));
                serv_un.sun_family = AF_UNIX;
                strncpy(serv_un.sun_path, unix_path, sizeof(serv_un.sun_path) - 1);

                g_streamSock = sock;
                int rcv_buf = 2097152; // 2MB socket receive buffer for large I-frames
                setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_buf, sizeof(rcv_buf));

                LOG("socket: connecting to unix://%s...", unix_path);
                if (connect(sock, (struct sockaddr*)&serv_un, sizeof(serv_un)) < 0) {
                    g_streamSock = -1;
                    close(sock);
                    if (!connect_retry_allowed()) break;
                    continue;
                }
                LOG("socket: connected successfully to unix://%s!", unix_path);
                hasDecodedKeyframe = false;
                hasRenderedWarmupFrame = false;
            } else {
                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) { usleep(100000); continue; }

                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family = AF_INET;
                serv_addr.sin_port = htons(port);
                inet_pton(AF_INET, host, &serv_addr.sin_addr);

                g_streamSock = sock;
                int flag = 1;
                setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

                int rcv_buf = 2097152; // 2MB socket receive buffer
                setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_buf, sizeof(rcv_buf));

                LOG("socket: connecting to %s:%d...", host, port);
                if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                    g_streamSock = -1;
                    close(sock);
                    if (!connect_retry_allowed()) break;
                    continue;
                }
                LOG("socket: connected successfully to %s:%d!", host, port);
                hasDecodedKeyframe = false;
                hasRenderedWarmupFrame = false;
            }
        } else {
            infile = fopen(g_videoSource, "rb");
            if (!infile) {
                LOGE("file: failed to open '%s'", g_videoSource);
                break;
            }
        }

        while (g_running) {
            ssize_t bytes_read = 0;
            if (is_socket) {
                bytes_read = recv(sock, inbuf, 131072, 0);
                if (bytes_read < 0) {
                    if (errno == EINTR) continue;
                    LOG("socket: recv error (errno=%d) t=%llu.%03llu", errno, T_ARGS(now_us()));
                    break;
                } else if (bytes_read == 0) {
                    LOG("socket: stream closed t=%llu.%03llu", T_ARGS(now_us()));
                    break;
                }
                if (!gotData) {
                    gotData = true;
                    LOG("socket: first data t=%llu.%03llu", T_ARGS(now_us()));
                }
            } else {
                bytes_read = fread(inbuf, 1, 32768, infile);
                if (bytes_read <= 0) {
                    if (g_loopFile && g_running) {
                        rewind(infile);
                        continue;
                    }
                    LOG("file: reached EOF");
                    break;
                }
            }

            uint8_t* data = inbuf;
            size_t data_size = bytes_read;

            while (data_size > 0 && g_running) {
                int len = av_parser_parse2(parser, codecCtx,
                                          &packet->data, &packet->size,
                                          data, data_size,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (len < 0) break;
                data += len;
                data_size -= len;

                if (packet->size > 0) {
                    uint64_t t_dec_start = now_us();
                    int ret = avcodec_send_packet(codecCtx, packet);
                    if (ret < 0) continue;

                    while (ret >= 0 && g_running) {
                        ret = avcodec_receive_frame(codecCtx, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) break;

                        uint64_t dec_dur = now_us() - t_dec_start;

                        int w = frame->width;
                        int h = frame->height;

                        bool isKey = false;
#ifdef AV_FRAME_FLAG_KEY
                        if ((frame->flags & AV_FRAME_FLAG_KEY) != 0) isKey = true;
#endif
                        if (frame->key_frame != 0) isKey = true;
                        if (frame->pict_type == AV_PICTURE_TYPE_I) isKey = true;

                        // Keyframe Gate: Discard pre-keyframe P-frames to eliminate initial macroblock mosaic
                        if (!hasDecodedKeyframe) {
                            if (!isKey) {
                                av_frame_unref(frame);
                                continue;
                            }
                            hasDecodedKeyframe = true;
                            LOG("decoder: first IDR/I keyframe received (%dx%d) t=%llu.%03llu; warming up one frame.",
                                w, h, T_ARGS(now_us()));
                            request_display_switch();
                        }

                        /*
                         * The first decoded IDR establishes codec/reference
                         * state but can still contain stale macroblocks when
                         * a client joins in the middle of an AA session. Keep
                         * the pre-filled black frame visible for one decoded
                         * frame, then publish the next frame once the decoder
                         * has had a clean reference to work from.
                         */
                        if (!hasRenderedWarmupFrame) {
                            hasRenderedWarmupFrame = true;
                            av_frame_unref(frame);
                            LOG("decoder: warmup frame consumed; rendering next decoded frame.");
                            continue;
                        }

                        // Direct memcpy to contiguous YUV buffer
                        pthread_mutex_lock(&g_frameMutex);
                        RGBABuffer* buf = g_framePool.getWriteBuffer(w, h);
                        buf->packetBytes = packet->size;
                        buf->isKeyframe = isKey;
                        
                        uint8_t* dstY = buf->pixels;
                        uint8_t* dstU = dstY + (w * h);
                        uint8_t* dstV = dstU + ((w / 2) * (h / 2));

                        // Copy Y plane
                        for (int y = 0; y < h; ++y) {
                            memcpy(dstY + y * w, frame->data[0] + y * frame->linesize[0], w);
                        }
                        // Copy U plane
                        for (int y = 0; y < h / 2; ++y) {
                            memcpy(dstU + y * (w / 2), frame->data[1] + y * frame->linesize[1], w / 2);
                        }
                        // Copy V plane
                        for (int y = 0; y < h / 2; ++y) {
                            memcpy(dstV + y * (w / 2), frame->data[2] + y * frame->linesize[2], w / 2);
                        }

                        g_framePool.publishWrite();
                        g_newFrameReady = true;
                        g_decodedFrameCount++;
                        g_lastDecodeDurationUs = dec_dur;

                        pthread_cond_signal(&g_frameCond);
                        pthread_mutex_unlock(&g_frameMutex);

                        av_frame_unref(frame);

                        // Monotonic 30.00 FPS Target Clock Pacing for file playback
                        if (!is_socket) {
                            static uint64_t s_nextTargetUs = 0;
                            const uint64_t kFrameIntervalUs = 33333ULL;
                            uint64_t now = now_us();
                            if (s_nextTargetUs == 0 || now > s_nextTargetUs + 100000ULL) {
                                s_nextTargetUs = now + kFrameIntervalUs;
                            } else {
                                if (now < s_nextTargetUs) {
                                    usleep((useconds_t)(s_nextTargetUs - now));
                                }
                                s_nextTargetUs += kFrameIntervalUs;
                            }
                        }
                    }
                }
            }
        }

        if (sock >= 0) { g_streamSock = -1; close(sock); sock = -1; }
        if (infile) { fclose(infile); infile = NULL; }
        if (is_socket) {
            /*
             * No reconnect. The hook never closes a working player's stream:
             * a closed stream is gal exiting or replacing this player, and on
             * the car every one of them was (6 of 6 on 2026-09-16; the
             * reconnect attempt that followed always failed). Ending here is
             * also how the player notices gal is gone, without a watchdog
             * thread polling for it.
             */
            g_running = false;
            pthread_cond_broadcast(&g_frameCond);
            break;
        }
        if (!g_running) break;
        if (!g_loopFile) break;
    }

    av_free(inbuf);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    av_parser_close(parser);
    LOG("decoder: worker thread exited cleanly");
    return NULL;
}

static int check_and_acquire_instance_lock() {
    int fd = open("/tmp/stream-player.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) return 0; // If /tmp is inaccessible, proceed without lock
    struct flock fl;
    for (int retry = 0; retry < 5; ++retry) {
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (fcntl(fd, F_SETLK, &fl) == 0) {
            return fd; // Acquired exclusive process lock
        }
        if (errno == ENOSYS || errno == EINVAL) {
            // Filesystem (e.g. QNX /dev/shmem) does not support POSIX advisory locks
            return fd;
        }
        usleep(20000); // 20ms retry in case previous instance is exiting
    }
    close(fd);
    return -1; // Another instance is actively running!
}

static void load_player_config() {
    const char* env_val = getenv("GAL_PLAYER_DEBLOCK");
    if (!env_val) env_val = getenv("GAL_DEBLOCK");
    if (!env_val) env_val = getenv("GAL_SKIP_LOOP_FILTER");

    char conf_val[64] = {0};

    if (!env_val) {
        static const char* candidate_paths[] = {
            "/fs/sdb0/gal_dualscreen.conf",
            "/fs/sda0/gal_dualscreen.conf",
            "/tmp/gal_dualscreen.conf",
            "/eso/lib/gal_dualscreen/gal_dualscreen.conf",
            "./gal_dualscreen.conf",
            NULL
        };
        for (int i = 0; candidate_paths[i]; ++i) {
            FILE* f = fopen(candidate_paths[i], "r");
            if (!f) continue;
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char* p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
                if (strncmp(p, "GAL_PLAYER_DEBLOCK=", 19) == 0 ||
                    strncmp(p, "GAL_DEBLOCK=", 12) == 0 ||
                    strncmp(p, "GAL_SKIP_LOOP_FILTER=", 21) == 0) {
                    char* eq = strchr(p, '=');
                    if (eq) {
                        char* val = eq + 1;
                        while (*val == ' ' || *val == '\t') val++;
                        char* end = val + strlen(val) - 1;
                        while (end >= val && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
                            *end = '\0';
                            end--;
                        }
                        strncpy(conf_val, val, sizeof(conf_val) - 1);
                        break;
                    }
                }
            }
            fclose(f);
            if (conf_val[0] != '\0') {
                env_val = conf_val;
                break;
            }
        }
    }

    if (env_val) {
        if (strcmp(env_val, "all") == 0 || strcmp(env_val, "0") == 0 ||
            strcasecmp(env_val, "false") == 0 || strcasecmp(env_val, "off") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
        } else if (strcmp(env_val, "none") == 0 || strcmp(env_val, "1") == 0 ||
                   strcasecmp(env_val, "true") == 0 || strcasecmp(env_val, "on") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
        } else if (strcmp(env_val, "noref") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONREF;
        }
    }

    // Parse candidate config files for additional performance settings
    static const char* conf_files[] = {
        "/fs/sdb0/gal_dualscreen.conf",
        "/fs/sda0/gal_dualscreen.conf",
        "/tmp/gal_dualscreen.conf",
        "/eso/lib/gal_dualscreen/gal_dualscreen.conf",
        "./gal_dualscreen.conf",
        NULL
    };
    for (int i = 0; conf_files[i]; ++i) {
        FILE* f = fopen(conf_files[i], "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* p = line;
            while (*p == ' ' || *p == '	') p++;
            if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
            char* eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char* key = p;
            char* val = eq + 1;
            while (*val == ' ' || *val == '	') val++;
            char* end = val + strlen(val) - 1;
            while (end >= val && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '	')) {
                *end = '\0';
                end--;
            }
            if (strcmp(key, "GAL_PLAYER_THREADS") == 0) {
                int t = atoi(val);
                if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
            } else if (strcmp(key, "GAL_PLAYER_IDLE_BYTES") == 0) {
                g_idleMaxBytes = atoi(val);
            } else if (strcmp(key, "GAL_PLAYER_HEARTBEAT_HZ") == 0) {
                g_idleHeartbeatHz = atoi(val);
            }
        }
        fclose(f);
        break; // Stop at first found config file
    }

    // Environment variables override config file
    const char* env_threads = getenv("GAL_PLAYER_THREADS");
    if (env_threads) {
        int t = atoi(env_threads);
        if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
    }
    const char* env_idle_bytes = getenv("GAL_PLAYER_IDLE_BYTES");
    if (env_idle_bytes) {
        g_idleMaxBytes = atoi(env_idle_bytes);
    }
    const char* env_hb = getenv("GAL_PLAYER_HEARTBEAT_HZ");
    if (env_hb) {
        g_idleHeartbeatHz = atoi(env_hb);
    }
}

// ---------------- Application Entry Point ----------------

#if defined(__QNX__) || defined(__linux__)
static int g_ack_fd = -1;
/*
 * At startup, not at the first swap: the hook grants the phone's stream only
 * once this socket is connected, and before the stream there is nothing to
 * swap. (It used to connect on the swap of an idle "WAITING FOR SIGNAL"
 * frame drawn after 1 s, which is why it came 1.2-1.5 s after the spawn.)
 * A failed connect is retried on the next ACK.
 */
static void ack_connect() {
    if (g_ack_fd >= 0) return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/gal_ack.sock", sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    g_ack_fd = fd;
    LOG("ack: connected t=%llu.%03llu", T_ARGS(now_us()));
}

static void write_ack() {
    ack_connect();
    if (g_ack_fd >= 0) {
        char b = 1;
        /* MSG_NOSIGNAL: once gal is gone a late ACK must fail, not raise
         * SIGPIPE and kill the player before it restores the Kombi map. */
        int n = send(g_ack_fd, &b, 1, MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(g_ack_fd);
            g_ack_fd = -1;
        }
    }
}
#else
static void ack_connect() {}
static void write_ack() {}
#endif

int main(int argc, char* argv[]) {
    g_parentPid = getppid();
    int lock_fd = check_and_acquire_instance_lock();
    if (lock_fd < 0) {
        LOG("stream-player: another instance is already running, exiting immediately to prevent duplicate renderers");
        return 0;
    }
    load_player_config();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--loop") == 0) {
            g_loopFile = true;
        } else if (strcmp(argv[i], "--overlay") == 0 || strcmp(argv[i], "--no-overlay") == 0) {
            /* Retained as accepted no-op options; the on-video metrics HUD was removed. */
        } else if (strcmp(argv[i], "--no-deblock") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
        } else if (strcmp(argv[i], "--deblock") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
        } else if (strncmp(argv[i], "--skip-loop-filter=", 19) == 0) {
            const char* opt = argv[i] + 19;
            if (strcmp(opt, "all") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
            else if (strcmp(opt, "none") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
            else if (strcmp(opt, "noref") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_NONREF;
        } else if (strncmp(argv[i], "--dot-px=", 9) == 0) {
            /* Backward-compatible no-op; the on-video metrics HUD was removed. */
        } else if (strncmp(argv[i], "--threads=", 10) == 0) {
            int t = atoi(argv[i] + 10);
            if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
        } else if (strncmp(argv[i], "--idle-bytes=", 13) == 0) {
            g_idleMaxBytes = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "--heartbeat-hz=", 15) == 0) {
            g_idleHeartbeatHz = atoi(argv[i] + 15);
#ifndef __QNX__
        } else if (strncmp(argv[i], "--dump-video=", 13) == 0) {
            snprintf(g_dumpVideoPath, sizeof(g_dumpVideoPath), "%s", argv[i] + 13);
        } else if (strncmp(argv[i], "--dump-frames=", 14) == 0) {
            g_dumpMaxFrames = atoi(argv[i] + 14);
#endif
        } else if (strncmp(argv[i], "--url=", 6) == 0) {
            snprintf(g_videoSource, sizeof(g_videoSource), "%s", argv[i] + 6);
        } else if (argv[i][0] != '-') {
            snprintf(g_videoSource, sizeof(g_videoSource), "%s", argv[i]);
        }
    }

    bool is_unix_sock = (strncmp(g_videoSource, "unix://", 7) == 0 ||
                         (strncmp(g_videoSource, "/tmp/", 5) == 0 && strstr(g_videoSource, ".sock") != NULL));
    g_isSocket = (strncmp(g_videoSource, "tcp://", 6) == 0 || is_unix_sock);
    LOG("stream-player starting (source: %s, loop: %d, pool: %d)",
        g_videoSource, g_loopFile ? 1 : 0, kBufferPoolSize);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGBUS,  crash_signal_handler);
    signal(SIGILL,  crash_signal_handler);
    signal(SIGFPE,  crash_signal_handler);

#ifdef __QNX__
    // Keep libdisplayinit.so resident for the entire application lifetime!
    g_displayInitHandle = dlopen("libdisplayinit.so", RTLD_LAZY);
    if (!g_displayInitHandle) {
        fprintf(stderr, "Error loading libdisplayinit.so: %s\n", dlerror());
        return 1;
    }

    void (*display_init)(int, int) = (void (*)(int, int))dlsym(g_displayInitHandle, "display_init");
    if (display_init) display_init(0, 0);

    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        fprintf(stderr, "EGL: no display\n");
        return 1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(eglDisplay, &major, &minor)) {
        fprintf(stderr, "EGL: eglInitialize failed (0x%x)\n", eglGetError());
        return 1;
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1, EGL_ALPHA_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig configs[5];
    EGLint num_configs = 0;
    EGLNativeWindowType windowEgl = 0;
    int kdWindow = 0;

    eglChooseConfig(eglDisplay, config_attribs, configs, 5, &num_configs);
    if (num_configs == 0) {
        fprintf(stderr, "EGL: no suitable config found\n");
        return 1;
    }

    void (*display_create_window)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*) =
        (void (*)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*))dlsym(g_displayInitHandle, "display_create_window");

    if (display_create_window) {
        // Displayable 3 = Center display on Virtual Cockpit
        display_create_window(eglDisplay, configs[0], windowWidth, windowHeight, 3, &windowEgl, &kdWindow);
        LOG("display: window created %dx%d (win=%p, kd=%d)", windowWidth, windowHeight, (void*)windowEgl, kdWindow);
    }

    eglSurface = eglCreateWindowSurface(eglDisplay, configs[0], windowEgl, 0);
    if (eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "EGL: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return 1;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, configs[0], EGL_NO_CONTEXT, context_attribs);
    if (eglContext == EGL_NO_CONTEXT) {
        fprintf(stderr, "EGL: eglCreateContext failed (0x%x)\n", eglGetError());
        return 1;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        fprintf(stderr, "EGL: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 1;
    }
    // Non-blocking buffer swap (interval 0):
    // Prevents eglSwapBuffers() from hanging on lost hardware VSYNC when
    // the Virtual Cockpit display powers off during vehicle ignition OFF.
    eglSwapInterval(eglDisplay, 0);
#else
    if (!glfwInit()) {
        LOGE("GLFW: failed to initialize");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    g_glfwWindow = glfwCreateWindow(windowWidth, windowHeight, "VW Virtual Cockpit - Unified GLES2 Player", NULL, NULL);
    if (!g_glfwWindow) {
        LOGE("GLFW: failed to create window");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowCloseCallback(g_glfwWindow, [](GLFWwindow* w) {
        (void)w;
        g_running = false;
        pthread_cond_broadcast(&g_frameCond);
    });
    glfwMakeContextCurrent(g_glfwWindow);
    glfwSwapInterval(0); // Driven by our monotonic target clock
#endif

    InitGL();
    ack_connect();

    // Pre-fill slot 0 with a blank black frame (Y=16, U=128, V=128)
    RGBABuffer* initBuf = g_framePool.getWriteBuffer(windowWidth, windowHeight);
    memset(initBuf->pixels, 16, (size_t)windowWidth * windowHeight);
    memset(initBuf->pixels + (size_t)windowWidth * windowHeight, 128, ((size_t)windowWidth * windowHeight) / 2);
    g_framePool.publishWrite();

    pthread_attr_t decAttr;
    pthread_attr_init(&decAttr);
    pthread_attr_setstacksize(&decAttr, 1024 * 1024);

    pthread_t decThread;
    if (pthread_create(&decThread, &decAttr, DecoderThreadFunc, NULL) != 0) {
        LOGE("decoder: failed to create background thread");
        pthread_attr_destroy(&decAttr);
        return 1;
    }
    pthread_attr_destroy(&decAttr);

    int prevFbW = 0, prevFbH = 0;
    bool firstFrame = true;

    uint64_t lastFpsUs = now_us();
    uint64_t frameCount = 0;
    double renderFps = 0.0;
    double decodeFps = 0.0;
    uint64_t lastDecCount = 0;
    double renderLatencyMs = 0.0;

    struct rusage lastUsage;
    getrusage(RUSAGE_SELF, &lastUsage);
    uint64_t lastCpuSampleUs = now_us();
    double cpuPct = 0.0;
    double processMemMb = 0.0;

    while (g_running) {
        pthread_mutex_lock(&g_frameMutex);
        while (!g_newFrameReady && g_running) {
            /*
             * The decoder signals every frame, and shutdown broadcasts, so
             * this sleeps until there is something to draw. The timeout only
             * bounds a wakeup lost to the signal handler; nothing is drawn
             * without a frame (the cluster is not showing this window before
             * the first keyframe anyway).
             */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_frameCond, &g_frameMutex, &ts);
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_frameMutex);
            break;
        }
        g_newFrameReady = false;

        uint64_t frameStartUs = now_us();

        RGBABuffer* dispBuf = g_framePool.getDisplayBuffer();
        if (dispBuf && dispBuf->pixels) {
            static uint64_t s_lastRenderUs = 0;
            bool isIdle = (g_idleMaxBytes > 0 && dispBuf->packetBytes > 0 &&
                           dispBuf->packetBytes <= g_idleMaxBytes && !dispBuf->isKeyframe);
            uint64_t heartbeatIntervalUs = (g_idleHeartbeatHz > 0) ? (1000000ULL / (uint64_t)g_idleHeartbeatHz) : 0ULL;
            uint64_t timeSinceRenderUs = frameStartUs - s_lastRenderUs;

            // Idle render bypass: when stationary, only render on heartbeat keepalive (~333ms at 3Hz)
            if (isIdle && heartbeatIntervalUs > 0 && s_lastRenderUs > 0 && timeSinceRenderUs < heartbeatIntervalUs) {
                pthread_mutex_unlock(&g_frameMutex);
                continue; // Skip texture upload & swap; previous surface stays presented cleanly
            }
            s_lastRenderUs = frameStartUs;

            int w = dispBuf->width;
            int h = dispBuf->height;

            uint8_t* pY = dispBuf->pixels;
            uint8_t* pU = pY + (w * h);
            uint8_t* pV = pU + ((w / 2) * (h / 2));

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_texY);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pY);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, pY);
            }

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_texU);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pU);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w/2, h/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, pU);
            }

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_texV);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pV);
                

                
                prevFbW = w;
                prevFbH = h;
                if (firstFrame)
                    LOG("render: first frame drawn t=%llu.%03llu", T_ARGS(now_us()));
                firstFrame = false;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w/2, h/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, pV);
            }
            
            pthread_mutex_unlock(&g_frameMutex);

            // Render to screen (GPU YUV Shader)
            glViewport(0, 0, windowWidth, windowHeight);
            glClear(GL_COLOR_BUFFER_BIT);

            if (g_useShader) {
                glUseProgram(g_programObject);
                glUniform1i(g_texLocY, 0);
                glUniform1i(g_texLocU, 1);
                glUniform1i(g_texLocV, 2);

                glVertexAttribPointer(g_posAttr, 3, GL_FLOAT, GL_FALSE, 0, landscapeVertices);
                glVertexAttribPointer(g_texAttr, 2, GL_FLOAT, GL_FALSE, 0, landscapeTexCoords);
                glEnableVertexAttribArray(g_posAttr);
                glEnableVertexAttribArray(g_texAttr);

                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                glDisableVertexAttribArray(g_posAttr);
                glDisableVertexAttribArray(g_texAttr);
            } else {
                static int s_noDrawWarn = 0;
                if (s_noDrawWarn++ < 3)
                    LOGE("render: SHADER PATH FAILED -- GPU YUV requires shaders! (Screen is blank)");
            }

#ifdef __QNX__
            if (!eglSwapBuffers(eglDisplay, eglSurface)) {
                EGLint err = eglGetError();
                if (err == EGL_CONTEXT_LOST) {
                    LOGE("render: EGL_CONTEXT_LOST (display power down or surface invalidated)");
                }
            }
            write_ack();
#else
            if (!g_dumpVideoPipe) {
                const char* dumpPath = getenv("GAL_PLAYER_DUMP_VIDEO");
                if ((!dumpPath || strlen(dumpPath) == 0) && g_dumpVideoPath[0] != '\0') {
                    dumpPath = g_dumpVideoPath;
                }
                const char* dumpFramesEnv = getenv("GAL_PLAYER_DUMP_FRAMES");
                if (dumpFramesEnv && g_dumpMaxFrames == 0) {
                    g_dumpMaxFrames = atoi(dumpFramesEnv);
                }
                if (dumpPath && strlen(dumpPath) > 0) {
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd),
                             "ffmpeg -y -f rawvideo -vcodec rawvideo -pix_fmt rgba -s %dx%d -r 30 -i - -vf vflip -c:v libx264 -pix_fmt yuv420p -preset fast \"%s\" >/dev/null 2>&1",
                             windowWidth, windowHeight, dumpPath);
                    g_dumpVideoPipe = popen(cmd, "w");
                    if (g_dumpVideoPipe) {
                        LOG("video dump: recording to %s (%dx%d @ 30 FPS, target_frames: %d)", dumpPath, windowWidth, windowHeight, g_dumpMaxFrames);
                    } else {
                        LOGE("video dump: failed to open ffmpeg pipe for %s", dumpPath);
                    }
                }
            }
            if (g_dumpVideoPipe) {
                static uint8_t* s_dumpBuf = NULL;
                if (!s_dumpBuf) {
                    s_dumpBuf = (uint8_t*)malloc((size_t)windowWidth * windowHeight * 4);
                }
                glReadPixels(0, 0, windowWidth, windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, s_dumpBuf);
                fwrite(s_dumpBuf, 1, (size_t)windowWidth * windowHeight * 4, g_dumpVideoPipe);
                g_dumpFrameCount++;
                if (g_dumpMaxFrames > 0 && g_dumpFrameCount >= g_dumpMaxFrames) {
                    LOG("video dump: reached target %d frames, terminating", g_dumpFrameCount);
                    g_running = false;
                }
            }
            glfwSwapBuffers(g_glfwWindow);
            glfwPollEvents();
            write_ack();
#endif
        } else {
            pthread_mutex_unlock(&g_frameMutex);
        }

        frameCount++;
        uint64_t frameEndUs = now_us();
        renderLatencyMs = us_to_ms(frameEndUs - frameStartUs);

        if ((frameEndUs - lastFpsUs) >= 1000000ULL) {
            uint64_t elapsedUs = frameEndUs - lastFpsUs;
            renderFps = (double)frameCount * 1000000.0 / (double)elapsedUs;

            uint64_t curDec = g_decodedFrameCount;
            decodeFps = (double)(curDec - lastDecCount) * 1000000.0 / (double)elapsedUs;
            lastDecCount = curDec;
            frameCount = 0;
            lastFpsUs = frameEndUs;

            struct rusage curUsage;
            getrusage(RUSAGE_SELF, &curUsage);
            uint64_t userUs = (curUsage.ru_utime.tv_sec - lastUsage.ru_utime.tv_sec) * 1000000ULL +
                              (curUsage.ru_utime.tv_usec - lastUsage.ru_utime.tv_usec);
            uint64_t sysUs  = (curUsage.ru_stime.tv_sec - lastUsage.ru_stime.tv_sec) * 1000000ULL +
                              (curUsage.ru_stime.tv_usec - lastUsage.ru_stime.tv_usec);
            uint64_t cpuElapsedUs = frameEndUs - lastCpuSampleUs;
            if (cpuElapsedUs > 0) {
                cpuPct = (double)(userUs + sysUs) * 100.0 / (double)cpuElapsedUs;
            }
            lastUsage = curUsage;
            lastCpuSampleUs = frameEndUs;

            processMemMb = get_process_memory_mb();

            if (g_verbose) {
                LOG_STATS("DISP: %5.1f fps | DEC: %5.1f fps (%4.1fms) | LAT: %4.1fms | CPU: %5.1f%% | MEM: %5.1f MB",
                          renderFps, decodeFps, us_to_ms(g_lastDecodeDurationUs),
                          renderLatencyMs, cpuPct, processMemMb);
            }
        }
    }

    // Put the stock Kombi map back (if this player switched it) before anything else.
    execute_final_commands();

    // Ensure decoder thread is unblocked from recv()
    g_running = false;
    int s = g_streamSock;
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
    }

#ifndef __QNX__
    if (g_dumpVideoPipe) {
        pclose(g_dumpVideoPipe);
        g_dumpVideoPipe = NULL;
    }
#endif
    LOG("shutting down: waiting for decoder thread");
    pthread_join(decThread, NULL);

#ifdef __QNX__
    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, eglSurface);
        if (eglContext != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay, eglContext);
        eglTerminate(eglDisplay);
    }
    if (g_displayInitHandle) {
        // Do not call display_deinit() on exit: Tegra driver deadlocks on semaphore fc522a2c
        // while the display pipeline is active. The OS reclaims resources automatically.
        dlclose(g_displayInitHandle);
    }
#else
    if (g_glfwWindow) {
        glfwDestroyWindow(g_glfwWindow);
        glfwTerminate();
    }
#endif

    LOG("stream-player terminated cleanly");
    return 0;
}
