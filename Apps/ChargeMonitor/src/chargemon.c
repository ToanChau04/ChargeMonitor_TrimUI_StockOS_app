/*
 * ChargeMonitor - theo dõi sạc pin & tối ưu máy khi sạc
 * cho TrimUI Brick Pro (stock OS). Vẽ thẳng lên /dev/fb0, đọc phím từ /dev/input/event*.
 *
 * Biến môi trường để thử trên PC:
 *   CM_ROOT=/thu_muc_gia   tiền tố cho mọi đường dẫn /sys, /dev
 *   CM_SNAPSHOT=out.ppm    vẽ 1 khung hình ra file PPM rồi thoát (không đụng phần cứng)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>
#include "font.h"

#define APP_VER "1.3"
#define BATT_MAH 5000.0          /* dự phòng khi driver không báo charge_full */
#define IDLE_SCREEN_OFF_S 30     /* tự tắt màn hình sau 30 s không bấm phím */
#define HOLD_EXIT_S 2.0          /* giữ phím bất kỳ 2 s để thoát */
#define HIST_PERIOD_S 30         /* lấy mẫu đồ thị + ghi log mỗi 30 s */
#define HIST_MAX 720             /* 6 giờ lịch sử */
#define EXIT_POWEROFF 10         /* mã thoát báo launch.sh tắt máy */

static char ROOT[256] = "";
static volatile sig_atomic_t g_quit = 0;

/* ---------------------------------------------------------------- sysfs */
static void P(char *out, size_t n, const char *path) { snprintf(out, n, "%s%s", ROOT, path); }

static int rd_str(const char *path, char *buf, int n) {
    char p[512]; P(p, sizeof p, path);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    if (!fgets(buf, n, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}
static long rd_long(const char *path, long def) {
    char b[64]; if (rd_str(path, b, sizeof b)) return def;
    char *e; long v = strtol(b, &e, 10); return e == b ? def : v;
}
static int wr_str(const char *path, const char *s) {
    char p[512]; P(p, sizeof p, path);
    int fd = open(p, O_WRONLY); if (fd < 0) return -1;
    int r = write(fd, s, strlen(s)); close(fd);
    return r < 0 ? -1 : 0;
}
static int wr_long(const char *path, long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); return wr_str(path, b); }
static int exists(const char *path) { char p[512]; P(p, sizeof p, path); return access(p, F_OK) == 0; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* -------------------------------------------------------------- battery */
typedef struct {
    int ok, cap, plugged, charging, full;
    double volt, cur_ma, temp, watt;
    long chg_cnt, chg_full, ttf_s, tte_s;   /* mAh, mAh, giây, giây (−1 = không có) */
    char status[32];
} Bat;

static void read_battery(Bat *b) {
    memset(b, 0, sizeof *b);
    b->cap = -1; b->temp = NAN; b->volt = NAN; b->cur_ma = NAN;
    b->chg_cnt = b->chg_full = b->ttf_s = b->tte_s = -1;
    char d[512]; P(d, sizeof d, "/sys/class/power_supply");
    DIR *dir = opendir(d); if (!dir) return;
    struct dirent *e; int ext_online = -1;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char base[300], p[400], type[32] = "";
        snprintf(base, sizeof base, "/sys/class/power_supply/%s", e->d_name);
        snprintf(p, sizeof p, "%s/type", base); rd_str(p, type, sizeof type);
        if (!strcasecmp(type, "Battery") && !b->ok) {
            b->ok = 1;
            snprintf(p, sizeof p, "%s/capacity", base);    b->cap = rd_long(p, -1);
            snprintf(p, sizeof p, "%s/status", base);      rd_str(p, b->status, sizeof b->status);
            snprintf(p, sizeof p, "%s/voltage_now", base); long v = rd_long(p, LONG_MIN);
            if (v != LONG_MIN) b->volt = v > 100000 ? v / 1e6 : (v > 1000 ? v / 1e3 : (double)v);
            /* TrimUI stock (driver AXP2202 của Allwinner): constant_charge_current trả về độ lớn dòng pin đo được
               (đã kiểm chứng: đổi theo tải, khớp sụt áp qua nội trở pin), dấu lấy theo trạng thái */
            static const char *cn[] = {"current_now", "current_avg", "batt_current", "bat_current", "current",
                                       "constant_charge_current", NULL};
            for (int k = 0; cn[k] && isnan(b->cur_ma); k++) {
                snprintf(p, sizeof p, "%s/%s", base, cn[k]); long c = rd_long(p, LONG_MIN);
                if (c != LONG_MIN) b->cur_ma = labs(c) > 20000 ? c / 1000.0 : (double)c;
            }
            snprintf(p, sizeof p, "%s/charge_counter", base);   b->chg_cnt = rd_long(p, -1);
            snprintf(p, sizeof p, "%s/charge_full", base);      b->chg_full = rd_long(p, -1);
            snprintf(p, sizeof p, "%s/time_to_full_now", base); b->ttf_s = rd_long(p, -1);
            snprintf(p, sizeof p, "%s/time_to_empty_now", base); b->tte_s = rd_long(p, -1);
            /* đổi µAh -> mAh nếu driver dùng đơn vị chuẩn */
            if (b->chg_full > 100000) { b->chg_full /= 1000; if (b->chg_cnt > 0) b->chg_cnt /= 1000; }
            snprintf(p, sizeof p, "%s/temp", base);        long t = rd_long(p, LONG_MIN);
            if (t != LONG_MIN) b->temp = labs(t) > 200 ? t / 10.0 : (double)t;
        } else if (!strcasecmp(type, "USB") || !strcasecmp(type, "Mains") || !strncasecmp(type, "USB_", 4)) {
            snprintf(p, sizeof p, "%s/online", base);
            long o = rd_long(p, -1);
            if (o >= 0) ext_online = (ext_online == 1) ? 1 : (int)o;
        }
    }
    closedir(dir);
    b->charging = !strcasecmp(b->status, "Charging");
    b->full = !strcasecmp(b->status, "Full") || (b->cap >= 100 && !b->charging);
    b->plugged = ext_online >= 0 ? ext_online : (b->charging || !strcasecmp(b->status, "Full"));
    if (b->full && !b->plugged) b->full = 0;
    /* một số driver báo dòng xả là số dương: lấy dấu theo trạng thái */
    if (!isnan(b->cur_ma)) {
        if (b->charging && b->cur_ma < 0) b->cur_ma = -b->cur_ma;
        if (!b->plugged && b->cur_ma > 0) b->cur_ma = -b->cur_ma;
    }
    if (!isnan(b->volt) && !isnan(b->cur_ma)) b->watt = b->volt * b->cur_ma / 1000.0; else b->watt = NAN;
}

static void dump_power_supply(void) {
    char d[512]; P(d, sizeof d, "/sys/class/power_supply");
    DIR *dir = opendir(d); if (!dir) { fprintf(stderr, "ps: no %s\n", d); return; }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char sd[600]; snprintf(sd, sizeof sd, "%s/%s", d, e->d_name);
        DIR *s2 = opendir(sd); if (!s2) continue;
        struct dirent *f;
        while ((f = readdir(s2))) {
            if (f->d_name[0] == '.' || !strcmp(f->d_name, "uevent")) continue;
            char fp[900], v[96] = "?"; snprintf(fp, sizeof fp, "%s/%s", sd, f->d_name);
            FILE *ff = fopen(fp, "r"); if (!ff) continue;
            if (!fgets(v, sizeof v, ff)) strcpy(v, "(err)\n"); fclose(ff);
            v[strcspn(v, "\r\n")] = 0;
            fprintf(stderr, "ps %s/%s = %s\n", e->d_name, f->d_name, v);
        }
        closedir(s2);
    }
    closedir(dir);
}

