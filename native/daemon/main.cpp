// hal_inject_daemon — Frida-free camera injection daemon
// Runs as root on Pixel 7a.
//
// Injection chain (no Frida anywhere):
//   IN-PROCESS HOOK — cam_injector ptrace-loads libcam_hook.so into cameraserver.
//               libcam_hook.so uses an inline arm64 hook on
//               Camera3Stream::returnBuffer to replace back-camera gralloc
//               dmabufs with the injected frame. This is the only write path:
//               an external /proc/PID/mem write cannot reach the gralloc output
//               buffer (it is shared into the *consumer* process, and the HAL's
//               own mappings are internal ISP buffers overwritten by DMA).
//
// Unix socket protocol (line-based, AF_UNIX /data/local/tmp/hal_inject/daemon.sock):
//   → SET_IMAGE <path>   copy image, generate NV21
//   → START              arm injection, (re-)inject libcam_hook.so
//   → STOP               disarm injection
//   → STATUS             JSON status line
//   → BACK_NODE          current back-camera device node
// Response: "OK\n" | "ERROR <msg>\n" | "STATUS <json>\n"

#include "bpf_loader.h"
#include "cam_detect.h"
#include "jpeg_yuv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <sstream>
#include <time.h>
#include <linux/input.h>

#define SOCK_PATH    "/data/local/tmp/hal_inject/daemon.sock"
#define CTRL_FILE    "/data/local/tmp/hal_inject/ctrl"
#define CTRL_YUV     "/data/local/tmp/hal_inject/ctrl_yuv"  // min YUV width, "0"=off
#define CTRL_CAM     "/data/local/tmp/hal_inject/ctrl_cam"  // target facing back/front/both
#define CUR_CAM      "/data/local/tmp/hal_inject/cur_cam"   // active facing (daemon-written)
#define CTRL_SLOT    "/data/local/tmp/hal_inject/ctrl_slot" // image slot idfront/idback
#define STATS_FILE   "/data/local/tmp/hal_inject/stats.json"
#define FRAME_JPEG   "/data/local/tmp/hal_inject/frame.jpg"
#define FRAME_NV21   "/data/local/tmp/hal_inject/frame.nv21"
#define FRAME_NV12   "/data/local/tmp/hal_inject/frame.nv12"
#define FRAME_DIM    "/data/local/tmp/hal_inject/frame.dim"   // "WxH" of frame.nv21/nv12
#define CTRL_FMT     "/data/local/tmp/hal_inject/ctrl_fmt"    // "nv21"(default)/"nv12"
#define CTRL_REGION  "/data/local/tmp/hal_inject/ctrl_region" // "cx cy [w h|size]" normalized; app sets this from Place-Frame placement
#define CTRL_COMPOSITE "/data/local/tmp/hal_inject/ctrl_composite" // "1"=Step-1 region composite (ON by default), else legacy full-overwrite
#define BPF_OBJ      "/data/local/tmp/hal_inject/cam_track.bpf.o"
#define CAM_INJECTOR "/data/local/tmp/hal_inject/cam_injector"
// cameraserver runs in the [system] section's ISOLATED linker namespace, whose
// search path is /system/${LIB}. A hook shipped at /system/lib64 (via the
// KernelSU module's systemless mount, label system_lib_file) loads cleanly;
// /data/local/tmp is not reliably loadable there even though /data is permitted.
// Prefer the system path, fall back to /data/local/tmp for ad-hoc testing.
#define CAM_HOOK_SO_SYS "/system/lib64/libcam_hook.so"
#define CAM_HOOK_SO     "/data/local/tmp/hal_inject/libcam_hook.so"

static std::atomic<bool>  g_inject_active{false};
static std::atomic<bool>  g_hook_loaded{false};
static std::string        g_image_path;
static std::string        g_back_node;
static std::mutex         g_mu;

// ─── ctrl file ────────────────────────────────────────────────────────────

static void write_ctrl(const char *val) {
    int fd = open(CTRL_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, val, strlen(val));
    close(fd);
}

static void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, val, strlen(val));
    close(fd);
}