/* --------------------------------------------------------- optimizations */
typedef struct {
    int applied;
    char gov[32]; long maxf, setspeed; int gov_changed, maxf_changed, setspeed_changed;
    int cpu_was_online[8];
    char bl_path[300]; long bl_max, bl_orig; int use_disp; long disp_orig;
    long led_orig; int led_changed;
    long low_freq; int cores_off;
    long dim_level;
} Saved;
static Saved S;
static int g_fb = -1;

#define CF "/sys/devices/system/cpu/cpu0/cpufreq/"

static long disp_brightness(long set) {      /* Allwinner /dev/disp: 0x102 set, 0x103 get */
    char p[512]; P(p, sizeof p, "/dev/disp");
    int fd = open(p, O_RDWR); if (fd < 0) return -1;
    unsigned long a[4] = {0, (unsigned long)(set < 0 ? 0 : set), 0, 0};
    long r = ioctl(fd, set < 0 ? 0x103 : 0x102, a);
    close(fd); return r;
}

static void find_backlight(void) {
    char d[512]; P(d, sizeof d, "/sys/class/backlight");
    DIR *dir = opendir(d);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.') continue;
            char p[400]; snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", e->d_name);
            if (exists(p)) {
                snprintf(S.bl_path, sizeof S.bl_path, "%s", p);
                snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", e->d_name);
                S.bl_max = rd_long(p, 255);
                S.bl_orig = rd_long(S.bl_path, S.bl_max);
                break;
            }
        }
        closedir(dir);
    }
    if (!S.bl_path[0]) {
        long v = disp_brightness(-1);
        if (v >= 0) { S.use_disp = 1; S.disp_orig = v; S.bl_max = 255; }
    }
    long cur = S.use_disp ? S.disp_orig : S.bl_orig;
    S.dim_level = S.bl_max * 12 / 100; if (S.dim_level < 1) S.dim_level = 1;
    if (cur > 0 && cur < S.dim_level) S.dim_level = cur;   /* không sáng hơn mức người dùng đang để */
}