// Read CTRL_CAM target: 0=back, 1=front, 2=both. Default back.
static int read_cam_target() {
    char b[8] = {};
    int fd = open(CTRL_CAM, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    if (b[0] == 'f') return 1;
    if (b[0] == 'b' && b[1] == 'o') return 2;
    return 0;
}

// ─── Active-camera facing detector (writes CUR_CAM) ─────────────────────────
// Tails `logcat -s CameraService:I`. The native CameraService logs every
// camera open and close with the LOGICAL camera id:
//   CameraService::connect call (PID 123 "pkg", camera ID 0) and Camera API version 2
//   disconnect: Disconnected client for camera 0 for PID 123
// id 0 = back, id 1 = front (confirmed on this device: id 1 => is_front_camera 1).
//
// Why logcat and not `dumpsys media.camera`: dumpsys takes the camera-service
// lock to render its report, which delays the streaming client's buffer
// returns and trips the capture watchdog ("fatal error (4)"), crashing the
// camera. logcat only reads the logd ring buffer — it never touches the
// camera service — so it is safe to run continuously while a camera streams.

static void set_cur_cam(int facing) {   // -1 none, 0 back, 1 front
    static int written = -2;
    if (facing == written) return;
    write_file(CUR_CAM, facing == 1 ? "front" : facing == 0 ? "back" : "");
    fprintf(stderr, "[daemon] active camera facing: %s\n",
            facing == 1 ? "front" : facing == 0 ? "back" : "none");
    written = facing;
}

static void cam_facing_logcat_loop() {
    // Replays the existing CameraService buffer first (so an already-open
    // camera is detected at startup), then tails live. Restarts the pipe if
    // logcat ever exits (logd restart / buffer wipe).
    while (true) {
        FILE *p = popen("logcat -s CameraService:I 2>/dev/null", "r");
        if (!p) { sleep(2); continue; }
        char line[1024];
        int cur_id = -1;     // logical id of the camera currently considered active
        while (fgets(line, sizeof(line), p)) {
            char *c = strstr(line, "camera ID ");
            if (c) {                                        // open
                cur_id = atoi(c + 10);
                set_cur_cam(cur_id == 1 ? 1 : 0);
                continue;
            }
            c = strstr(line, "Disconnected client for camera ");
            if (c) {                                        // close
                int did = atoi(c + 31);
                if (did == cur_id) { cur_id = -1; set_cur_cam(-1); }
                continue;
            }
        }
        pclose(p);           // logcat exited unexpectedly — restart the tail
        usleep(500000);
    }
}

// ─── Volume-up double-press monitor (toggles CTRL_CAM front<->back) ─────────

static long mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Read CTRL_SLOT: 0=idfront, 1=idback.
static int read_cam_slot() {
    char b[12] = {};
    int fd = open(CTRL_SLOT, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    return strstr(b, "back") ? 1 : 0;
}

static void volume_monitor_loop() {
    // gpio_keys lives at /dev/input/event0 on the Pixel 7a and carries both volume keys.
    //   vol-up  double-press -> toggle inject camera facing (front <-> back)
    //   vol-down double-press -> toggle injected media slot (ID front <-> ID back)
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[daemon] volume monitor: open event0 failed (%s)\n",
                strerror(errno));
        return;
    }
    struct input_event ev;
    long last_up = 0, last_dn = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY || ev.value != 1) continue;   // key-down only
        long t = mono_ms();
        if (ev.code == KEY_VOLUMEUP) {
            if (last_up != 0 && t - last_up <= 450) {
                const char *nv = (read_cam_target() == 0) ? "front" : "back";
                write_file(CTRL_CAM, nv);
                fprintf(stderr, "[daemon] vol-up x2 -> camera: %s\n", nv);
                last_up = 0;
            } else last_up = t;
        } else if (ev.code == KEY_VOLUMEDOWN) {
            if (last_dn != 0 && t - last_dn <= 450) {
                const char *ns = (read_cam_slot() == 0) ? "idback" : "idfront";
                write_file(CTRL_SLOT, ns);
                fprintf(stderr, "[daemon] vol-dn x2 -> slot: %s\n", ns);
                last_dn = 0;
            } else last_dn = t;
        }
    }
    close(fd);
}

// ─── IN-PROCESS HOOK: ptrace injection of libcam_hook.so ──────────────────

static bool run_injector() {
    // Prefer the module-provided /system/lib64 copy (loadable from cameraserver's
    // isolated namespace); fall back to the /data copy if the module isn't installed.
    const char *hook = (access(CAM_HOOK_SO_SYS, R_OK) == 0) ? CAM_HOOK_SO_SYS : CAM_HOOK_SO;
    fprintf(stderr, "[daemon] injecting hook: %s\n", hook);
    pid_t p = fork();
    if (p == 0) {
        const char *argv[] = { CAM_INJECTOR, "cameraserver", hook, nullptr };
        execv(CAM_INJECTOR, const_cast<char **>(argv));
        _exit(1);
    }
    if (p < 0) return false;
    int status;
    waitpid(p, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    g_hook_loaded.store(ok);
    fprintf(stderr, "[daemon] cam_injector %s\n", ok ? "succeeded" : "FAILED");
    return ok;
}

// ─── Socket command handler ───────────────────────────────────────────────

static void handle_client(int cfd) {
    char line[512] = {};
    ssize_t n = recv(cfd, line, sizeof(line) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    for (int i = 0; i < n; i++)
        if (line[i] == '\n' || line[i] == '\r') line[i] = 0;

    std::string cmd(line);
    std::string resp = "OK\n";

    if (cmd.size() > 10 && cmd.substr(0, 9) == "SET_IMAGE") {
        std::string path = cmd.substr(10);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_image_path = path;
        }
        // Use execv — never pass user input to system()
        {
            pid_t p = fork();
            if (p == 0) {
                execl("/system/bin/cp", "cp", path.c_str(), FRAME_JPEG, nullptr);
                _exit(1);
            }
            if (p > 0) { int s; waitpid(p, &s, 0); }
        }
        // Convert the JPEG to raw NV21 + NV12 (capped to 1920 long side) for YUV
        // stream injection. Native AImageDecoder — no ffmpeg needed. The .dim
        // sidecar records the real size, so it never disagrees with the frames.
        bool conv = jpeg_to_yuv(FRAME_JPEG, FRAME_NV21, FRAME_NV12, FRAME_DIM, 1920);
        if (!conv) fprintf(stderr, "[daemon] jpeg->yuv conversion failed\n");

    } else if (cmd.size() > 15 && cmd.substr(0, 14) == "SET_SLOT_IMAGE") {
        // SET_SLOT_IMAGE <idfront|idback> <yuv_jpg> [<capture_jpg>]
        //   <yuv_jpg>     -> frame_<slot>.nv21/.nv12/.dim  (YUV preview/analysis
        //                    streams — the consumer applies the sensor rotation)
        //   <capture_jpg> -> frame_<slot>.jpg              (BLOB still-capture,
        //                    saved AS-IS, so it carries its own rotation)
        // If <capture_jpg> is omitted, <yuv_jpg> feeds both (legacy single-orient).
        std::string rest = cmd.substr(15);
        size_t sp = rest.find(' ');
        std::string slot  = (sp == std::string::npos) ? "" : rest.substr(0, sp);
        std::string paths = (sp == std::string::npos) ? "" : rest.substr(sp + 1);
        size_t sp2 = paths.find(' ');
        std::string yuv_path  = (sp2 == std::string::npos) ? paths : paths.substr(0, sp2);
        std::string blob_path = (sp2 == std::string::npos) ? yuv_path : paths.substr(sp2 + 1);
        if ((slot != "idfront" && slot != "idback") || yuv_path.empty()) {
            resp = "ERROR bad args\n";
        } else {
            std::string base = std::string("/data/local/tmp/hal_inject/frame_") + slot;
            std::string jpg = base + ".jpg";
            // BLOB still-capture source (separately-oriented)
            pid_t p = fork();
            if (p == 0) {
                execl("/system/bin/cp", "cp", blob_path.c_str(), jpg.c_str(), nullptr);
                _exit(1);
            }
            if (p > 0) { int s; waitpid(p, &s, 0); }
            // YUV source: convert the YUV-oriented jpeg straight into the raw planes.
            std::string nv21 = base + ".nv21", nv12 = base + ".nv12", dim = base + ".dim";
            if (!jpeg_to_yuv(yuv_path.c_str(), nv21.c_str(), nv12.c_str(), dim.c_str(), 1920))
                resp = "ERROR convert\n";
        }

    } else if (cmd == "START") {
        write_ctrl("on");
        g_inject_active.store(true);
        // In-process hook (libcam_hook.so) performs the injection from inside
        // cameraserver; load/reload it now.
        run_injector();
        resp = "OK\n";

    } else if (cmd == "STOP") {
        write_ctrl("off");
        g_inject_active.store(false);
        g_hook_loaded.store(false);
        resp = "OK\n";

    } else if (cmd == "STATUS") {
        // Read frame counter from stats file written by libcam_hook.so
        char sbuf[256] = "{}";
        int sfd = open(STATS_FILE, O_RDONLY);
        if (sfd >= 0) { read(sfd, sbuf, sizeof(sbuf) - 1); close(sfd); }
        // Strip trailing newline
        for (char *p = sbuf; *p; p++) if (*p == '\n') *p = 0;

        char curcam[8] = {};
        int cfd2 = open(CUR_CAM, O_RDONLY);
        if (cfd2 >= 0) { read(cfd2, curcam, sizeof(curcam) - 1); close(cfd2); }
        for (char *p = curcam; *p; p++) if (*p == '\n') *p = 0;
        int tgt = read_cam_target();
        const char *tgts = (tgt == 1) ? "front" : (tgt == 2) ? "both" : "back";
        const char *slots = (read_cam_slot() == 1) ? "idback" : "idfront";
        char fmtbuf[8] = {};
        int ffd = open(CTRL_FMT, O_RDONLY);
        if (ffd >= 0) { read(ffd, fmtbuf, sizeof(fmtbuf) - 1); close(ffd); }
        for (char *q = fmtbuf; *q; q++) if (*q == '\n') *q = 0;
        const char *fmts = strstr(fmtbuf, "12") ? "nv12" : "nv21";

        std::ostringstream oss;
        oss << "STATUS {"
            << "\"active\":"    << (g_inject_active.load() ? "true" : "false") << ","
            << "\"hook_loaded\":" << (g_hook_loaded.load() ? "true" : "false") << ","
            << "\"back_node\":\"" << g_back_node << "\","
            << "\"image\":\""   << g_image_path << "\","
            << "\"cam_target\":\"" << tgts << "\","
            << "\"cam_current\":\"" << curcam << "\","
            << "\"slot\":\""    << slots << "\","
            << "\"fmt\":\""     << fmts << "\","
            << "\"stats\":"     << sbuf
            << "}\n";
        resp = oss.str();

    } else if (cmd == "BACK_NODE") {
        resp = g_back_node + "\n";

    } else if (cmd.size() > 14 && cmd.substr(0, 13) == "SET_YUV_WIDTH") {
        // SET_YUV_WIDTH <N>  — enable YUV injection for streams ≥ N pixels wide.
        // Use 0 to disable YUV injection (front-cam liveness stays untouched).
        std::string wstr = cmd.substr(14);
        // Validate: digits only
        bool ok = !wstr.empty();
        for (char c : wstr) if (!isdigit((unsigned char)c)) { ok = false; break; }
        if (ok) {
            int fd = open(CTRL_YUV, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { write(fd, wstr.c_str(), wstr.size()); close(fd); }
        } else {
            resp = "ERROR invalid width\n";
        }

    } else if (cmd.size() > 8 && cmd.substr(0, 7) == "SET_CAM") {
        // SET_CAM back|front|both — which camera facing to inject. "back" lets the
        // front camera pass through; a volume-up double-press also toggles this.
        std::string v = cmd.substr(8);
        if (v == "back" || v == "front" || v == "both") {
            write_file(CTRL_CAM, v.c_str());
        } else {
            resp = "ERROR invalid cam\n";
        }

    } else if (cmd.size() > 9 && cmd.substr(0, 8) == "SET_SLOT") {
        // SET_SLOT idfront|idback — which prepared media slot to inject.
        // A volume-down double-press also toggles this.
        std::string v = cmd.substr(9);
        if (v == "idfront" || v == "idback") {
            write_file(CTRL_SLOT, v.c_str());
        } else {
            resp = "ERROR invalid slot\n";
        }

    } else if (cmd.size() > 8 && cmd.substr(0, 7) == "SET_FMT") {
        // SET_FMT nv21|nv12 — YUV chroma order injected into YUV streams.
        std::string v = cmd.substr(8);
        if (v == "nv21" || v == "nv12") {
            write_file(CTRL_FMT, v.c_str());
        } else {
            resp = "ERROR invalid fmt\n";
        }

    } else if (cmd.size() > 9 && cmd.substr(0, 8) == "SNAP_YUV") {
        // Option-B real-scene backdrop: `SNAP_YUV <dst_path>`.
        // Usage: `SNAP_YUV /data/user/0/com.secpen.halinject/cache/real_yuv.nv21`
        // We can't just let the hook write directly where the app wants
        // because the hook runs as cameraserver and SELinux denies it
        // `setattr` on shell_data_file (and the app's data_app_file). So
        // this handler:
        //   1. writes "1" to snap_yuv → hook dumps real_yuv.{nv21,dim}
        //   2. polls the output for up to 800 ms
        //   3. reads + copies into <dst_path> (we're root, so SELinux
        //      allows writing to the app's cache)
        //   4. chmods the copy 0644 and companion .dim so the app can read
        //   5. returns "OK <WxH>" with the frame dims
        std::string dst = cmd.substr(9);
        if (dst.empty() || dst[0] != '/') {
            resp = "ERROR SNAP_YUV requires absolute path\n";
        } else {
            write_file("/data/local/tmp/hal_inject/snap_yuv", "1");
            chmod("/data/local/tmp/hal_inject/snap_yuv", 0666);
            // Poll for the hook to finish writing + reset snap_yuv to "0".
            bool ready = false;
            for (int i = 0; i < 80; i++) {
                usleep(10 * 1000);               // 10 ms
                int fd = open("/data/local/tmp/hal_inject/snap_yuv", O_RDONLY);
                if (fd >= 0) { char b[4] = {}; read(fd, b, 1); close(fd);
                               if (b[0] == '0') { ready = true; break; } }
            }
            // Copy real_yuv.nv21 → dst; same for .dim. Reused both for fresh
            // snaps (hook cleared snap_yuv) AND the stale-fallback below.
            auto copy_file = [](const char *src, const std::string &out) -> bool {
                int s = open(src, O_RDONLY); if (s < 0) return false;
                int d = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (d < 0) { close(s); return false; }
                char buf[64 * 1024]; ssize_t n;
                while ((n = read(s, buf, sizeof(buf))) > 0) write(d, buf, n);
                close(s); close(d);
                chmod(out.c_str(), 0644);
                return true;
            };
            std::string dst_dim = dst.substr(0, dst.find_last_of('.')) + ".dim";
            bool src_exists = access("/data/local/tmp/hal_inject/real_yuv.nv21", F_OK) == 0;
            if (!ready && !src_exists) {
                // Nothing to give — no stream now AND no cached frame.
                resp = "ERROR snap timeout and no cached frame\n";
            } else if (copy_file("/data/local/tmp/hal_inject/real_yuv.nv21", dst) &&
                       copy_file("/data/local/tmp/hal_inject/real_yuv.dim",  dst_dim)) {
                // Read dims back from the dim we just copied for the response.
                int fd = open(dst_dim.c_str(), O_RDONLY);
                char db[32] = {}; if (fd >= 0) { read(fd, db, sizeof(db) - 1); close(fd); }
                // Prefix with STALE when we used the cached frame (user ran
                // Export without the camera streaming right now — common
                // path since the HAL app itself doesn't hold the camera).
                const char *prefix = ready ? "OK " : "OK STALE ";
                resp = std::string(prefix) + db + "\n";
            } else {
                resp = "ERROR copy failed\n";
            }
        }

    } else if (cmd.size() > 14 && cmd.substr(0, 13) == "SET_COMPOSITE") {
        // SET_COMPOSITE 0|1 — opt in to the Step-1 region compositor. OFF
        // by default so sessions that never call it get the legacy full-
        // overwrite (byte-identical to the pre-compositor build), which is
        // the regression baseline the roadmap asks for. The app flips this
        // to "1" right before pushing a YUV + BLOB from exportAndSend.
        std::string v = cmd.substr(14);
        if (v == "0" || v == "1") {
            write_file(CTRL_COMPOSITE, v.c_str());
        } else {
            resp = "ERROR invalid composite flag\n";
        }

    } else if (cmd.size() > 13 && cmd.substr(0, 12) == "SET_BLOB_RES") {
        // SET_BLOB_RES <w> <h>  — force the BLOB rescale target in the hook.
        // (0, 0) → AUTO: hook matches whatever dims the KYC app negotiated at
        // runtime (s->width × s->height). Any non-zero pair forces that size
        // via ctrl_blob_res_w / _h; useful when a SDK rejects "right dims,
        // wrong aspect". Writes both files atomically.
        std::string v = cmd.substr(13);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        int w = 0, h = 0;
        if (sscanf(v.c_str(), "%d %d", &w, &h) == 2 && w >= 0 && h >= 0 &&
            w <= 8192 && h <= 8192) {
            char bw[16], bh[16];
            snprintf(bw, sizeof(bw), "%d", w); snprintf(bh, sizeof(bh), "%d", h);
            write_file("/data/local/tmp/hal_inject/ctrl_blob_res_w", bw);
            write_file("/data/local/tmp/hal_inject/ctrl_blob_res_h", bh);
        } else {
            resp = "ERROR SET_BLOB_RES <w> <h> (0 0 = auto)\n";
        }

    } else if (cmd.size() > 16 && cmd.substr(0, 15) == "SET_CONTENT_PAD") {
        // SET_CONTENT_PAD <0..200> — strength of apply_content_pad in the
        // hook. 0 = off (default), ~50 = "match real paper", 100 = aggressive,
        // >100 = exaggerate (useful for PCE visualization). Writes a decimal
        // percent to /data/local/tmp/hal_inject/ctrl_content_pad.
        std::string v = cmd.substr(16);
        // Trim leading spaces, accept digits only up to 3 chars.
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        bool ok = !v.empty() && v.size() <= 3;
        for (char c : v) if (c < '0' || c > '9') { ok = false; break; }
        if (ok) {
            int n = std::atoi(v.c_str());
            if (n > 200) n = 200;
            char buf[8]; snprintf(buf, sizeof(buf), "%d", n);
            write_file("/data/local/tmp/hal_inject/ctrl_content_pad", buf);
        } else {
            resp = "ERROR SET_CONTENT_PAD <0..200>\n";
        }

    } else if (cmd.size() > 11 && cmd.substr(0, 10) == "SET_REGION") {
        // SET_REGION <cx> <cy> <w> <h>   — manual-aspect region (4 floats, 0..1 normalized to sensor)
        // SET_REGION <cx> <cy> <size>    — auto-aspect region (3 floats: size is rect-H fraction, hook derives W from source aspect)
        // Written through to /data/local/tmp/hal_inject/ctrl_region so the hook
        // picks it up on its next frame; app can drive it from the user's live
        // Place-Frame placement so each KYC's guide rect becomes the real inject
        // rect (replaces the static center region).
        std::string v = cmd.substr(11);
        float a, b, c, d;
        int n = sscanf(v.c_str(), "%f %f %f %f", &a, &b, &c, &d);
        char line[64];
        if (n == 4) {
            snprintf(line, sizeof(line), "%.4f %.4f %.4f %.4f\n", a, b, c, d);
            write_file(CTRL_REGION, line);
        } else if (n == 3) {
            snprintf(line, sizeof(line), "%.4f %.4f %.4f\n", a, b, c);
            write_file(CTRL_REGION, line);
        } else {
            resp = "ERROR region needs 3 or 4 floats\n";
        }

    } else {
        resp = "ERROR unknown command\n";
    }

    send(cfd, resp.c_str(), resp.size(), 0);
    close(cfd);
}

// ─── BPF event handler ────────────────────────────────────────────────────

static void bpf_event_handler(const CamEvent &ev) {
    if (ev.ev_type == EV_OPEN && ev.is_back_cam) {
        fprintf(stderr, "[bpf] back cam opened pid=%u fd=%d (%s)\n",
                ev.pid, ev.fd, ev.comm);
        // Re-inject on each camera open in case cameraserver restarted
        if (g_inject_active.load()) run_injector();
    } else if (ev.ev_type == EV_CLOSE && ev.is_back_cam) {
        fprintf(stderr, "[bpf] back cam closed pid=%u fd=%d\n", ev.pid, ev.fd);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────

int main(int /*argc*/, char ** /*argv*/) {
    if (getuid() != 0) {
        fprintf(stderr, "[daemon] must run as root\n");
        return 1;
    }

    mkdir("/data/local/tmp/hal_inject", 0755);
    // 777 the dir so the hook (running as cameraserver uid, not root) can
    // CREATE files in it — needed for the ctrl_dump → dump_NNN.nv21 path.
    chmod("/data/local/tmp/hal_inject", 0777);

    // Step 3: start the gyroscope producer thread (writes samples at ~200 Hz
    // to /data/local/tmp/hal_inject/imu.shm). The cameraserver hook mmaps
    // that file RO and uses it for motion-blur matching in Step 4. Starting
    // this before the single-instance guard means a probing instance briefly
    // claims the sensor; benign — it exits within a few ms if another daemon
    // is already up.
    extern void imu_start();
    imu_start();

    // Single-instance guard: if a daemon already owns the control socket, exit
    // instead of orphaning our facing/volume threads behind it (and without
    // clobbering the live daemon's control state). A stale socket file with no
    // listener fails connect(), so we fall through and rebind it below.
    {
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0) {
            struct sockaddr_un pa = {};
            pa.sun_family = AF_UNIX;
            strncpy(pa.sun_path, SOCK_PATH, sizeof(pa.sun_path) - 1);
            if (connect(probe, (struct sockaddr *)&pa, sizeof(pa)) == 0) {
                close(probe);
                fprintf(stderr, "[daemon] another instance already running — exiting\n");
                return 0;
            }
            close(probe);
        }
    }

    write_ctrl("off");
    // Default: YUV injection ON for KYC document/liveness streams (≥640px wide).
    // KYC flows (Jumio/Onfido/etc.) deliver frames on a YUV_420_888 stream, never
    // a BLOB still-capture, so YUV must be on for them to be injected at all.
    // SET_YUV_WIDTH overrides; 0 disables. The facing target (default back) still
    // gates which camera is injected, so the front camera passes through untouched.
    {
        int fd = open(CTRL_YUV, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "640", 3); close(fd); }
    }
    // Default inject target = back (front camera passes through until switched).
    write_file(CTRL_CAM, "back");
    write_file(CUR_CAM, "");
    // Default media slot = ID front side (vol-down double-press switches to idback).
    write_file(CTRL_SLOT, "idfront");
    // Default YUV chroma order = NV21.
    write_file(CTRL_FMT, "nv21");
    // Compositor OFF by default (regression guard — sessions without the
    // SET_COMPOSITE opt-in run the legacy full-overwrite, which is bit-
    // identical to the pre-compositor build). The app's exportAndSend()
    // flips it to "1" via bridge.setComposite(true) right before pushing
    // the YUV + BLOB, so interactive use stays seamless; but a test or a
    // regression check that never calls SET_COMPOSITE sees untouched hook
    // behaviour and can compare byte-exact against the baseline binary.
    write_file(CTRL_COMPOSITE, "0");

    g_back_node = primaryBackCamNode();
    fprintf(stderr, "[daemon] back camera node: %s\n", g_back_node.c_str());

    // Background: track the active camera facing, and watch the volume-up key for
    // a double-press that toggles the inject target (front <-> back).
    std::thread(cam_facing_logcat_loop).detach();
    std::thread(volume_monitor_loop).detach();

    // Load eBPF program (non-fatal; injection still works without BPF tracking)
    BpfLoader bpf(BPF_OBJ);
    bool bpf_ok = bpf.load();
    if (!bpf_ok)
        fprintf(stderr, "[daemon] eBPF load failed (continuing)\n");

    std::thread([&]() {
        if (bpf_ok) bpf.poll(bpf_event_handler);
    }).detach();

    // Unix socket server
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);
    unlink(SOCK_PATH);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    chmod(SOCK_PATH, 0666);
    listen(srv, 8);

    fprintf(stderr, "[daemon] listening on %s\n", SOCK_PATH);
    fprintf(stderr, "[daemon] injection path:\n"
                    "  in-process hook : ptrace cam_injector → libcam_hook.so\n"
                    "  frida           : DISABLED (crashes KYC SDKs)\n");

    while (true) {
        int cfd = accept(srv, nullptr, nullptr);
        if (cfd < 0) continue;
        std::thread([cfd]() { handle_client(cfd); }).detach();
    }
    return 0;
}