static void set_backlight(long v) {
    if (S.bl_path[0]) wr_long(S.bl_path, v);
    else if (S.use_disp) disp_brightness(v);
}

static void screen_power(int on) {
    if (g_fb >= 0) ioctl(g_fb, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
    set_backlight(on ? S.dim_level : 0);
}

static void apply_opt(void) {
    memset(&S, 0, sizeof S);
    /* CPU: hạ xung về mức thấp nhất */
    rd_str(CF "scaling_governor", S.gov, sizeof S.gov);
    S.maxf = rd_long(CF "scaling_max_freq", -1);
    S.setspeed = rd_long(CF "scaling_setspeed", -1);
    long minf = rd_long(CF "cpuinfo_min_freq", -1);
    if (minf < 0) minf = rd_long(CF "scaling_min_freq", -1);
    S.low_freq = minf;
    char avail[256] = ""; rd_str(CF "scaling_available_governors", avail, sizeof avail);
    if (minf > 0 && S.maxf > 0 && !wr_long(CF "scaling_max_freq", minf)) S.maxf_changed = 1;
    if (strstr(avail, "powersave") && strcmp(S.gov, "powersave")) {
        if (!wr_str(CF "scaling_governor", "powersave")) S.gov_changed = 1;
    } else if (!strcmp(S.gov, "userspace") && minf > 0) {
        if (!wr_long(CF "scaling_setspeed", minf)) S.setspeed_changed = 1;
    }
    /* tắt bớt nhân CPU (giữ cpu0) */
    for (int i = 1; i < 8; i++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", i);
        long o = rd_long(p, -1);
        S.cpu_was_online[i] = (int)o;
        if (o == 1 && !wr_str(p, "0")) S.cores_off++;
    }
    /* đèn LED RGB */
    S.led_orig = rd_long("/sys/class/led_anim/max_scale", -1);
    if (S.led_orig > 0 && !wr_str("/sys/class/led_anim/max_scale", "0")) S.led_changed = 1;
    /* đèn nền */
    find_backlight();
    set_backlight(S.dim_level);
    S.applied = 1;
}

static void restore_opt(void) {
    if (!S.applied) return;
    S.applied = 0;
    if (g_fb >= 0) ioctl(g_fb, FBIOBLANK, FB_BLANK_UNBLANK);
    for (int i = 1; i < 8; i++) if (S.cpu_was_online[i] == 1) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", i); wr_str(p, "1");
    }
    if (S.maxf_changed) wr_long(CF "scaling_max_freq", S.maxf);
    if (S.gov_changed) wr_str(CF "scaling_governor", S.gov);
    if (S.setspeed_changed) wr_long(CF "scaling_setspeed", S.setspeed);
    if (S.led_changed) wr_long("/sys/class/led_anim/max_scale", S.led_orig);
    if (S.bl_path[0]) wr_long(S.bl_path, S.bl_orig > 0 ? S.bl_orig : S.bl_max / 2);
    else if (S.use_disp) disp_brightness(S.disp_orig > 0 ? S.disp_orig : 128);
}

static void on_signal(int s) { (void)s; g_quit = 1; }

/* ------------------------------------------------------------- graphics */
static int W = 1024, H = 768;
static uint32_t *buf;

#define RGB(r,g,b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
static const uint32_t C_BG = RGB(14, 17, 22), C_CARD = RGB(24, 29, 37), C_LINE = RGB(44, 52, 64);
static const uint32_t C_TXT = RGB(232, 236, 241), C_DIM = RGB(138, 148, 162);
static const uint32_t C_GREEN = RGB(62, 207, 122), C_AMBER = RGB(244, 180, 64), C_RED = RGB(240, 88, 80), C_BLUE = RGB(86, 160, 250);

static inline uint32_t mix(uint32_t d, uint32_t s, int a) {
    if (a >= 255) return s;
    if (a <= 0) return d;
    int r = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
    int g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
    int b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return RGB(r, g, b);
}
static inline void px(int x, int y, uint32_t c, int a) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) buf[y * W + x] = mix(buf[y * W + x], c, a);
}
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c, 255);
}
static void rrect(int x, int y, int w, int h, int r, uint32_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int dx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (dx && dy) {
            double d = sqrt((double)dx * dx + (double)dy * dy) - r;
            int a = d <= -0.5 ? 255 : (d >= 0.5 ? 0 : (int)((0.5 - d) * 255));
            px(x + i, y + j, c, a);
        } else px(x + i, y + j, c, 255);
    }
}

static const Glyph *find_glyph(const Font *f, uint32_t cp) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (f->g[m].cp == cp) return &f->g[m]; if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1; }
    return NULL;
}
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; uint32_t c = *p;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (n == 2) c = (p[0] & 31) << 6 | (p[1] & 63);
    else if (n == 3) c = (p[0] & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
    else if (n == 4) c = (p[0] & 7) << 18 | (p[1] & 63) << 12 | (p[2] & 63) << 6 | (p[3] & 63);
    *s += n; return c;
}
static int text_w(const Font *f, const char *s) {
    int w = 0; while (*s) { const Glyph *g = find_glyph(f, utf8_next(&s)); if (!g) g = find_glyph(f, '?'); if (g) w += g->adv; } return w;
}
/* y là đường chân chữ (baseline); align: 0 trái, 1 giữa, 2 phải */
static int text(const Font *f, int x, int y, const char *s, uint32_t c, int align) {
    int w = text_w(f, s);
    if (align == 1) x -= w / 2; else if (align == 2) x -= w;
    while (*s) {
        const Glyph *g = find_glyph(f, utf8_next(&s)); if (!g) g = find_glyph(f, '?'); if (!g) continue;
        const uint8_t *a = f->data + g->off;
        for (int j = 0; j < g->h; j++) for (int i = 0; i < g->w; i++) if (a[j * g->w + i]) px(x + g->xoff + i, y + g->yoff + j, c, a[j * g->w + i]);
        x += g->adv;
    }
    return w;
}
static void line(double x0, double y0, double x1, double y1, double th, uint32_t c) {
    double minx = fmin(x0, x1) - th, maxx = fmax(x0, x1) + th, miny = fmin(y0, y1) - th, maxy = fmax(y0, y1) + th;
    double dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++) for (int x = (int)minx; x <= (int)maxx; x++) {
        double t = L2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / L2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
        double ex = x0 + t * dx - x, ey = y0 + t * dy - y, d = sqrt(ex * ex + ey * ey) - th / 2;
        if (d < 0.5) px(x, y, c, d <= -0.5 ? 255 : (int)((0.5 - d) * 255));
    }
}

/* ------------------------------------------------------------ framebuffer */
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint8_t *fbmem; static size_t fblen;

static int fb_open(void) {
    char p[512]; P(p, sizeof p, "/dev/fb0");
    g_fb = open(p, O_RDWR); if (g_fb < 0) return -1;
    if (ioctl(g_fb, FBIOGET_VSCREENINFO, &vinfo) || ioctl(g_fb, FBIOGET_FSCREENINFO, &finfo)) return -1;
    W = vinfo.xres; H = vinfo.yres;
    fblen = finfo.smem_len;
    fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb, 0);
    if (fbmem == MAP_FAILED) return -1;
    ioctl(g_fb, FBIOBLANK, FB_BLANK_UNBLANK);
    return 0;
}
static void fb_flip(void) {
    if (!fbmem) return;
    /* vẽ vào trang đang hiển thị hiện tại */
    uint8_t *base = fbmem + (size_t)vinfo.yoffset * finfo.line_length + (size_t)vinfo.xoffset * (vinfo.bits_per_pixel / 8);
    for (int y = 0; y < H; y++) {
        uint8_t *row = base + (size_t)y * finfo.line_length;
        if ((size_t)(row - fbmem) + (size_t)W * (vinfo.bits_per_pixel / 8) > fblen) break;
        uint32_t *src = buf + y * W;
        if (vinfo.bits_per_pixel == 32) {
            uint32_t *d = (uint32_t *)row;
            for (int x = 0; x < W; x++) {
                uint32_t c = src[x];
                uint32_t r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
                d[x] = (255u << (vinfo.transp.length ? vinfo.transp.offset : 24)) | r << vinfo.red.offset | g << vinfo.green.offset | b << vinfo.blue.offset;
            }
        } else if (vinfo.bits_per_pixel == 16) {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < W; x++) { uint32_t c = src[x]; d[x] = ((c >> 19 & 31) << 11) | ((c >> 10 & 63) << 5) | (c >> 3 & 31); }
        }
    }
}

/* ----------------------------------------------------------------- input */
#define MAXIN 16
static int infd[MAXIN], nin;
static void input_open(void) {
    for (int i = 0; i < 32 && nin < MAXIN; i++) {
        char p[512], q[64]; snprintf(q, sizeof q, "/dev/input/event%d", i); P(p, sizeof p, q);
        int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd >= 0) infd[nin++] = fd;
    }
}

/* ------------------------------------------------------------- history */
typedef struct { double t; int cap; double ma; long mah; } Sample;
static Sample hist[HIST_MAX]; static int nhist;
static void hist_add(double t, int cap, double ma, long mah) {
    if (nhist == HIST_MAX) { memmove(hist, hist + 1, sizeof(Sample) * (HIST_MAX - 1)); nhist--; }
    hist[nhist++] = (Sample){t, cap, ma, mah};
}
/* dòng ước tính (mA) = độ dốc của charge_counter (mAh) theo giờ, hồi quy trên 10 phút gần nhất */
static double rate_mah_h(double now) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int i = 0; i < nhist; i++) if (now - hist[i].t <= 10 * 60 && hist[i].mah >= 0) {
        double x = (hist[i].t - now) / 3600.0, y = hist[i].mah;
        sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
    }
    if (n < 6) return NAN;
    double den = n * sxx - sx * sx; if (fabs(den) < 1e-12) return NAN;
    return (n * sxy - sx * sy) / den;
}
/* tốc độ %/phút bằng hồi quy tuyến tính trên 15 phút gần nhất */
static double rate_pct_min(double now) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int i = 0; i < nhist; i++) if (now - hist[i].t <= 15 * 60 && hist[i].cap >= 0) {
        double x = (hist[i].t - now) / 60.0, y = hist[i].cap;
        sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
    }
    if (n < 6) return NAN;
    double den = n * sxx - sx * sx; if (fabs(den) < 1e-9) return NAN;
    return (n * sxy - sx * sy) / den;
}
static void fmt_dur(char *o, size_t n, double min) {
    if (isnan(min) || min < 0 || min > 7 * 24 * 60) { snprintf(o, n, "--"); return; }
    int m = (int)(min + 0.5);
    if (m >= 60) snprintf(o, n, "%d giờ %02d phút", m / 60, m % 60); else snprintf(o, n, "%d phút", m);
}

/* ------------------------------------------------------------------ UI */
typedef struct {
    Bat b; double start, now; int autooff, screen_on, last_key, poweroff_in; double rate, cur_est; char logname[128];
} UI;

static void metric_row(int x, int y, int w, const char *label, const char *val, uint32_t vc) {
    text(&FONT_S, x, y, label, C_DIM, 0);
    text(&FONT_M, x + w, y + 2, val, vc, 2);
}

static void draw(UI *u) {
    char s[128], s2[64];
    Bat *b = &u->b;
    uint32_t acc = b->full ? C_GREEN : b->charging ? C_GREEN : b->plugged ? C_AMBER : (b->cap >= 0 && b->cap <= 15 ? C_RED : C_BLUE);
    rect(0, 0, W, H, C_BG);

    /* header */
    text(&FONT_M, 32, 52, "Theo dõi sạc pin", C_TXT, 0);
    int el = (int)(u->now - u->start);
    snprintf(s, sizeof s, "Đã chạy %02d:%02d:%02d", el / 3600, el / 60 % 60, el % 60);
    text(&FONT_S, W - 32, 50, s, C_DIM, 2);
    rect(32, 74, W - 64, 1, C_LINE);

    /* thẻ trái: % pin */
    rrect(32, 94, 456, 300, 18, C_CARD);
    if (b->cap >= 0) snprintf(s, sizeof s, "%d", b->cap); else snprintf(s, sizeof s, "--");
    int tw = text(&FONT_L, 60, 262, s, C_TXT, 0);
    text(&FONT_M, 66 + tw, 262, "%", C_DIM, 0);
    snprintf(s, sizeof s, "Tự tắt máy khi đầy: %s", u->autooff ? "BẬT" : "TẮT");
    text(&FONT_S, 464, 130, s, u->autooff ? C_GREEN : C_DIM, 2);
    const char *st = !b->ok ? "KHÔNG ĐỌC ĐƯỢC PIN" : b->full ? "ĐÃ SẠC ĐẦY" : b->charging ? "ĐANG SẠC" : b->plugged ? "CẮM SẠC – CHƯA NẠP" : "KHÔNG CẮM SẠC";
    int pw = text_w(&FONT_SB, st) + 32;
    rrect(60, 284, pw, 38, 19, mix(C_CARD, acc, 60));
    text(&FONT_SB, 60 + pw / 2, 311, st, acc, 1);
    /* thanh pin */
    int bx = 60, by = 340, bw = 384, bh = 30;
    rrect(bx, by, bw, bh, 8, C_LINE);
    rrect(bx + bw + 4, by + 8, 8, bh - 16, 3, C_LINE);
    if (b->cap > 0) {
        int fw = (bw - 8) * (b->cap > 100 ? 100 : b->cap) / 100;
        if (fw > 12) rrect(bx + 4, by + 4, fw, bh - 8, 5, acc); else rect(bx + 4, by + 4, fw, bh - 8, acc);
    }

    /* thẻ phải: thông số */
    int rx = 508, rw = W - 32 - rx;
    rrect(rx, 94, rw, 300, 18, C_CARD);
    int mx = rx + 28, mw = rw - 56, my = 136;
    if (!isnan(b->volt)) snprintf(s, sizeof s, "%.3f V", b->volt); else snprintf(s, sizeof s, "--");
    metric_row(mx, my, mw, "Điện áp", s, C_TXT); my += 48;
    double cur = b->cur_ma, watt = b->watt; int est = 0;
    if (isnan(cur) && !isnan(u->cur_est)) {         /* driver không có dòng: suy ra từ bộ đếm điện lượng */
        cur = u->cur_est; est = 1;
        if (!isnan(b->volt)) watt = b->volt * cur / 1000.0;
    }
    const char *lbl = isnan(cur) ? "Dòng điện" : cur >= 0 ? (est ? "Dòng nạp (ước tính)" : "Dòng nạp") : (est ? "Dòng xả (ước tính)" : "Dòng xả");
    if (!isnan(cur)) snprintf(s, sizeof s, "%s%+.0f mA", est ? "~" : "", cur); else snprintf(s, sizeof s, "đang đo…");
    metric_row(mx, my, mw, lbl, s, isnan(cur) ? C_DIM : cur >= 0 ? C_GREEN : C_AMBER); my += 48;
    if (!isnan(watt)) snprintf(s, sizeof s, "%s%.2f W", est ? "~" : "", fabs(watt)); else snprintf(s, sizeof s, "đang đo…");
    metric_row(mx, my, mw, est ? "Công suất (ước tính)" : "Công suất", s, isnan(watt) ? C_DIM : C_TXT); my += 48;
    if (!isnan(b->temp)) snprintf(s, sizeof s, "%.1f °C", b->temp); else snprintf(s, sizeof s, "--");
    metric_row(mx, my, mw, "Nhiệt độ pin", s, !isnan(b->temp) && b->temp >= 45 ? C_RED : C_TXT); my += 48;
    if (!isnan(u->rate)) snprintf(s, sizeof s, "%+.2f %%/phút", u->rate); else snprintf(s, sizeof s, "đang đo…");
    metric_row(mx, my, mw, "Tốc độ", s, C_TXT); my += 48;
    /* ước tính */
    double eta = NAN; int approx = 0, chip = 0;
    const char *elbl = b->plugged ? "Đầy sau" : "Hết pin sau";
    if (b->charging && b->cap >= 0 && b->cap < 100) {
        if (b->ttf_s > 60 && b->ttf_s < 86400) { eta = b->ttf_s / 60.0; chip = 1; }
        else if (!isnan(u->rate) && u->rate > 0.01) eta = (100 - b->cap) / u->rate;
        else if (!isnan(cur) && cur > 50) { eta = (100 - b->cap) / 100.0 * (b->chg_full > 0 ? b->chg_full : BATT_MAH) / cur * 60.0 * 1.15; approx = 1; }
    } else if (!b->plugged) {
        if (b->tte_s > 60 && b->tte_s < 7 * 86400) { eta = b->tte_s / 60.0; chip = 1; }
        else if (!isnan(u->rate) && u->rate < -0.01) eta = b->cap / -u->rate;
    }
    if (b->full) snprintf(s, sizeof s, "Đầy");
    else { fmt_dur(s2, sizeof s2, eta); snprintf(s, sizeof s, "%s%s", approx && !isnan(eta) ? "~" : "", s2); }
    if (chip) elbl = b->plugged ? "Đầy sau (chip)" : "Hết pin sau (chip)";
    metric_row(mx, my, mw, elbl, s, b->plugged ? C_GREEN : C_TXT);

    /* đồ thị */
    int gx = 32, gy = 414, gw = W - 64, gh = 262;
    rrect(gx, gy, gw, gh, 18, C_CARD);
    text(&FONT_SB, gx + 28, gy + 38, "Lịch sử % pin", C_TXT, 0);
    double span = 60 * 60; /* hiển thị tối thiểu 1 giờ, mở rộng theo dữ liệu */
    if (nhist > 1 && hist[nhist - 1].t - hist[0].t > span) span = hist[nhist - 1].t - hist[0].t;
    snprintf(s, sizeof s, "%d phút gần nhất · mẫu mỗi %d giây", (int)(span / 60), HIST_PERIOD_S);
    text(&FONT_S, gx + gw - 28, gy + 36, s, C_DIM, 2);
    int px0 = gx + 92, py0 = gy + 62, pw2 = gw - 120, ph = gh - 92;
    for (int k = 0; k <= 4; k++) {
        int yy = py0 + ph - ph * k / 4;
        rect(px0, yy, pw2, 1, C_LINE);
        snprintf(s, sizeof s, "%d%%", k * 25); text(&FONT_S, px0 - 10, yy + 8, s, C_DIM, 2);
    }
    if (nhist >= 1) {
        double t1 = u->now, t0 = t1 - span;
        double lx = -1, ly = -1;
        for (int i = 0; i < nhist; i++) {
            if (hist[i].t < t0 || hist[i].cap < 0) continue;
            double x = px0 + (hist[i].t - t0) / span * pw2, y = py0 + ph - hist[i].cap / 100.0 * ph;
            if (lx >= 0) {
                for (int xx = (int)lx + 1; xx <= (int)x; xx++) {
                    double ty = ly + (y - ly) * (x > lx ? (xx - lx) / (x - lx) : 0);
                    for (int yy = (int)ty; yy < py0 + ph; yy++) px(xx, yy, acc, 70 - 60 * (yy - (int)ty) / (ph + 1));
                }
                line(lx, ly, x, y, 3, acc);
            }
            lx = x; ly = y;
        }
        if (lx >= 0) { rrect((int)lx - 6, (int)ly - 6, 12, 12, 6, C_TXT); }
    } else {
        text(&FONT_S, px0 + pw2 / 2, py0 + ph / 2, "Đang thu thập dữ liệu…", C_DIM, 1);
    }

    /* chân trang: trạng thái tối ưu + phím */
    char opt[200];
    snprintf(opt, sizeof opt, "Tối ưu: CPU %ld MHz · tắt %d nhân · đèn nền %ld%% · LED %s · Wi-Fi %s",
             S.low_freq > 0 ? S.low_freq / 1000 : 0, S.cores_off,
             S.bl_max > 0 ? S.dim_level * 100 / S.bl_max : 0, S.led_changed ? "tắt" : "giữ",
             getenv("CM_WIFI_OFF") ? "tắt" : "giữ nguyên");
    text(&FONT_S, 32, H - 52, opt, C_DIM, 0);
    snprintf(opt, sizeof opt, "START: tắt màn hình   SELECT: tự tắt máy   MENU / giữ phím 2s: thoát   (phím %d)", u->last_key);
    text(&FONT_S, 32, H - 18, opt, C_DIM, 0);

    if (u->poweroff_in > 0) {
        rrect(W / 2 - 300, H / 2 - 80, 600, 160, 20, C_CARD);
        text(&FONT_M, W / 2, H / 2 - 12, "Pin đã đầy – sắp tắt máy", C_GREEN, 1);
        snprintf(s, sizeof s, "Tắt sau %d giây · bấm phím bất kỳ để huỷ", u->poweroff_in);
        text(&FONT_S, W / 2, H / 2 + 34, s, C_DIM, 1);
    }
}

/* ----------------------------------------------------------------- main */
static int is_code(int c, const int *list) { for (; *list; list++) if (*list == c) return 1; return 0; }
static const int K_EXIT[] = {BTN_MODE, KEY_ESC, KEY_HOME, KEY_MENU, KEY_HOMEPAGE, 0};
static const int K_START[] = {BTN_START, KEY_ENTER, 0};
static const int K_SELECT[] = {BTN_SELECT, KEY_RIGHTCTRL, KEY_BACKSPACE, 0};

static void snapshot(const char *out) {
    UI u = {0}; u.start = 0; u.now = 3725; u.screen_on = 1; u.last_key = 315;
    read_battery(&u.b);
    S.low_freq = 408000; S.cores_off = 3; S.bl_max = 255; S.dim_level = 30; S.led_changed = 1;
    for (int i = 0; i < 120; i++) hist_add(u.now - (119 - i) * 30, 42 + i * 45 / 119, 1500, 1260 + i * 11);
    u.rate = rate_pct_min(u.now); u.cur_est = rate_mah_h(u.now);
    draw(&u);
    FILE *f = fopen(out, "wb"); fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint8_t c[3] = {buf[i] >> 16, buf[i] >> 8, buf[i]}; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *r = getenv("CM_ROOT"); if (r) snprintf(ROOT, sizeof ROOT, "%s", r);
    const char *snap = getenv("CM_SNAPSHOT");
    if (snap) { buf = calloc(W * H, 4); snapshot(snap); return 0; }

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    if (fb_open()) { fprintf(stderr, "không mở được /dev/fb0\n"); return 1; }
    buf = calloc((size_t)W * H, 4);
    fprintf(stderr, "fb %dx%d %dbpp line=%d yoff=%d\n", W, H, vinfo.bits_per_pixel, finfo.line_length, vinfo.yoffset);
    input_open();
    dump_power_supply();
    apply_opt();
    fprintf(stderr, "opt: gov=%s low=%ld cores_off=%d bl=%s disp=%d dim=%ld/%ld led=%d\n",
            S.gov, S.low_freq, S.cores_off, S.bl_path, S.use_disp, S.dim_level, S.bl_max, S.led_changed);

    mkdir("logs", 0755);
    UI u = {0}; u.start = u.now = now_s(); u.screen_on = 1; u.rate = NAN; u.cur_est = NAN;
    time_t wall = time(NULL); struct tm tm; localtime_r(&wall, &tm);
    strftime(u.logname, sizeof u.logname, "logs/charge_%Y%m%d_%H%M%S.csv", &tm);
    FILE *log = fopen(u.logname, "w");
    if (log) { fprintf(log, "elapsed_s,capacity_pct,status,voltage_v,current_ma,est_current_ma,power_w,temp_c,plugged,charge_counter_mah,charge_full_mah,time_to_full_s,time_to_empty_s\n"); fflush(log); }

    double last_key_t = u.now, last_read = -10, last_hist = -1e9, full_since = -1, poweroff_at = -1;
    int down_code = -1; double down_t = 0; int exit_code = 0;

    while (!g_quit) {
        u.now = now_s();
        if (u.now - last_read >= 2) {
            read_battery(&u.b); last_read = u.now;
            if (u.now - last_hist >= HIST_PERIOD_S) {
                last_hist = u.now;
                hist_add(u.now, u.b.cap, u.b.cur_ma, u.b.chg_cnt);
                u.rate = rate_pct_min(u.now);
                u.cur_est = rate_mah_h(u.now);
                if (isnan(u.cur_est) && !isnan(u.rate))   /* không có charge_counter: suy từ %/phút */
                    u.cur_est = u.rate / 100.0 * (u.b.chg_full > 0 ? u.b.chg_full : BATT_MAH) * 60.0;
                if (log) {
                    double cw = !isnan(u.b.cur_ma) ? u.b.cur_ma : u.cur_est;
                    fprintf(log, "%.0f,%d,%s,%.3f,%.0f,%.0f,%.2f,%.1f,%d,%ld,%ld,%ld,%ld\n", u.now - u.start, u.b.cap, u.b.status,
                            u.b.volt, u.b.cur_ma, u.cur_est, isnan(cw) || isnan(u.b.volt) ? NAN : u.b.volt * cw / 1000.0,
                            u.b.temp, u.b.plugged, u.b.chg_cnt, u.b.chg_full, u.b.ttf_s, u.b.tte_s);
                    fflush(log);
                }
            }
            /* tự tắt máy khi pin đầy liên tục 2 phút */
            if (u.autooff && u.b.plugged && (u.b.full || u.b.cap >= 100)) {
                if (full_since < 0) full_since = u.now;
                if (u.now - full_since >= 120 && poweroff_at < 0) {
                    poweroff_at = u.now + 15;
                    if (!u.screen_on) { screen_power(1); u.screen_on = 1; }
                    last_key_t = u.now;
                }
            } else full_since = -1;
        }
        if (poweroff_at > 0) {
            u.poweroff_in = (int)ceil(poweroff_at - u.now);
            if (u.poweroff_in <= 0) { exit_code = EXIT_POWEROFF; break; }
        } else u.poweroff_in = 0;

        /* tự tắt màn hình khi rảnh */
        if (u.screen_on && poweroff_at < 0 && u.now - last_key_t >= IDLE_SCREEN_OFF_S) { screen_power(0); u.screen_on = 0; }

        if (u.screen_on) { draw(&u); fb_flip(); }

        /* đọc phím */
        struct pollfd pf[MAXIN];
        for (int i = 0; i < nin; i++) pf[i] = (struct pollfd){infd[i], POLLIN, 0};
        int timeout = u.screen_on ? 250 : 1000;
        if (down_code >= 0) timeout = 100;
        if (nin) poll(pf, nin, timeout); else usleep(timeout * 1000);
        for (int i = 0; i < nin; i++) {
            struct input_event ev;
            while (read(infd[i], &ev, sizeof ev) == sizeof ev) {
                if (ev.type != EV_KEY) continue;
                double t = now_s();
                if (ev.value == 1) {
                    u.last_key = ev.code; down_code = ev.code; down_t = t;
                    fprintf(stderr, "key %d\n", ev.code);
                    last_key_t = t;
                    if (poweroff_at > 0) { poweroff_at = -1; u.autooff = 0; full_since = -1; continue; }
                    if (!u.screen_on) { screen_power(1); u.screen_on = 1; down_code = -1; continue; }
                    if (is_code(ev.code, K_EXIT)) g_quit = 1;
                    else if (is_code(ev.code, K_START)) { screen_power(0); u.screen_on = 0; down_code = -1; }
                    else if (is_code(ev.code, K_SELECT)) u.autooff = !u.autooff;
                } else if (ev.value == 0 && ev.code == down_code) down_code = -1;
            }
        }
        if (down_code >= 0 && now_s() - down_t >= HOLD_EXIT_S) g_quit = 1;
    }

    if (log) fclose(log);
    if (!u.screen_on) screen_power(1);
    rect(0, 0, W, H, C_BG);
    text(&FONT_M, W / 2, H / 2, exit_code == EXIT_POWEROFF ? "Đang tắt máy…" : "Đang khôi phục cài đặt…", C_TXT, 1);
    fb_flip();
    restore_opt();
    return exit_code;
}
