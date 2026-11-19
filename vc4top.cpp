/*
 * vc4top - realtime TUI for the Broadcom VideoCore IV (V3D/QPU) block.
 *
 * Based on the original vc4top by Jonas Pfeil and the terminal fork by
 * FASTSHIFT. This version keeps the terminal-only, zero-runtime-dependency
 * design but widens the telemetry to the whole V3D block: all 30 performance
 * counter sources, the full documented register map, QPU scheduler/VPM state,
 * clocks/voltages/thermal/throttle, CMA/DRM/display state and a live TUI.
 *
 * Register access: as root, vc4top maps /dev/mem exactly as before.  When run
 * unprivileged it reads the same V3D registers through /sys/kernel/vc4mon/
 * (the out-of-tree vc4mon module) and samples the performance counters with
 * the two narrow write attributes perf_enable and perf_clear.  It never opens
 * /dev/mem unprivileged and never writes any other register.
 *
 * The V3D block must be powered: the vc4cl-v3d-power service (or writing "on"
 * to 3fc00000.v3d/power/control) keeps it alive.
 *
 * License: GPL-3.0 (see LICENSE.txt).
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "vc4_defines.h"

namespace {

constexpr size_t MAP_SIZE = 0x1000;
constexpr uint32_t POWEROFF_VALUE = 0xdeadbeef;
constexpr int DEFAULT_INTERVAL_MS = 500;

// Register source. Root keeps the historical /dev/mem mapping; unprivileged
// runs use the vc4mon sysfs snapshot plus its two counter write attributes.
enum RegSource { REGS_NONE, REGS_DEVMEM, REGS_SYSFS };
constexpr const char* VC4MON_DIR = "/sys/kernel/vc4mon";

// PCTRE bit 31 is marked reserved but is required on this V3D.  Bits 0..15
// enable the 16 counter latches; no other bit is accepted by vc4mon.
constexpr uint32_t PCTRE_MASK = 0x80000000u | 0xffffu;
constexpr unsigned COUNTERS = 16;

volatile uint32_t* g_base = nullptr;
RegSource g_reg_source = REGS_NONE;
std::map<uint32_t, uint32_t> g_sysfs_regs;
std::string g_counter_error;
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_winch = 0;

// Set by the live TUI: makes counter sampling interruptible by keys/signals.
bool g_interactive = false;
int g_pending_key = -1;

bool g_color = true;

const char* K_RESET = "\033[0m";
const char* K_BOLD = "\033[1m";
const char* K_DIM = "\033[2m";
const char* K_RED = "\033[31m";
const char* K_GREEN = "\033[32m";
const char* K_YELLOW = "\033[33m";
const char* K_CYAN = "\033[36m";
const char* K_MAGENTA = "\033[35m";
const char* K_HEAD = "\033[1;36m";

struct SysData {
    std::string time, temp, throttle, throttle_decoded;
    std::string volts_core, volts_sdram;
    std::string arm_clk, core_clk, v3d_clk, h264_clk, isp_clk;
    std::string pixel_clk, hdmi_clk, vec_clk, emmc_clk;
    std::string gpu_mem, arm_mem, cma;
    // Numeric memory pools (kB) used for the live bars and the --once
    // percentages. Filled from /proc/meminfo in collect_sys_local().
    long mem_total_kb = -1, mem_avail_kb = -1;
    long cma_total_kb = -1, cma_free_kb = -1;
    std::string v3d_state, v3d_control, undervolt;
    std::string hvs_underrun, drm, hdmi_status, hdmi_mode, bo_stats, clients;
};

struct QpuData {
    uint32_t ident0 = 0, ident1 = 0, ident2 = 0;
    uint32_t srqcs = 0, vpmbase = 0, sqrsv0 = 0, sqrsv1 = 0, sqcntl = 0;
    uint32_t srqpc = 0, srqua = 0, srqul = 0;
    uint32_t intctl = 0, intena = 0, intdis = 0;
    uint32_t pctre = 0, pctrc = 0, l2cactl = 0, slcactl = 0, errstat = 0, vpacntl = 0;
    int revision = 0, slices = 0, tmus = 0, qpus = 0, semaphores = 0;
    int tmus_per_slice = 0, qpus_per_slice = 0, vpm_kb = 0, hdr = 0;
    bool powered = false;
};

struct CounterValue {
    int source = 0;
    uint32_t value = 0;
    double rate = 0.0;
    bool powered_off = false;
};

std::string cmd(const std::string& c)
{
    std::string out;
    FILE* f = popen((c + " 2>/dev/null").c_str(), "r");
    if (!f) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string after_eq(const std::string& s)
{
    auto p = s.find('=');
    return p == std::string::npos ? s : s.substr(p + 1);
}

std::string read_file(const std::string& path)
{
    std::string out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    fclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

size_t utf8_char_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t ansi_seq_len(const std::string& s, size_t i)
{
    // s[i] is ESC. Skip CSI sequences ("ESC [ ... letter").
    size_t j = i + 1;
    if (j < s.size() && s[j] == '[')
    {
        ++j;
        while (j < s.size() && !std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
        if (j < s.size()) ++j;
    }
    return j - i;
}

size_t visible_len(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == '\033')
        {
            i += ansi_seq_len(s, i);
            continue;
        }
        i += utf8_char_len(static_cast<unsigned char>(s[i]));
        ++n;
    }
    return n;
}

std::string pad_visible(const std::string& s, size_t width)
{
    const size_t n = visible_len(s);
    if (n >= width) return s;
    return s + std::string(width - n, ' ');
}

std::string truncate_plain(const std::string& s, size_t width)
{
    if (s.size() <= width) return s;
    if (width == 0) return "";
    return s.substr(0, width);
}

std::string truncate_visible(const std::string& s, size_t width)
{
    std::string out;
    size_t vis = 0;
    for (size_t i = 0; i < s.size() && vis < width;)
    {
        if (s[i] == '\033')
        {
            const size_t l = ansi_seq_len(s, i);
            out.append(s, i, l);
            i += l;
            continue;
        }
        const size_t l = utf8_char_len(static_cast<unsigned char>(s[i]));
        if (vis + 1 > width) break;
        out.append(s, i, l);
        i += l;
        ++vis;
    }
    if (g_color) out += K_RESET;
    return out;
}

std::string clip_vis(const std::string& s, size_t width)
{
    if (visible_len(s) <= width) return s;
    if (width == 0) return "";
    std::string out = truncate_visible(s, width - 1);
    out += ">";
    return out;
}

std::string colorize(const char* color, const std::string& s)
{
    if (!g_color) return s;
    return std::string(color) + s + K_RESET;
}

std::string hex32(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%08x", v);
    return b;
}

std::string mhz_from_hz(const std::string& hz)
{
    if (hz.empty()) return "n/a";
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(hz.c_str(), &end, 10);
    if (end == hz.c_str() || *end != '\0' || errno != 0) return hz;
    char b[64];
    snprintf(b, sizeof(b), "%.3f MHz", static_cast<double>(v) / 1000000.0);
    return b;
}

std::string clock_with_units(const std::string& hz)
{
    if (hz.empty()) return "n/a";
    return hz + " Hz (" + mhz_from_hz(hz) + ")";
}

std::string norm_temp(const std::string& t)
{
    if (t.empty()) return "n/a";
    std::string s = t;
    auto p = s.find("'C");
    if (p != std::string::npos) s.replace(p, 2, " \xC2\xB0\x43"); // " °C"
    return s;
}

std::string mem_with_units(const std::string& m)
{
    if (m.empty()) return "n/a";
    if (m.back() == 'M') return m.substr(0, m.size() - 1) + " MiB";
    if (m.back() == 'G') return m.substr(0, m.size() - 1) + " GiB";
    return m;
}

std::string cma_units(const std::string& c)
{
    if (c.empty()) return "n/a";
    unsigned long total = 0, freeb = 0;
    if (sscanf(c.c_str(), "CMA %luM/%luM free", &total, &freeb) == 2)
    {
        char b[80];
        snprintf(b, sizeof(b), "CMA %lu MiB / %lu MiB free", total, freeb);
        return b;
    }
    return c;
}

struct MemStat {
    bool valid = false;
    long used_mib = 0;
    long total_mib = 0;
    double pct = 0.0;
};

// Percentage is derived from the same rounded MiB numbers that are printed,
// so "218/905 MiB" always agrees with "24.1%". `free_kb` is MemAvailable for
// ARM and CmaFree for the GPU pool; used MiB is total MiB minus free MiB so the
// raw memory line and the bar always add up to the same total.
MemStat mem_stat(long total_kb, long free_kb)
{
    MemStat m;
    if (total_kb <= 0 || free_kb < 0) return m;
    m.total_mib = (total_kb + 512) / 1024;
    long free_mib = (free_kb + 512) / 1024;
    if (free_mib > m.total_mib) free_mib = m.total_mib;
    m.used_mib = m.total_mib - free_mib;
    if (m.total_mib <= 0) return m;
    m.pct = 100.0 * static_cast<double>(m.used_mib) / static_cast<double>(m.total_mib);
    m.valid = true;
    return m;
}

// ARM memory: MemTotal minus MemAvailable from /proc/meminfo.
MemStat arm_mem_stat(const SysData& s)
{
    if (s.mem_total_kb <= 0 || s.mem_avail_kb < 0) return MemStat();
    return mem_stat(s.mem_total_kb, s.mem_avail_kb);
}

// GPU pool: the CMA region carved out for the firmware/GPU; used = CmaTotal -
// CmaFree. The firmware split itself (vcgencmd get_mem gpu) stays visible as a
// raw value next to it.
MemStat gpu_cma_stat(const SysData& s)
{
    if (s.cma_total_kb <= 0 || s.cma_free_kb < 0) return MemStat();
    return mem_stat(s.cma_total_kb, s.cma_free_kb);
}

const char* mem_color(double pct)
{
    if (pct >= 90.0) return K_RED;
    if (pct >= 75.0) return K_YELLOW;
    return K_GREEN;
}

// width = cells available for this line (the caller adds its 9-column indent
// separately). The used/total MiB tail is kept whenever it fits; on very
// narrow columns it degrades to "M" and then to the percentage only, so the
// bar always stays inside the column instead of being clipped.
std::string mem_bar_line(const std::string& label, const MemStat& m, int width)
{
    std::string lab = colorize(K_CYAN, pad_visible(label, 7));
    if (!m.valid) return lab + " n/a";
    char full[96], compact[96], pct_only[32];
    snprintf(full, sizeof(full), " %5.1f%%  %ld/%ld MiB", m.pct, m.used_mib, m.total_mib);
    snprintf(compact, sizeof(compact), " %5.1f%%  %ld/%ldM", m.pct, m.used_mib, m.total_mib);
    snprintf(pct_only, sizeof(pct_only), " %5.1f%%", m.pct);
    const int base = 7 + 1 + 2; // label, space, brackets
    const char* tail = full;
    if (base + static_cast<int>(strlen(full)) + 6 > width) tail = compact;
    if (base + static_cast<int>(strlen(compact)) + 6 > width) tail = pct_only;
    const int barw = std::max(6, std::min(24, width - base - static_cast<int>(strlen(tail))));
    const double p = std::max(0.0, std::min(100.0, m.pct));
    const int filled = static_cast<int>(p / 100.0 * barw + 0.5);
    std::string bar = "[";
    bar += std::string(filled, '#');
    bar += std::string(std::max(0, barw - filled), '.');
    bar += "]";
    return lab + " " + colorize(mem_color(m.pct), bar) + tail;
}

const char* temp_color(const std::string& t)
{
    const double v = strtod(t.c_str(), nullptr);
    if (v >= 80.0) return K_RED;
    if (v >= 70.0) return K_YELLOW;
    return K_GREEN;
}

const char* throttle_color(uint32_t v)
{
    if (v & 0x000Fu) return K_RED;    // currently throttled / undervolt / temp
    if (v & 0xFFFF0000u) return K_YELLOW; // sticky past events
    return K_GREEN;
}

// Load the sysfs register snapshot into the cache. The file is a fixed-format
// list of "<offset> <value>" lines, one per documented register.
bool refresh_sysfs_regs()
{
    FILE* f = fopen((std::string(VC4MON_DIR) + "/regs").c_str(), "r");
    if (!f) return false;
    std::map<uint32_t, uint32_t> regs;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        unsigned off = 0, val = 0;
        if (sscanf(line, "%x %x", &off, &val) == 2) regs[off] = val;
    }
    fclose(f);
    if (regs.empty()) return false;
    g_sysfs_regs.swap(regs);
    return true;
}

// Called once before each register snapshot so every field in a frame comes
// from one pass over the sysfs interface.
void refresh_regs()
{
    if (g_reg_source == REGS_SYSFS) refresh_sysfs_regs();
}

void map_regs()
{
    // Root keeps the historical /dev/mem mapping.  Unprivileged runs, and a
    // root fallback when /dev/mem is unavailable, use vc4mon.
    if (geteuid() == 0)
    {
        int fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd >= 0)
        {
            void* p = mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, VC4_BASE);
            close(fd);
            if (p != MAP_FAILED)
            {
                g_base = static_cast<volatile uint32_t*>(p);
                g_reg_source = REGS_DEVMEM;
                return;
            }
            fprintf(stderr, "vc4top: mmap V3D failed: %s; trying %s\n",
                strerror(errno), VC4MON_DIR);
        }
        else
        {
            fprintf(stderr, "vc4top: open /dev/mem failed: %s; trying %s\n",
                strerror(errno), VC4MON_DIR);
        }
    }

    // Unprivileged (and root fallback): sysfs view.  Never opens /dev/mem;
    // the only writes go to vc4mon's perf_enable/perf_clear attributes.
    if (access((std::string(VC4MON_DIR) + "/regs").c_str(), R_OK) == 0)
    {
        g_reg_source = REGS_SYSFS;
        if (refresh_sysfs_regs()) return;
        fprintf(stderr, "vc4top: cannot read %s/regs\n", VC4MON_DIR);
        exit(1);
    }

    fprintf(stderr, "vc4top: no access to the V3D registers: %s/regs is missing\n"
        "  (load the vc4mon module: sudo modprobe vc4mon, or install vc4top-dkms)\n"
        "  and /dev/mem is only available to root\n", VC4MON_DIR);
    exit(1);
}

inline uint32_t rd(uint32_t off)
{
    if (g_base) return g_base[off / 4];
    auto it = g_sysfs_regs.find(off);
    return it == g_sysfs_regs.end() ? 0 : it->second;
}

// The raw sysfs path has no general-purpose store: only vc4mon's two counter
// attributes are writable, through sysfs_counter_write().
inline void wr(uint32_t off, uint32_t val)
{
    if (g_base) g_base[off / 4] = val;
}

bool counters_available()
{
    if (g_reg_source == REGS_DEVMEM) return true;
    if (g_reg_source != REGS_SYSFS) return false;
    const std::string enable = std::string(VC4MON_DIR) + "/perf_enable";
    const std::string clear = std::string(VC4MON_DIR) + "/perf_clear";
    return access(enable.c_str(), W_OK) == 0 && access(clear.c_str(), W_OK) == 0;
}

bool v3d_powered() { return rd(V3D_IDENT0) != POWEROFF_VALUE; }

uint64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
        static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

std::string now_string()
{
    char buf[64];
    std::time_t t = std::time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tmv);
    return buf;
}

struct MemKb {
    long total = -1, avail = -1, cma_total = -1, cma_free = -1;
};

// One read of /proc/meminfo for every memory field the view needs. The live
// view calls this once per frame, so a single open/read replaces the previous
// two (and later four) per-key reads.
MemKb read_meminfo()
{
    MemKb m;
    const std::string all = read_file("/proc/meminfo");
    size_t pos = 0;
    while (pos < all.size())
    {
        size_t eol = all.find('\n', pos);
        if (eol == std::string::npos) eol = all.size();
        char key[32] = {0};
        long val = 0;
        if (sscanf(all.c_str() + pos, "%31[^:]: %ld", key, &val) == 2)
        {
            if (!strcmp(key, "MemTotal")) m.total = val;
            else if (!strcmp(key, "MemAvailable")) m.avail = val;
            else if (!strcmp(key, "CmaTotal")) m.cma_total = val;
            else if (!strcmp(key, "CmaFree")) m.cma_free = val;
        }
        pos = eol + 1;
    }
    return m;
}

std::string vcgen(const char* what)
{
    return after_eq(cmd(std::string("vcgencmd ") + what));
}

void decode_throttle(SysData& d)
{
    const uint32_t v = static_cast<uint32_t>(strtoul(d.throttle.c_str(), nullptr, 16));
    const struct { int bit; const char* name; } bits[] = {
        {0, "under-voltage"}, {1, "freq-cap"}, {2, "throttled"}, {3, "temp-limit"},
        {16, "uv-past"}, {17, "freq-cap-past"}, {18, "thr-past"}, {19, "temp-past"}};
    std::string s;
    for (const auto& b : bits)
        if (v & (1u << b.bit)) { if (!s.empty()) s += ","; s += b.name; }
    d.throttle_decoded = s.empty() ? "none" : s;
}

// Values that only change slowly (mailbox clocks/voltages, firmware memory
// split). Each slot costs one vcgencmd fork, so the live view refreshes them
// round-robin instead of re-reading all of them on every frame.
constexpr size_t SYS_SLOT_COUNT = 11;

void refresh_sys_slot(SysData& d, size_t slot)
{
    switch (slot)
    {
    case 0: d.volts_core = vcgen("measure_volts core"); break;
    case 1: d.volts_sdram = vcgen("measure_volts sdram_c"); break;
    case 2: d.arm_clk = vcgen("measure_clock arm"); break;
    case 3: d.core_clk = vcgen("measure_clock core"); break;
    case 4: d.v3d_clk = vcgen("measure_clock v3d"); break;
    case 5: d.h264_clk = vcgen("measure_clock h264"); break;
    case 6: d.isp_clk = vcgen("measure_clock isp"); break;
    case 7: d.pixel_clk = vcgen("measure_clock pixel"); break;
    case 8: d.hdmi_clk = vcgen("measure_clock hdmi"); break;
    case 9: d.vec_clk = vcgen("measure_clock vec"); break;
    default: d.emmc_clk = vcgen("measure_clock emmc"); break;
    }
}

// Fork-free part of the system snapshot: /proc and sysfs reads only.
void collect_sys_local(SysData& d)
{
    d.time = now_string();
    {
        const MemKb mi = read_meminfo();
        d.mem_total_kb = mi.total;
        d.mem_avail_kb = mi.avail;
        d.cma_total_kb = mi.cma_total;
        d.cma_free_kb = mi.cma_free;
        if (mi.cma_total < 0 || mi.cma_free < 0)
            d.cma = "n/a";
        else
            d.cma = "CMA " + std::to_string((mi.cma_total + 512) / 1024) + "M/" +
                std::to_string((mi.cma_free + 512) / 1024) + "M free";
    }
    d.v3d_state = read_file("/sys/bus/platform/devices/3fc00000.v3d/power/runtime_status");
    d.v3d_control = read_file("/sys/bus/platform/devices/3fc00000.v3d/power/control");
    d.undervolt = read_file("/sys/class/hwmon/hwmon1/in0_lcrit_alarm");
    d.hvs_underrun = read_file("/sys/kernel/debug/dri/0/hvs_underrun");
    d.drm = read_file("/sys/kernel/debug/dri/0/name");
    d.hdmi_status = read_file("/sys/class/drm/card0-HDMI-A-1/status");
    d.hdmi_mode = read_file("/sys/class/drm/card0-HDMI-A-1/modes");
    auto nl = d.hdmi_mode.find('\n');
    if (nl != std::string::npos) d.hdmi_mode = d.hdmi_mode.substr(0, nl);
    d.bo_stats = read_file("/sys/kernel/debug/dri/0/bo_stats");
    {
        std::string raw = read_file("/sys/kernel/debug/dri/0/clients");
        int lines = 0;
        for (char c : raw) if (c == '\n') ++lines;
        if (!raw.empty() && raw.back() != '\n') ++lines;
        d.clients = std::to_string(std::max(0, lines - 1)) + " client(s)";
    }
}

// Complete snapshot for --once/--frame and the first live frame.
void collect_sys(SysData& d)
{
    collect_sys_local(d);
    d.temp = vcgen("measure_temp");
    d.throttle = vcgen("get_throttled");
    decode_throttle(d);
    for (size_t i = 0; i < SYS_SLOT_COUNT; ++i) refresh_sys_slot(d, i);
    d.gpu_mem = vcgen("get_mem gpu");
    d.arm_mem = vcgen("get_mem arm");
}

struct SysSampler {
    SysData d;
    size_t slot = 0;
    uint64_t last_scalar = 0;
    uint64_t last_mem = 0;
    bool primed = false;
};

// Live sampler: cheap data every frame, thermal/throttle every 500 ms, and one
// clock/voltage slot per frame. That keeps a frame far below the sample
// interval while still refreshing every field within a few seconds.
void sample_sys(SysSampler& s)
{
    SysData& d = s.d;
    const uint64_t now = mono_ms();
    collect_sys_local(d);
    if (!s.primed || now - s.last_scalar >= 500)
    {
        d.temp = vcgen("measure_temp");
        d.throttle = vcgen("get_throttled");
        decode_throttle(d);
        s.last_scalar = now;
    }
    if (!s.primed)
    {
        for (size_t i = 0; i < SYS_SLOT_COUNT; ++i) refresh_sys_slot(d, i);
        d.gpu_mem = vcgen("get_mem gpu");
        d.arm_mem = vcgen("get_mem arm");
        s.primed = true;
        s.last_mem = now;
        return;
    }
    refresh_sys_slot(d, s.slot);
    s.slot = (s.slot + 1) % SYS_SLOT_COUNT;
    if (now - s.last_mem >= 15000)
    {
        d.gpu_mem = vcgen("get_mem gpu");
        d.arm_mem = vcgen("get_mem arm");
        s.last_mem = now;
    }
}

void collect_qpu(QpuData& q)
{
    refresh_regs();
    q.ident0 = rd(V3D_IDENT0);
    q.ident1 = rd(V3D_IDENT1);
    q.ident2 = rd(V3D_IDENT2);
    q.srqcs = rd(V3D_SRQCS);
    q.vpmbase = rd(V3D_VPMBASE);
    q.sqrsv0 = rd(V3D_SQRSV0);
    q.sqrsv1 = rd(V3D_SQRSV1);
    q.sqcntl = rd(V3D_SQCNTL);
    q.srqpc = rd(V3D_SRQPC);
    q.srqua = rd(V3D_SRQUA);
    q.srqul = rd(V3D_SRQUL);
    q.intctl = rd(V3D_INTCTL);
    q.intena = rd(V3D_INTENA);
    q.intdis = rd(V3D_INTDIS);
    q.pctre = rd(V3D_PCTRE);
    q.pctrc = rd(V3D_PCTRC);
    q.l2cactl = rd(V3D_L2CACTL);
    q.slcactl = rd(V3D_SLCACTL);
    q.errstat = rd(V3D_ERRSTAT);
    q.vpacntl = rd(V3D_VPACNTL);
    q.powered = q.ident0 != POWEROFF_VALUE;
    if (q.powered)
    {
        // V3D_IDENT1 (VideoCoreIV-AG100-R, Table 80):
        // 31:28 VPMSZ (1K units, 0 => 16K), 27:24 HDRT, 23:16 NSEM,
        // 15:12 TUPS (TMUs/slice), 11:8 QUPS (QPUs/slice), 7:4 NSLC, 3:0 REV.
        q.revision = q.ident1 & 0xF;
        q.slices = (q.ident1 >> 4) & 0xF;
        q.qpus_per_slice = (q.ident1 >> 8) & 0xF;
        q.tmus_per_slice = (q.ident1 >> 12) & 0xF;
        q.semaphores = (q.ident1 >> 16) & 0xFF;
        q.hdr = (q.ident1 >> 24) & 0xF;
        q.vpm_kb = (q.ident1 >> 28) & 0xF;
        if (q.vpm_kb == 0) q.vpm_kb = 16;
        q.qpus = q.slices * q.qpus_per_slice;
        q.tmus = q.slices * q.tmus_per_slice;
    }
}

std::string errstat_string(uint32_t v)
{
    static const char* names[16] = {
        "VPAEABB", "VPAERGS", "VPAEBRGL", "VPAERRGL",
        "VPMEWR", "VPMERR", "VPMERNA", "VPMEWNA",
        "VPMEFNA", "VPMEAS", "VDWE", "VCDE",
        "VCD-idle", "VCMRE", "VCMBE", "L2CARE"};
    std::string s;
    for (int i = 0; i < 16; ++i)
        if (v & (1u << i))
        {
            if (!s.empty()) s += ",";
            s += names[i];
        }
    return s.empty() ? "none" : s;
}

bool errstat_is_clean(uint32_t v) { return (v & ~0x1000u) == 0; }

std::vector<int> page_sources(int page)
{
    if (page == 1)
    {
        std::vector<int> s;
        for (int i = 0; i < 16; ++i) s.push_back(i);
        return s;
    }
    if (page == 2)
    {
        std::vector<int> s;
        for (int i = 16; i < 30; ++i) s.push_back(i);
        return s;
    }
    return {13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
}

// Wait for the counter measurement window. In the live TUI the wait is
// interruptible: keys are read as they arrive (q quits immediately) and any
// other key is queued for the next frame, so the view does not add a sleep on
// top of the sampling interval.
void wait_interval(int ms)
{
    if (!g_interactive)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (g_running)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const long remain = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        struct timeval tv;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;
        const int r = select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;
        if (c == 'q' || c == 3) { g_running = 0; return; }
        if (g_pending_key < 0) g_pending_key = c;
    }
}

// vc4mon sysfs writes are the only writes the unprivileged path performs.
// perf_enable takes a PCTRE value, perf_clear takes the literal 1.
bool sysfs_counter_write(const char* attr, uint32_t value)
{
    char text[32];
    const int len = snprintf(text, sizeof(text), "%u\n", value);
    const std::string path = std::string(VC4MON_DIR) + "/" + attr;
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        g_counter_error = path + ": " + strerror(errno);
        return false;
    }
    const ssize_t n = write(fd, text, static_cast<size_t>(len));
    const int saved = errno;
    close(fd);
    if (n != len)
    {
        g_counter_error = path + ": " + strerror(saved);
        return false;
    }
    g_counter_error.clear();
    return true;
}

struct SysfsPerf {
    uint32_t pctr[COUNTERS] = {};
    uint32_t pctrs[COUNTERS] = {};
};

bool read_sysfs_perf(SysfsPerf& p)
{
    FILE* f = fopen((std::string(VC4MON_DIR) + "/perf").c_str(), "r");
    if (!f)
    {
        g_counter_error = std::string(VC4MON_DIR) + "/perf: " + strerror(errno);
        return false;
    }
    char line[128];
    bool any = false;
    while (fgets(line, sizeof(line), f))
    {
        unsigned idx = 0, val = 0;
        if (sscanf(line, "PCTRS%u 0x%x", &idx, &val) == 2)
        {
            if (idx < COUNTERS) { p.pctrs[idx] = val; any = true; }
        }
        else if (sscanf(line, "PCTR%u 0x%x", &idx, &val) == 2)
        {
            if (idx < COUNTERS) { p.pctr[idx] = val; any = true; }
        }
    }
    fclose(f);
    return any;
}

// Unprivileged sampling.  Source selection is deliberately not exposed by
// vc4mon; the 16 counters keep the mapping configured in the module (the
// documented 13..28 primary map on a fresh load, or whatever a previous root
// owner selected).  PCTRS is read back so every row is labelled with the
// source the hardware is actually counting.
std::vector<CounterValue> sample_counters_sysfs(int interval_ms)
{
    SysfsPerf perf;

    // Release any stale lease from a previous frame, then clear and read the
    // current source map.  Both stores are serialised by vc4mon.
    if (!sysfs_counter_write("perf_enable", 0)) return {};
    if (!sysfs_counter_write("perf_clear", 1)) return {};
    if (!read_sysfs_perf(perf)) return {};

    // Enable all 16 counters.  Bit 31 is the hardware's required enable bit.
    const uint32_t mask = PCTRE_MASK;
    if (!sysfs_counter_write("perf_enable", mask)) return {};

    wait_interval(interval_ms);

    SysfsPerf post;
    const bool sampled = read_sysfs_perf(post);
    // Always give the counters back, even when the read failed.
    sysfs_counter_write("perf_enable", 0);
    sysfs_counter_write("perf_clear", 1);
    if (!sampled) return {};

    std::vector<CounterValue> out;
    out.reserve(COUNTERS);
    for (unsigned i = 0; i < COUNTERS; ++i)
    {
        CounterValue v;
        const uint32_t select = post.pctrs[i];
        v.source = static_cast<int>(select & 0x1Fu);
        v.value = post.pctr[i];
        v.powered_off = (v.value == POWEROFF_VALUE);
        v.rate = v.powered_off ? 0.0 : v.value * 1000.0 / std::max(1, interval_ms);
        out.push_back(v);
    }
    return out;
}

std::vector<CounterValue> sample_counters(int page, int interval_ms)
{
    if (!counters_available())
    {
        // Keep the sampling cadence but do not touch the counters: the
        // module is missing or its counter attributes are not writable.
        wait_interval(interval_ms);
        return {};
    }
    if (g_reg_source == REGS_SYSFS)
        return sample_counters_sysfs(interval_ms);

    const std::vector<int> sources = page_sources(page);
    const size_t n = std::min<size_t>(sources.size(), 16);

    wr(V3D_PCTRE, 0);
    wr(V3D_PCTRC, 0xFFFFu);
    for (size_t i = 0; i < n; ++i)
    {
        wr(V3D_PCTRS0 + 8 * static_cast<uint32_t>(i), static_cast<uint32_t>(sources[i]));
        wr(V3D_PCTRC, 1u << i);
    }
    // Table 84 marks bit 31 reserved, but on this V3D the counters do not
    // advance unless bit 31 is set (the original vc4top does the same).
    wr(V3D_PCTRE, 0x80000000u);
    for (size_t i = 0; i < n; ++i)
        wr(V3D_PCTRE, rd(V3D_PCTRE) | (1u << i));

    wait_interval(interval_ms);

    std::vector<CounterValue> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        CounterValue v;
        v.source = sources[i];
        v.value = rd(V3D_PCTR0 + 8 * static_cast<uint32_t>(i));
        v.powered_off = (v.value == POWEROFF_VALUE);
        v.rate = v.powered_off ? 0.0 : v.value * 1000.0 / std::max(1, interval_ms);
        out.push_back(v);
    }

    for (size_t i = 0; i < n; ++i)
        wr(V3D_PCTRE, rd(V3D_PCTRE) & ~(1u << i));
    wr(V3D_PCTRC, 0xFFFFu);
    return out;
}

const char* counter_name(int id)
{
    const int n = static_cast<int>(sizeof(counters) / sizeof(counters[0]));
    return (id >= 0 && id < n) ? counters[id].desc : "unknown";
}

// Short, stable labels for the live view only. Full descriptions are kept in
// --once / --all-counters.
const char* counter_short(int id)
{
    static const char* names[30] = {
        "FEP no-pixels", "FEP valid prim", "FEP clipped quads", "FEP valid quads",
        "TLB stencil fail", "TLB Z+stencil fail", "TLB Z+stencil pass", "TLB zero coverage",
        "TLB non-zero cov", "TLB color write", "PTB offscreen", "PTB clipped",
        "PSE reversed", "QPU idle cycles", "QPU vertex cycles", "QPU fragment cyc",
        "QPU valid cycles", "QPU TMU stall", "QPU scoreboard st", "QPU varying stall",
        "QPU i-cache hits", "QPU i-cache miss", "QPU u-cache hits", "QPU u-cache miss",
        "TMU texture quads", "TMU cache misses", "VPM VDW stall", "VPM VCD stall",
        "L2C hits", "L2C misses"};
    return (id >= 0 && id < 30) ? names[id] : "unknown";
}

std::vector<std::string> packed(const std::string& label, const std::vector<std::string>& chunks,
    int width)
{
    std::vector<std::string> out;
    if (width < 10) width = 10;
    const size_t lw = 8;
    std::string prefix = colorize(K_CYAN, pad_visible(label, lw)) + " ";
    std::string indent(lw + 1, ' ');
    std::string cur = prefix;
    bool has = false;
    for (const auto& ch : chunks)
    {
        if (ch.empty()) continue;
        if (has && visible_len(cur) + 2 + visible_len(ch) > static_cast<size_t>(width))
        {
            out.push_back(clip_vis(cur, static_cast<size_t>(width)));
            cur = indent + ch;
        }
        else
        {
            if (has) cur += "  ";
            cur += ch;
        }
        has = true;
    }
    out.push_back(clip_vis(cur, static_cast<size_t>(width)));
    return out;
}

void append(std::vector<std::string>& dst, const std::vector<std::string>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

std::string section_line(const std::string& title, int width)
{
    std::string s = colorize(K_HEAD, title);
    const size_t n = visible_len(s);
    if (n + 2 < static_cast<size_t>(width))
        s += colorize(K_DIM, " " + std::string(static_cast<size_t>(width) - n - 1, '-'));
    return clip_vis(s, static_cast<size_t>(width));
}

std::vector<std::string> system_lines(const SysData& s, int width)
{
    std::vector<std::string> b;
    const uint32_t thr = static_cast<uint32_t>(strtoul(s.throttle.c_str(), nullptr, 16));

    append(b, packed("status", {
        s.time,
        "temp " + colorize(temp_color(s.temp), norm_temp(s.temp)),
        "thr " + colorize(throttle_color(thr), s.throttle + " (" + s.throttle_decoded + ")")},
        width));

    append(b, packed("clocks", {
        "arm " + mhz_from_hz(s.arm_clk),
        "core " + mhz_from_hz(s.core_clk),
        "v3d " + mhz_from_hz(s.v3d_clk),
        "h264 " + mhz_from_hz(s.h264_clk),
        "isp " + mhz_from_hz(s.isp_clk),
        "pixel " + mhz_from_hz(s.pixel_clk),
        "hdmi " + mhz_from_hz(s.hdmi_clk),
        "vec " + mhz_from_hz(s.vec_clk),
        "emmc " + mhz_from_hz(s.emmc_clk)}, width));

    append(b, packed("volts", {
        "core " + s.volts_core,
        "sdram_c " + s.volts_sdram}, width));

    append(b, packed("memory", {
        "gpu " + mem_with_units(s.gpu_mem),
        "arm " + mem_with_units(s.arm_mem),
        cma_units(s.cma)}, width));

    // Two pools, each represented exactly once: label + bar + percentage +
    // used/total MiB. The raw firmware split and CMA free stay on the line
    // above, so no value is lost.
    {
        const std::string ind(9, ' ');
        const int room = std::max(12, width - 9);
        b.push_back(clip_vis(ind + mem_bar_line("ARM", arm_mem_stat(s), room),
            static_cast<size_t>(width)));
        b.push_back(clip_vis(ind + mem_bar_line("GPU/CMA", gpu_cma_stat(s), room),
            static_cast<size_t>(width)));
    }

    append(b, packed("power", {
        "v3d " + s.v3d_state + " / " + s.v3d_control,
        "undervolt " + colorize(s.undervolt == "0" ? K_GREEN : K_RED, s.undervolt.empty() ? "n/a" : s.undervolt),
        "hvs underrun " + colorize(s.hvs_underrun == "0" ? K_GREEN : K_RED, s.hvs_underrun.empty() ? "n/a" : s.hvs_underrun),
        s.clients}, width));

    append(b, packed("display", {
        "drm " + (s.drm.empty() ? std::string("n/a") : s.drm),
        "hdmi " + (s.hdmi_status.empty() ? std::string("n/a") : s.hdmi_status) + " " + s.hdmi_mode,
        "bo " + (s.bo_stats.empty() ? std::string("none") : s.bo_stats)}, width));
    return b;
}

std::vector<std::string> qpu_lines(const QpuData& q, int width)
{
    std::vector<std::string> b;
    if (!q.powered)
    {
        b.push_back(colorize(K_RED, "V3D POWERED OFF (0xdeadbeef)"));
        b.push_back("start vc4cl-v3d-power or write on to power/control");
        return b;
    }
    const uint32_t queue = q.srqcs & 0x3F;
    const uint32_t requests = (q.srqcs >> 8) & 0xFF;
    const uint32_t done = (q.srqcs >> 16) & 0xFF;
    const uint32_t qerr = (q.srqcs >> 7) & 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%d slices x %d QPUs = %d", q.slices, q.qpus_per_slice, q.qpus);
    const std::string slices = buf;
    snprintf(buf, sizeof(buf), "%d TMUs", q.tmus);
    const std::string tmus = buf;
    snprintf(buf, sizeof(buf), "%d semaphores", q.semaphores);
    const std::string sem = buf;
    snprintf(buf, sizeof(buf), "VPM %d KiB", q.vpm_kb);
    const std::string vpm = buf;
    snprintf(buf, sizeof(buf), "HDR %d", q.hdr);
    const std::string hdr = buf;
    snprintf(buf, sizeof(buf), "reserved %u B", (q.vpmbase & 0x1F) * 256);

    append(b, packed("ident", {
        "rev " + std::to_string(q.revision), slices, tmus, sem, vpm, hdr}, width));
    append(b, packed("regs", {
        "IDENT0 " + hex32(q.ident0), "IDENT1 " + hex32(q.ident1), "IDENT2 " + hex32(q.ident2)}, width));
    append(b, packed("queue", {
        "SRQCS " + hex32(q.srqcs),
        "queue " + std::to_string(queue),
        "req " + std::to_string(requests),
        "done " + std::to_string(done),
        "qerr " + colorize(qerr ? K_RED : K_GREEN, std::to_string(qerr))}, width));
    append(b, packed("pointers", {
        "SRQPC " + hex32(q.srqpc), "SRQUA " + hex32(q.srqua), "SRQUL " + hex32(q.srqul)}, width));
    append(b, packed("vpm", {
        "VPMBASE " + hex32(q.vpmbase), buf, "VPACNTL " + hex32(q.vpacntl)}, width));
    append(b, packed("sched", {
        "SQRSV0 " + hex32(q.sqrsv0), "SQRSV1 " + hex32(q.sqrsv1), "SQCNTL " + hex32(q.sqcntl)}, width));
    append(b, packed("irq", {
        "INTCTL " + hex32(q.intctl), "INTENA " + hex32(q.intena), "INTDIS " + hex32(q.intdis)}, width));
    append(b, packed("perf", {
        "PCTRE " + hex32(q.pctre), "PCTRC " + hex32(q.pctrc),
        "L2CACTL " + hex32(q.l2cactl), "SLCACTL " + hex32(q.slcactl)}, width));
    append(b, packed("errors", {
        "ERRSTAT " + hex32(q.errstat),
        colorize(errstat_is_clean(q.errstat) ? K_GREEN : K_RED, errstat_string(q.errstat))}, width));
    return b;
}

void utilization_values(const std::vector<CounterValue>& counters, double c[7], double& total)
{
    for (int i = 0; i < 7; ++i) c[i] = 0;
    for (const auto& v : counters)
        if (v.source >= 13 && v.source <= 19 && !v.powered_off)
            c[v.source - 13] += v.value;
    total = 0;
    for (int i = 0; i < 7; ++i) total += c[i];
    if (total <= 0) { c[0] = 1; total = 1; }
}

// One utilization metric per line: aligned label, bar, single percentage.
// The bar and the percentage encode the same value, so the number is printed
// exactly once (the previous layout printed a percentage summary line and a
// separate bar for every metric).
std::string make_util_line(const std::string& label, double pct, int barw)
{
    pct = std::max(0.0, std::min(100.0, pct));
    int filled = static_cast<int>(pct / 100.0 * barw + 0.5);
    std::string bar = "[";
    bar += std::string(filled, '#');
    bar += std::string(std::max(0, barw - filled), '.');
    bar += "]";
    const char* c = K_GREEN;
    if (pct >= 80.0) c = K_RED;
    else if (pct >= 50.0) c = K_YELLOW;
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), " %5.1f%%", pct);
    return colorize(K_CYAN, pad_visible(label, 11)) + " " + colorize(c, bar) + pbuf;
}

std::vector<std::string> utilization_lines(const std::vector<CounterValue>& counters, int width,
    int max_bar = 30)
{
    if (!counters_available())
        return {colorize(K_DIM, "n/a (vc4mon counter attributes unavailable)")};
    static const char* labels[7] = {"Idle", "Vertex", "Fragment", "Valid", "Stall TMU", "Stall SB", "Stall Vary"};
    double c[7];
    double total = 0;
    utilization_values(counters, c, total);
    std::vector<std::string> b;
    const int barw = std::max(8, std::min(max_bar, width - 23));
    for (int i = 0; i < 7; ++i)
        b.push_back(clip_vis(make_util_line(labels[i], c[i] / total * 100.0, barw),
            static_cast<size_t>(width)));
    return b;
}

std::string page_title(int page)
{
    if (g_reg_source == REGS_SYSFS)
        return "hardware counter map (fixed; 16 counters)";
    if (page == 0) return "page 1/3  QPU/GPU primary (13-28)";
    if (page == 1) return "page 2/3  FEP/TLB/PTB/PSE (0-15)";
    return "page 3/3  QPU/TMU/VPM/L2C (16-29)";
}

std::vector<std::string> counter_lines(const std::vector<CounterValue>& counters, int page, int width,
    bool with_title = true)
{
    std::vector<std::string> b;
    if (!counters_available())
    {
        b.push_back(colorize(K_DIM, "counters unavailable"));
        b.push_back(colorize(K_DIM, g_counter_error.empty()
            ? "install vc4top-dkms >= 1.0.5" : g_counter_error));
        return b;
    }
    if (with_title)
    {
        std::string title = page_title(page) + "  counts";
        if (visible_len(title) > static_cast<size_t>(width))
            title = page_title(page);
        if (visible_len(title) > static_cast<size_t>(width))
            title = "page " + std::to_string(page + 1) + "/3";
        b.push_back(colorize(K_DIM, clip_vis(title, static_cast<size_t>(width))));
    }
    const int name_w = std::max(8, std::min(28, width - 28));
    char buf[256];
    snprintf(buf, sizeof(buf), "%-3s %-*s %10s %12s", "ID", name_w, "NAME", "VALUE", "RATE cnt/s");
    b.push_back(colorize(K_DIM, truncate_plain(buf, static_cast<size_t>(width))));
    for (const auto& v : counters)
    {
        std::string name = truncate_plain(counter_short(v.source), static_cast<size_t>(name_w));
        if (v.powered_off)
            snprintf(buf, sizeof(buf), "%-3d %-*s %10s %12s", v.source, name_w, name.c_str(), "POWERED", "-");
        else
            snprintf(buf, sizeof(buf), "%-3d %-*s %10u %12.1f", v.source, name_w, name.c_str(), v.value, v.rate);
        b.push_back(colorize(v.value == 0 || v.powered_off ? K_DIM : K_RESET, buf));
    }
    if (page != 0 && g_reg_source != REGS_SYSFS)
        b.push_back(colorize(K_DIM, "page changed; zero counts are expected for the first sample"));
    return b;
}

std::string register_group(int offset)
{
    if (offset < 0x00040) return "IDENTITY / GLOBAL / INTERRUPTS";
    if (offset < 0x00200) return "CONTROL LIST EXECUTOR / PIPELINE";
    if (offset < 0x00400) return "BINNER MEMORY";
    if (offset < 0x00500) return "QPU SCHEDULER / QUEUES";
    if (offset < 0x00600) return "VPM";
    if (offset < 0x00f00) return "PERFORMANCE COUNTERS";
    return "DEBUG / ERRORS";
}

std::vector<std::string> register_lines(int width)
{
    refresh_regs();
    std::vector<std::string> b;
    const int total = static_cast<int>(sizeof(registers) / sizeof(registers[0]));
    std::string last;
    char buf[512];
    for (int i = 0; i < total; ++i)
    {
        const std::string group = register_group(registers[i].offset);
        if (last != group)
        {
            b.push_back(colorize(K_HEAD, std::string("-- ") + group + " " +
                std::string(std::max(1, width - static_cast<int>(group.size()) - 4), '-')));
            last = group;
        }
        snprintf(buf, sizeof(buf), "  %-12s @0x%05x  %-10s  %s", registers[i].name,
            registers[i].offset, hex32(rd(registers[i].offset)).c_str(),
            truncate_plain(registers[i].desc, static_cast<size_t>(std::max(10, width - 40))).c_str());
        b.push_back(buf);
    }
    return b;
}

std::vector<std::string> box(const std::string& title, const std::vector<std::string>& body, int width)
{
    if (width < 12) width = 12;
    std::vector<std::string> out;
    std::string top = "+- " + truncate_plain(title, static_cast<size_t>(width - 8)) + " ";
    if (static_cast<int>(top.size()) < width - 1) top += std::string(width - 1 - top.size(), '-');
    top += "+";
    out.push_back(colorize(K_CYAN, top));
    for (const auto& b : body)
    {
        std::string content = clip_vis(b, static_cast<size_t>(std::max(4, width - 4)));
        std::string line = "| " + content;
        const size_t n = visible_len(line);
        if (static_cast<int>(n) < width - 1) line += std::string(width - 1 - n, ' ');
        line += "|";
        out.push_back(line);
    }
    out.push_back(colorize(K_CYAN, "+" + std::string(width - 2, '-') + "+"));
    return out;
}

// True when the line starts a new labelled group: skip any leading ANSI escape
// sequence, then require a non-space visible character. Continuation lines are
// indented, so this distinguishes them from labels without parsing colour.
static bool starts_group(const std::string& line)
{
    size_t i = 0;
    while (i < line.size() && line[i] == '\033')
    {
        ++i;
        if (i < line.size() && line[i] == '[')
        {
            ++i;
            while (i < line.size() && (line[i] < '@' || line[i] > '~')) ++i;
            if (i < line.size()) ++i;   // final selector byte
        }
    }
    return i < line.size() && line[i] != ' ' && line[i] != '\t';
}

// Grow a panel body to exactly `target` rows by inserting blank rows between
// labelled groups. The extra rows are spread over the groups instead of being
// dumped against the bottom border, so tall panels keep an even rhythm.
static std::vector<std::string> spread_lines(const std::vector<std::string>& body, int target)
{
    const int n = static_cast<int>(body.size());
    if (target <= n) return body;
    int extra = target - n;
    std::vector<int> starts;
    for (int i = 0; i < n; ++i)
        if (starts_group(body[i])) starts.push_back(i);
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(target));
    if (starts.empty())
    {
        out = body;
    }
    else
    {
        // Never insert blank rows above the first group: the panel's own top
        // border already provides that separation.
        const int slots = static_cast<int>(starts.size()) - 1;
        const int base = slots > 0 ? extra / slots : 0;
        const int rem = slots > 0 ? extra % slots : 0;
        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            if (k > 0 && k <= slots && starts[k] == i)
            {
                const int pad = base + (k <= rem ? 1 : 0);
                for (int j = 0; j < pad; ++j) out.push_back("");
            }
            out.push_back(body[i]);
            if (k <= slots && starts[k] == i) ++k;
        }
    }
    while (static_cast<int>(out.size()) < target) out.push_back("");
    return out;
}

// box() with a requested height: the body is spread, then padded with blank
// content rows just above the bottom border, so every panel in a row ends on
// the same screen line. A natural body taller than the request is kept intact
// (the caller decides whether that means scrolling).
static std::vector<std::string> box_sized(const std::string& title,
    const std::vector<std::string>& body, int width, int height)
{
    const int inner = std::max(0, height - 2);
    std::vector<std::string> spread = spread_lines(body, inner);
    std::vector<std::string> out = box(title, spread, width);
    while (static_cast<int>(out.size()) < height)
        out.insert(out.end() - 1, std::string(static_cast<size_t>(width), ' '));
    return out;
}

// Compose already equal-height boxes side by side with a single-column gutter.
static void compose_row(const std::vector<std::vector<std::string>>& cells,
    const std::vector<int>& widths, int gap, std::vector<std::string>& out)
{
    size_t n = 0;
    for (const auto& c : cells) n = std::max(n, c.size());
    for (size_t i = 0; i < n; ++i)
    {
        std::string line;
        for (size_t k = 0; k < cells.size(); ++k)
        {
            const std::string part = i < cells[k].size() ? cells[k][i] : "";
            // Pad every cell but the last: the last one must end exactly at the
            // terminal width or the single-line clip marker appears on-screen.
            line += pad_visible(part, static_cast<size_t>(widths[k] +
                (k + 1 < cells.size() ? gap : 0)));
        }
        out.push_back(line);
    }
}

// One counter row in the same layout as counter_lines(); split out so the wide
// view can place two tables side by side without duplicating the format.
static std::string counter_row_text(const CounterValue& v, int name_w)
{
    char buf[256];
    const std::string name = truncate_plain(counter_short(v.source), static_cast<size_t>(name_w));
    if (v.powered_off)
        snprintf(buf, sizeof(buf), "%-3d %-*s %10s %12s", v.source, name_w, name.c_str(), "POWERED", "-");
    else
        snprintf(buf, sizeof(buf), "%-3d %-*s %10u %12.1f", v.source, name_w, name.c_str(), v.value, v.rate);
    return colorize(v.value == 0 || v.powered_off ? K_DIM : K_RESET, buf);
}

static std::string counter_header_text(int name_w)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%-3s %-*s %10s %12s", "ID", name_w, "NAME", "VALUE", "RATE cnt/s");
    return colorize(K_DIM, buf);
}

// Wide counter table: two rank-ordered halves side by side, each with its own
// header and right-aligned numeric columns. This uses the extra width instead
// of leaving a 100-column gutter inside one narrow table, and halves the rows
// the table needs. The source order is preserved left half then right half.
static std::vector<std::string> wide_counter_body(const std::vector<CounterValue>& counters,
    int width)
{
    if (counters.size() < 4)
        return counter_lines(counters, 0, width, false);
    const int inner = std::max(40, width - 4);
    const int gutter = 3;
    const int half = (inner - gutter) / 2;
    // Fill the half: right-align the numeric columns at its right edge instead
    // of leaving an unused band after RATE on very wide terminals.
    const int name_w = std::max(8, half - 28);
    const int left_n = (static_cast<int>(counters.size()) + 1) / 2;
    std::vector<std::string> left;
    std::vector<std::string> right;
    left.push_back(counter_header_text(name_w));
    right.push_back(counter_header_text(name_w));
    for (int i = 0; i < static_cast<int>(counters.size()); ++i)
    {
        std::string row = counter_row_text(counters[i], name_w);
        if (i < left_n) left.push_back(row);
        else right.push_back(row);
    }
    std::vector<std::string> out;
    const size_t n = std::max(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
    {
        const std::string l = i < left.size() ? left[i] : "";
        const std::string r = i < right.size() ? right[i] : "";
        out.push_back(pad_visible(l, static_cast<size_t>(half + gutter)) + r);
    }
    return out;
}

// Wide layout notes: the old 12-row FLAGS / NOTES panel, reflowed into as few
// full-width rows as the terminal needs. It keeps every hint but stops
// reserving a tall column for documentation on a short desktop screen.
static std::vector<std::string> wide_note_lines(int cols)
{
    static const char* chunks[] = {
        "flags --once --all-counters --regs --poll [ms] --frame --no-color",
        "plain: NO_COLOR=1 or piped",
        "30 counter sources / 3 pages (a/[] cycles)",
        "rate: counts/s over interval",
        "memory: ARM from /proc, GPU/CMA pool",
        "util: label + bar + %, printed once",
        "QPU counters are 3D-oriented; raw compute may leave idle/valid at zero",
        "h: full help/flags"};
    std::vector<std::string> out;
    std::string line;
    for (const char* chunk : chunks)
    {
        const std::string piece = chunk;
        const std::string sep = line.empty() ? "" : "   |   ";
        if (!line.empty() &&
            static_cast<int>(visible_len(line + sep + piece)) > cols)
        {
            out.push_back(colorize(K_DIM, line));
            line = piece;
        }
        else
        {
            line += sep + piece;
        }
    }
    if (!line.empty()) out.push_back(colorize(K_DIM, line));
    return out;
}

void compose_columns(const std::vector<std::string>& left, const std::vector<std::string>& right,
    int left_width, int gap, std::vector<std::string>& out)
{
    const size_t n = std::max(left.size(), right.size());
    for (size_t i = 0; i < n; ++i)
    {
        std::string l = i < left.size() ? left[i] : "";
        std::string r = i < right.size() ? right[i] : "";
        out.push_back(pad_visible(l, static_cast<size_t>(left_width + gap)) + r);
    }
}

void get_terminal_size(int& rows, int& cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    else
    {
        rows = 40;
        cols = 120;
        const char* er = getenv("LINES");
        const char* ec = getenv("COLUMNS");
        if (er && atoi(er) > 0) rows = atoi(er);
        if (ec && atoi(ec) > 0) cols = atoi(ec);
    }
}

uint64_t g_write_calls = 0;
uint64_t g_frame_write_calls = 0;
uint64_t g_render_us = 0;
uint64_t g_sys_us = 0;
uint64_t g_sample_us = 0;
uint64_t g_frames = 0;

void write_all(const char* p, size_t n)
{
    while (n > 0)
    {
        const ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        ++g_write_calls;
        p += w;
        n -= static_cast<size_t>(w);
    }
}

// One write() for the whole frame: no per-line stdio flush, no tearing.
void write_frame(const std::string& frame)
{
    fflush(stdout);
    write_all(frame.data(), frame.size());
}

// Wide layouts have room for the flag/notes panel in addition to the
// always-on two-row footer legend.
std::vector<std::string> note_lines()
{
    return {
        "flags --once  --all-counters",
        "      --regs  --poll [ms]",
        "      --frame  --no-color",
        "plain: NO_COLOR=1 or piped",
        "30 counter sources / 3 pages",
        "rate = counts/s over interval",
        "memory: ARM /proc, GPU/CMA pool",
        "util: label + bar + %, printed once",
        "QPU counters are 3D-oriented;",
        "raw compute may leave idle/",
        "valid at zero.",
        "h = full help screen"};
}

void compose_three(const std::vector<std::string>& a, const std::vector<std::string>& b,
    const std::vector<std::string>& c, int wa, int wb, std::vector<std::string>& out)
{
    const size_t n = std::max(a.size(), std::max(b.size(), c.size()));
    for (size_t i = 0; i < n; ++i)
    {
        std::string la = i < a.size() ? a[i] : "";
        std::string lb = i < b.size() ? b[i] : "";
        std::string lc = i < c.size() ? c[i] : "";
        out.push_back(pad_visible(la, static_cast<size_t>(wa + 1)) +
            pad_visible(lb, static_cast<size_t>(wb + 1)) + lc);
    }
}

// Desktop / horizontal layout (>= 160 columns). Three content-sized panels
// across the top and a full-width counter table below, with the flag notes
// reflowed into a full-width strip. Both grid rows are stretched to use every
// remaining screen row, so a short 45-row desktop terminal does not waste the
// width on fixed percentage columns or leave a blank band under the panels.
std::vector<std::string> wide_desktop_content(const SysData& sys, const QpuData& qpu,
    const std::vector<CounterValue>& counters, int page, int cols, int avail)
{
    std::vector<std::string> out;
    if (avail <= 0) return out;

    const int gap = 1;
    int sys_w = std::max(50, std::min(84, cols * 31 / 100));
    int qpu_w = std::max(50, std::min(84, cols * 28 / 100));
    int util_w = cols - sys_w - qpu_w - 2 * gap;
    // Keep the utilization panel readable on the narrowest wide screen; take
    // the space from the two text panels proportionally if it is not.
    while (util_w < 42 && (sys_w > 50 || qpu_w > 50))
    {
        if (sys_w >= qpu_w && sys_w > 50) --sys_w;
        else if (qpu_w > 50) --qpu_w;
        else --sys_w;
        util_w = cols - sys_w - qpu_w - 2 * gap;
    }

    const std::vector<std::string> sys_body = system_lines(sys, sys_w - 4);
    const std::vector<std::string> qpu_body = qpu_lines(qpu, qpu_w - 4);
    // Wide panels have room for bars that actually use the available width.
    const std::vector<std::string> util_body =
        utilization_lines(counters, util_w - 4, std::max(30, util_w - 27));
    std::vector<std::string> ctr_body = wide_counter_body(counters, cols);
    if (page != 0 && g_reg_source != REGS_SYSFS)
        ctr_body.push_back(colorize(K_DIM,
            "page changed; zero counts are expected for the first sample"));

    const int top_nat = std::max({static_cast<int>(sys_body.size()),
        static_cast<int>(qpu_body.size()), static_cast<int>(util_body.size())}) + 2;
    const int ctr_nat = static_cast<int>(ctr_body.size()) + 2;
    const std::vector<std::string> notes = wide_note_lines(cols);
    const int notes_h = static_cast<int>(notes.size());
    // One blank separator row between the top row and the counters, and one
    // between the counters and the notes strip.
    const int fixed = ctr_nat + notes_h + 2;

    int top_h = top_nat;
    int ctr_h = ctr_nat;
    const int spare = avail - (top_nat + fixed);
    if (spare > 0)
    {
        // Give the counter table some of the extra height (it spreads between
        // its rows) but keep the top row, the visual focus, the larger share.
        const int to_ctr = spare * 2 / 5;
        ctr_h += to_ctr;
        top_h += spare - to_ctr;
    }

    std::vector<std::vector<std::string>> top;
    top.push_back(box_sized("SYSTEM / POWER / MEMORY", sys_body, sys_w, top_h));
    top.push_back(box_sized("QPU / QUEUES / VPM", qpu_body, qpu_w, top_h));
    top.push_back(box_sized("UTILIZATION", util_body, util_w, top_h));
    compose_row(top, {sys_w, qpu_w, util_w}, gap, out);

    out.push_back(std::string(static_cast<size_t>(cols), ' '));

    std::string ctr_title = "COUNTERS  " + page_title(page);
    append(out, box_sized(ctr_title, ctr_body, cols, ctr_h));

    out.push_back(std::string(static_cast<size_t>(cols), ' '));
    for (const auto& line : notes)
        out.push_back(pad_visible(clip_vis(line, static_cast<size_t>(cols)),
            static_cast<size_t>(cols)));
    return out;
}

std::vector<std::string> wide_content(const SysData& sys, const QpuData& qpu,
    const std::vector<CounterValue>& counters, int page, int cols, int avail)
{
    std::vector<std::string> out;
    if (cols >= 160)
        return wide_desktop_content(sys, qpu, counters, page, cols, avail);
    if (cols >= 136)
    {
        const int w1 = cols * 35 / 100;
        const int w2 = cols * 28 / 100;
        const int w3 = cols - w1 - w2 - 2;
        std::vector<std::string> c1;
        append(c1, box("SYSTEM / POWER / MEMORY", system_lines(sys, w1 - 4), w1));
        append(c1, box("UTILIZATION", utilization_lines(counters, w1 - 4), w1));
        std::vector<std::string> c2;
        append(c2, box("QPU / QUEUES / VPM", qpu_lines(qpu, w2 - 4), w2));
        std::vector<std::string> c3;
        append(c3, box("COUNTERS", counter_lines(counters, page, w3 - 4), w3));
        append(c3, box("FLAGS / NOTES", note_lines(), w3));
        compose_three(c1, c2, c3, w1, w2, out);
        return out;
    }

    const int left_w = std::max(46, std::min(68, cols * 46 / 100));
    const int right_w = cols - left_w - 1;
    std::vector<std::string> left;
    append(left, box("SYSTEM / POWER / MEMORY", system_lines(sys, left_w - 4), left_w));
    append(left, box("QPU / QUEUES / VPM", qpu_lines(qpu, left_w - 4), left_w));
    append(left, box("UTILIZATION", utilization_lines(counters, left_w - 4), left_w));
    std::vector<std::string> right;
    append(right, box("COUNTERS", counter_lines(counters, page, right_w - 4), right_w));
    append(right, box("FLAGS / NOTES", note_lines(), right_w));
    compose_columns(left, right, left_w, 1, out);
    return out;
}

std::vector<std::string> narrow_content(const SysData& sys, const QpuData& qpu,
    const std::vector<CounterValue>& counters, int page, int cols, int rows)
{
    std::vector<std::string> out;
    const int w = cols - 2;
    const bool roomy = rows >= 30;
    if (roomy) out.push_back(section_line("STATUS", cols));
    for (const auto& l : system_lines(sys, w)) out.push_back(" " + l);
    out.push_back(section_line("UTILIZATION", cols));
    for (const auto& l : utilization_lines(counters, w)) out.push_back(" " + l);
    // The counter page title replaces the old separate COUNTERS heading, so
    // the table header + first rows stay on the 80x24 screen now that the
    // always-on legend occupies two footer rows.
    out.push_back(section_line("COUNTERS  " + page_title(page) + "  counts", cols));
    for (const auto& l : counter_lines(counters, page, w, false)) out.push_back(" " + l);
    out.push_back(section_line("QPU / QUEUES / VPM", cols));
    for (const auto& l : qpu_lines(qpu, w)) out.push_back(" " + l);
    return out;
}

std::string build_header(const SysData& sys, const QpuData& qpu, bool paused, int interval_ms, int cols)
{
    const uint32_t thr = static_cast<uint32_t>(strtoul(sys.throttle.c_str(), nullptr, 16));
    std::string stamp = sys.time;
    if (cols < 70)
    {
        const auto sp = stamp.find(' ');
        if (sp != std::string::npos) stamp = stamp.substr(sp + 1);
    }
    std::string h = colorize(K_BOLD, "VC4TOP");
    h += "  " + stamp;
    h += "  " + (qpu.powered ? colorize(K_GREEN, "V3D ON") : colorize(K_RED, "V3D OFF"));
    h += "  " + colorize(temp_color(sys.temp), norm_temp(sys.temp));
    h += "  thr " + colorize(throttle_color(thr), cols >= 70
        ? sys.throttle + " (" + sys.throttle_decoded + ")" : sys.throttle);
    h += "  " + std::to_string(interval_ms) + "ms";
    if (paused) h += colorize(K_MAGENTA, "  PAUSED");
    return pad_visible(clip_vis(h, static_cast<size_t>(cols)), static_cast<size_t>(cols));
}

// Always-on compact legend, two fixed rows at the bottom of the live view:
// row 1 = keys + visible-line range, row 2 = units + colour key. `h` still
// opens the full help/flags screen.
std::vector<std::string> build_footer(bool regs_view, int first, int last, int total, int cols)
{
    std::string keys;
    if (cols >= 110)
        keys = "q quit  a/[] page  g regs  j/k scroll  p pause  +/- int  h help/flags";
    else if (cols >= 80)
        keys = "q quit  a page  g regs  j/k scroll  p pause  +/- int  h help";
    else
        keys = "q quit  a page  g regs  j/k  p pause  +/-  h help";
    std::string range;
    if (total > 0 && (first > 1 || last < total))
    {
        char b[64];
        snprintf(b, sizeof(b), "  [%d-%d/%d]", first, last, total);
        range = b;
    }
    (void)regs_view;
    // Prefer keeping the range visible: drop trailing key hints if needed.
    std::string f = keys + range;
    while (visible_len(f) > static_cast<size_t>(cols) && keys.size() > 12)
    {
        auto p = keys.rfind("  ");
        if (p == std::string::npos) break;
        keys = keys.substr(0, p);
        f = keys + range;
    }

    std::string units;
    if (cols >= 100)
        units = "units  MHz  " "\xC2\xB0" "C  MiB  %  cnt/s    ";
    else if (cols >= 70)
        units = "units  MHz " "\xC2\xB0" "C MiB % cnt/s   ";
    else
        units = "MHz " "\xC2\xB0" "C MiB % cnt/s  ";
    units += colorize(K_GREEN, "ok") + " " + colorize(K_YELLOW, "warn") + " " +
        colorize(K_RED, "err") + " " + colorize(K_DIM, "idle");
    if (cols >= 100) units += "    h full help/flags";

    std::vector<std::string> out;
    out.push_back(pad_visible(clip_vis(colorize(K_DIM, f), static_cast<size_t>(cols)),
        static_cast<size_t>(cols)));
    out.push_back(pad_visible(clip_vis(colorize(K_DIM, units), static_cast<size_t>(cols)),
        static_cast<size_t>(cols)));
    return out;
}

void render_tui(const SysData& sys, const QpuData& qpu, const std::vector<CounterValue>& counters,
    int page, bool regs_view, int& reg_scroll, bool paused, int interval_ms, int& view_scroll)
{
    int rows = 40, cols = 120;
    get_terminal_size(rows, cols);
    // Never emit more lines or more cells than the terminal actually has:
    // a single overflowing line makes the terminal wrap and scroll.
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    const int layout_cols = std::max(cols, 40);

    const bool tty = isatty(STDOUT_FILENO) != 0;
    // Header + two fixed legend rows; the viewport is what is left.
    const int footer_rows = (rows >= 5) ? 2 : (rows >= 2 ? 1 : 0);
    const int avail = std::max(0, rows - 1 - footer_rows);
    std::vector<std::string> content;
    int first = 0, last = 0, total = 0;

    if (regs_view)
    {
        auto all = register_lines(layout_cols - 4);
        total = static_cast<int>(all.size());
        const int inner = std::max(0, avail - 2);
        const int max_scroll = std::max(0, total - inner);
        if (reg_scroll > max_scroll) reg_scroll = max_scroll;
        if (reg_scroll < 0) reg_scroll = 0;
        const int end = std::min(total, reg_scroll + inner);
        std::vector<std::string> slice(all.begin() + reg_scroll, all.begin() + end);
        char title[64];
        snprintf(title, sizeof(title), "V3D REGISTERS  %d-%d of %d", reg_scroll + 1, end, total);
        content = box(title, slice, layout_cols);
        first = reg_scroll + 1;
        last = end;
    }
    else
    {
        content = (layout_cols >= 100)
            ? wide_content(sys, qpu, counters, page, layout_cols, avail)
            : narrow_content(sys, qpu, counters, page, layout_cols, rows);
        total = static_cast<int>(content.size());
        const int max_scroll = std::max(0, total - avail);
        if (view_scroll > max_scroll) view_scroll = max_scroll;
        if (view_scroll < 0) view_scroll = 0;
        const int end = std::min(total, view_scroll + avail);
        content = std::vector<std::string>(content.begin() + view_scroll, content.begin() + end);
        first = view_scroll + 1;
        last = end;
    }

    std::vector<std::string> out;
    if (rows >= 1) out.push_back(build_header(sys, qpu, paused, interval_ms, layout_cols));
    if (avail > 0)
        for (const auto& l : content)
            out.push_back(pad_visible(clip_vis(l, static_cast<size_t>(layout_cols)),
                static_cast<size_t>(layout_cols)));
    if (footer_rows > 0)
    {
        const auto foot = build_footer(regs_view, first, last, total, layout_cols);
        for (int i = 0; i < footer_rows && i < static_cast<int>(foot.size()); ++i)
            out.push_back(foot[i]);
    }

    // Exactly `rows` physical lines: header + viewport + footer, then blanks.
    while (static_cast<int>(out.size()) < rows) out.push_back(std::string(layout_cols, ' '));
    if (static_cast<int>(out.size()) > rows) out.resize(rows);

    std::string frame;
    frame.reserve(static_cast<size_t>(rows) * (static_cast<size_t>(cols) + 16) + 32);
    const auto render_start = std::chrono::steady_clock::now();
    if (tty)
    {
        // Wrap off for the whole repaint, home once, one ESC[K per line.
        // The last line deliberately gets no newline: a newline on the
        // bottom row would scroll the alternate screen.
        frame += "\033[?7l\033[H";
        for (int i = 0; i < rows; ++i)
        {
            frame += pad_visible(clip_vis(out[i], static_cast<size_t>(cols)),
                static_cast<size_t>(cols));
            frame += "\033[K";
            if (i + 1 < rows) frame += "\r\n";
        }
        frame += "\033[?7h";
    }
    else
    {
        for (int i = 0; i < rows; ++i) frame += out[i] + "\n";
    }
    const uint64_t wc0 = g_write_calls;
    write_frame(frame);
    g_frame_write_calls += g_write_calls - wc0;
    const auto render_end = std::chrono::steady_clock::now();
    g_render_us += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(render_end - render_start).count());
    ++g_frames;
}

void print_sub(const std::string& title)
{
    std::string s = "-- " + title + " ";
    if (s.size() < 74) s += std::string(74 - s.size(), '-');
    printf("%s\n", s.c_str());
}

void row(const char* label, const std::string& value)
{
    printf("%-16s %s\n", label, value.c_str());
}

void print_counters(const std::vector<CounterValue>& c)
{
    printf("  %2s  %-66s %10s %12s\n", "ID", "NAME", "VALUE", "RATE");
    printf("  %2s  %-66s %10s %12s\n", "", "", "count", "count/s");
    printf("  %2s  %-66s %10s %12s\n", "--", "------------------------------------------------------------------",
        "----------", "------------");
    for (const auto& v : c)
        printf("  %2d  %-66s %10u %12.1f\n", v.source, counter_name(v.source), v.value, v.rate);
}

void print_register_dump()
{
    refresh_regs();
    const int total = static_cast<int>(sizeof(registers) / sizeof(registers[0]));
    std::string last;
    for (int i = 0; i < total; ++i)
    {
        const std::string group = register_group(registers[i].offset);
        if (last != group)
        {
            printf("\n  -- %s --\n", group.c_str());
            last = group;
        }
        printf("  %-12s @0x%05x  0x%08x  %s\n", registers[i].name, registers[i].offset,
            rd(registers[i].offset), registers[i].desc);
    }
}

void print_snapshot(bool all_counters, bool regs)
{
    SysData sys;
    QpuData qpu;
    collect_sys(sys);
    collect_qpu(qpu);

    printf("VC4TOP - VideoCore IV / V3D wide snapshot\n");
    printf("============================================================\n");

    printf("[ SYSTEM / POWER ]\n");
    print_sub("time & thermal");
    row("time:", sys.time);
    row("temp:", norm_temp(sys.temp));
    row("throttled:", sys.throttle + " (" + sys.throttle_decoded + ")");
    print_sub("voltages");
    row("core volts:", sys.volts_core);
    row("sdram_c volts:", sys.volts_sdram);
    print_sub("clocks (Hz / MHz)");
    row("arm clock:", clock_with_units(sys.arm_clk));
    row("core clock:", clock_with_units(sys.core_clk));
    row("v3d clock:", clock_with_units(sys.v3d_clk));
    row("h264 clock:", clock_with_units(sys.h264_clk));
    row("isp clock:", clock_with_units(sys.isp_clk));
    row("pixel clock:", clock_with_units(sys.pixel_clk));
    row("hdmi clock:", clock_with_units(sys.hdmi_clk));
    row("vec clock:", clock_with_units(sys.vec_clk));
    row("emmc clock:", clock_with_units(sys.emmc_clk));
    print_sub("memory");
    row("gpu mem:", mem_with_units(sys.gpu_mem));
    row("arm mem:", mem_with_units(sys.arm_mem));
    row("cma:", cma_units(sys.cma));
    {
        const MemStat arm = arm_mem_stat(sys);
        char b[96];
        if (arm.valid)
        {
            snprintf(b, sizeof(b), "%ld / %ld MiB (%.1f%% used, MemTotal-MemAvailable)",
                arm.used_mib, arm.total_mib, arm.pct);
            row("arm mem used:", b);
        }
        else
            row("arm mem used:", "n/a");

        const MemStat gpu = gpu_cma_stat(sys);
        if (gpu.valid)
        {
            snprintf(b, sizeof(b), "%ld / %ld MiB (%.1f%% used, CmaTotal-CmaFree)",
                gpu.used_mib, gpu.total_mib, gpu.pct);
            row("gpu cma used:", b);
        }
        else
            row("gpu cma used:", "n/a");
    }
    print_sub("power & display");
    row("v3d power:", sys.v3d_state + " / " + sys.v3d_control);
    row("undervoltage:", sys.undervolt.empty() ? "n/a" : sys.undervolt + (sys.undervolt == "0" ? " (ok)" : " (ALARM)"));
    row("hvs underrun:", sys.hvs_underrun.empty() ? "n/a" : sys.hvs_underrun + " (events)");
    row("drm:", sys.drm);
    row("hdmi:", sys.hdmi_status + " " + sys.hdmi_mode);
    row("bo_stats:", sys.bo_stats);

    printf("\n[ QPU / SCHEDULER ]\n");
    print_sub("identity & configuration");
    row("IDENT0:", hex32(qpu.ident0));
    row("IDENT1:", hex32(qpu.ident1));
    row("IDENT2:", hex32(qpu.ident2));
    row("revision:", std::to_string(qpu.revision));
    row("slices:", std::to_string(qpu.slices) + " slices");
    row("QPUs/slice:", std::to_string(qpu.qpus_per_slice) + " QPUs/slice (total " +
        std::to_string(qpu.qpus) + " QPUs)");
    row("TMUs/slice:", std::to_string(qpu.tmus_per_slice) + " TMUs/slice (total " +
        std::to_string(qpu.tmus) + " TMUs)");
    row("semaphores:", std::to_string(qpu.semaphores) + " semaphores");
    row("VPM size:", std::to_string(qpu.vpm_kb) + " KiB");
    row("HDR support:", std::to_string(qpu.hdr) + (qpu.hdr ? " (yes)" : " (no)"));
    print_sub("queues & scheduler");
    {
        char b[192];
        snprintf(b, sizeof(b), "%s queue=%u requests=%u completed=%u qerr=%u", hex32(qpu.srqcs).c_str(),
            qpu.srqcs & 0x3F, (qpu.srqcs >> 8) & 0xFF, (qpu.srqcs >> 16) & 0xFF, (qpu.srqcs >> 7) & 1);
        row("SRQCS:", b);
    }
    {
        char b[192];
        snprintf(b, sizeof(b), "%s reserved=%u bytes of %d KiB", hex32(qpu.vpmbase).c_str(),
            (qpu.vpmbase & 0x1F) * 256, qpu.vpm_kb);
        row("VPMBASE:", b);
    }
    row("VPACNTL:", hex32(qpu.vpacntl));
    row("SQRSV0/1:", hex32(qpu.sqrsv0) + " / " + hex32(qpu.sqrsv1));
    row("SQCNTL:", hex32(qpu.sqcntl));
    row("SRQPC/SRQUA:", hex32(qpu.srqpc) + " / " + hex32(qpu.srqua) + " (len " + hex32(qpu.srqul) + ")");
    print_sub("interrupts, counters & caches");
    row("INTCTL/ENA/DIS:", hex32(qpu.intctl) + " / " + hex32(qpu.intena) + " / " + hex32(qpu.intdis));
    row("PCTRE/PCTRC:", hex32(qpu.pctre) + " / " + hex32(qpu.pctrc));
    row("L2CACTL/SLCACTL:", hex32(qpu.l2cactl) + " / " + hex32(qpu.slcactl));
    print_sub("errors");
    row("ERRSTAT:", hex32(qpu.errstat) + " " + errstat_string(qpu.errstat));
    printf("\n");

    if (!counters_available())
    {
        printf("[ COUNTERS ]\n");
        printf("  unavailable: vc4mon perf_enable/perf_clear are not writable.\n");
        if (!g_counter_error.empty()) printf("  %s\n", g_counter_error.c_str());
        printf("  Install vc4top-dkms >= 1.0.5 and load vc4mon.\n\n");
    }
    else if (all_counters && g_reg_source == REGS_SYSFS)
    {
        auto c = sample_counters(0, DEFAULT_INTERVAL_MS);
        printf("[ COUNTERS (vc4mon hardware map) ]\n");
        printf("  Source selection needs root; these are the 16 mapped counters.\n");
        print_counters(c);
        printf("\n");
    }
    else if (all_counters)
    {
        for (int page = 1; page <= 2; ++page)
        {
            auto c = sample_counters(page, DEFAULT_INTERVAL_MS);
            printf("[ COUNTERS page %d/3 ]\n", page + 1);
            print_counters(c);
            printf("\n");
        }
    }
    else
    {
        auto c = sample_counters(0, DEFAULT_INTERVAL_MS);
        printf("[ PRIMARY COUNTERS ]\n");
        if (g_reg_source == REGS_SYSFS)
            printf("  Sources fixed by vc4mon; sampled via sysfs perf_enable/perf_clear.\n");
        print_counters(c);
        printf("\n");
    }

    if (regs)
    {
        printf("[ V3D REGISTER DUMP ]\n");
        print_register_dump();
    }
}

void poll_loop(int interval_ms)
{
    printf("time,srqcs,queue,requests,completed,qerr,srqpc,srqua,srqul,vpmbase,errstat,pctre\n");
    fflush(stdout);
    while (g_running)
    {
        refresh_regs();
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
        uint32_t srqcs = rd(V3D_SRQCS);
        printf("%s,0x%08x,%u,%u,%u,%u,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x\n",
            ts, srqcs, srqcs & 0x3F, (srqcs >> 8) & 0xFF, (srqcs >> 16) & 0xFF, (srqcs >> 7) & 1,
            rd(V3D_SRQPC), rd(V3D_SRQUA), rd(V3D_SRQUL), rd(V3D_VPMBASE),
            rd(V3D_ERRSTAT), rd(V3D_PCTRE));
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

struct TermState {
    struct termios orig;
    bool valid = false;
    bool alt = false;
};

// Alternate screen, hidden cursor, wrap disabled, cleared. Restoring turns
// wrap back on and returns to the shell screen. Both are single writes.
const char TERM_ENTER[] = "\033[?1049h\033[?7l\033[?25l\033[2J";
const char TERM_LEAVE[] = "\033[0m\033[?7h\033[?25h\033[?1049l";
volatile sig_atomic_t g_term_active = 0;

TermState setup_term()
{
    TermState st;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return st;
    if (tcgetattr(STDIN_FILENO, &st.orig) == 0)
    {
        struct termios raw = st.orig;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) st.valid = true;
    }
    g_term_active = 1;
    write_all(TERM_ENTER, sizeof(TERM_ENTER) - 1);
    st.alt = true;
    return st;
}

void restore_term(TermState& st)
{
    if (st.alt)
    {
        st.alt = false;
        if (g_term_active)
        {
            g_term_active = 0;
            write_all(TERM_LEAVE, sizeof(TERM_LEAVE) - 1);
        }
    }
    if (st.valid)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &st.orig);
        st.valid = false;
    }
}

int read_key()
{
    if (g_pending_key >= 0)
    {
        const int k = g_pending_key;
        g_pending_key = -1;
        return k;
    }
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return 0;
}

// SIGINT/SIGTERM/SIGHUP restore the terminal directly from the handler, using
// only async-signal-safe write(). If the process dies while blocked on the
// mailbox, the shell still gets its screen, cursor and line wrap back.
void on_signal(int)
{
    g_running = 0;
    if (g_term_active)
    {
        g_term_active = 0;
        ssize_t r = write(STDOUT_FILENO, TERM_LEAVE, sizeof(TERM_LEAVE) - 1);
        (void)r;
    }
}
void on_winch(int) { g_winch = 1; }

void help_screen()
{
    int rows = 40, cols = 120;
    get_terminal_size(rows, cols);
    std::vector<std::string> body = {
        "KEYS",
        "  q quit    a/[] page    g regs",
        "  j/k scroll    p pause    +/- interval",
        "  h this help (any key returns)",
        "FLAGS",
        "  --once            wide snapshot + primary counters",
        "  --all-counters    all 30 counter sources",
        "  --regs            full V3D register map",
        "  --poll [ms]       CSV scheduler stream",
        "  --frame           one TUI frame, then exit",
        "  --no-color        plain text (or NO_COLOR=1)",
        "UNITS",
        "  MHz clocks, \xC2\xB0\x43 temperature, MiB memory, % utilization,",
        "  counts and cnt/s for counters, 0x hex for registers",
        "  the footer legend is always visible",
        "MEMORY",
        "  ARM      MemTotal - MemAvailable (/proc/meminfo)",
        "  GPU/CMA  CmaTotal - CmaFree (GPU pool)",
        "  each: label + bar + % + used/total MiB",
        "UTILIZATION",
        "  one line per metric: label + bar + %",
        "REGISTERS",
        "  root: /dev/mem; unprivileged: /sys/kernel/vc4mon",
        "  counters: perf_enable/perf_clear only (PCTRE/PCTRC)",
        "Any key returns."
    };
    int w = std::min(cols - 2, 72);
    auto boxed = box("VC4TOP HELP", body, w);
    // Same repaint discipline as the live view: one write, never more lines
    // than the terminal has, and no newline on the last row (which would
    // scroll the alternate screen).
    std::vector<std::string> out;
    const int start = std::max(0, (rows - static_cast<int>(boxed.size())) / 2);
    for (int i = 0; i < start && static_cast<int>(out.size()) < rows; ++i) out.push_back("");
    for (const auto& l : boxed)
        if (static_cast<int>(out.size()) < rows) out.push_back(l);

    std::string frame = "\033[H\033[2J";
    for (size_t i = 0; i < out.size(); ++i)
    {
        frame += pad_visible(clip_vis(out[i], static_cast<size_t>(cols)), static_cast<size_t>(cols));
        if (i + 1 < out.size()) frame += "\r\n";
    }
    write_frame(frame);
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) != 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

bool want_color(bool no_color)
{
    if (no_color) return false;
    if (getenv("NO_COLOR") != nullptr) return false;
    return isatty(STDOUT_FILENO) != 0;
}

} // namespace

int main(int argc, char** argv)
{
    bool once = false, all_counters = false, regs = false, poll = false, frame_only = false, no_color = false;
    int interval_ms = DEFAULT_INTERVAL_MS;

    static const struct option long_opts[] = {
        {"once", no_argument, nullptr, 'o'},
        {"all-counters", no_argument, nullptr, 'A'},
        {"regs", no_argument, nullptr, 'R'},
        {"poll", optional_argument, nullptr, 'P'},
        {"frame", no_argument, nullptr, 'F'},
        {"no-color", no_argument, nullptr, 'C'},
        {"interval", required_argument, nullptr, 'i'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "oARFCP::i:s:u:h", long_opts, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'o': once = true; break;
        case 'A': all_counters = true; break;
        case 'R': regs = true; break;
        case 'P':
            poll = true;
            if (optarg) interval_ms = atoi(optarg);
            break;
        case 'F': frame_only = true; break;
        case 'C': no_color = true; break;
        case 'i': interval_ms = atoi(optarg); break;
        case 's': interval_ms = atoi(optarg); break;
        case 'u': break; // compatibility with the terminal fork; single interval here
        case 'h':
            printf("usage: vc4top [--once] [--all-counters] [--regs] [--poll [ms]]\n");
            printf("               [--frame] [--no-color] [--interval ms]\n\n");
            printf("  --once           wide snapshot: system, QPU and primary counters\n");
            printf("  --all-counters   all 30 counter sources (pages 2 and 3)\n");
            printf("  --regs           append the full V3D register map\n");
            printf("  --poll [ms]      CSV stream of scheduler registers\n");
            printf("  --frame          render one TUI frame and exit\n");
            printf("  --no-color       plain text (same as NO_COLOR=1)\n");
            printf("  --interval ms    sample interval, 50..10000 ms\n");
            printf("\nRegisters: root uses /dev/mem; unprivileged uses\n");
            printf("/sys/kernel/vc4mon (modprobe vc4mon). Counter sampling uses\n");
            printf("only perf_enable and perf_clear (PCTRE/PCTRC).\n");
            printf("\nKeys in the TUI: q quit, a/[] page, g regs, j/k scroll,\n");
            printf("p pause, +/- interval, h help/flags.\n");
            return 0;
        default:
            return 1;
        }
    }
    if (interval_ms < 50) interval_ms = 50;
    if (interval_ms > 10000) interval_ms = 10000;
    g_color = want_color(no_color);

    map_regs();

    if (poll)
    {
        // Ctrl-C / SIGTERM ends the CSV stream cleanly (exit 0) instead of
        // relying on the default terminate action.
        struct sigaction sa = {};
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
        poll_loop(interval_ms);
        return 0;
    }

    if (!v3d_powered())
        fprintf(stderr, "vc4top: warning: V3D reads 0xdeadbeef; start vc4cl-v3d-power\n");

    if (frame_only)
    {
        SysData sys;
        QpuData qpu;
        collect_sys(sys);
        collect_qpu(qpu);
        auto counters = sample_counters(0, interval_ms);
        int reg_scroll = 0, view_scroll = 0;
        render_tui(sys, qpu, counters, 0, false, reg_scroll, false, interval_ms, view_scroll);
        return 0;
    }

    if (once || all_counters || regs || !isatty(STDOUT_FILENO))
    {
        print_snapshot(all_counters, regs);
        return 0;
    }

    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, nullptr);

    TermState term = setup_term();
    g_interactive = true;

    int page = 0;
    bool regs_view = false, paused = false;
    int reg_scroll = 0, view_scroll = 0;
    std::vector<CounterValue> counters;
    SysSampler sampler;

    while (g_running)
    {
        QpuData qpu;
        const auto t_sys0 = std::chrono::steady_clock::now();
        sample_sys(sampler);
        collect_qpu(qpu);
        const auto t_sys1 = std::chrono::steady_clock::now();

        if (!paused && !regs_view)
            counters = sample_counters(page, interval_ms);
        const auto t_sys2 = std::chrono::steady_clock::now();
        g_sys_us += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t_sys1 - t_sys0).count());
        g_sample_us += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t_sys2 - t_sys1).count());
        if (!g_running) break;

        // A resize can arrive while we were sampling; render once with the
        // new size immediately, then redraw after the key is handled.
        if (g_winch) g_winch = 0;

        render_tui(sampler.d, qpu, counters, page, regs_view, reg_scroll, paused, interval_ms, view_scroll);

        int key = read_key();
        if (key == 'q' || key == 3) break;
        else if (key == 'a' || key == '[' || key == ']')
        {
            page = (page + 1) % 3;
            counters.clear();
        }
        else if (key == 'g')
        {
            regs_view = !regs_view;
            reg_scroll = 0;
        }
        else if (key == 'p')
        {
            paused = !paused;
        }
        else if (key == '+' || key == '=')
        {
            interval_ms = std::min(10000, interval_ms * 2);
        }
        else if (key == '-')
        {
            interval_ms = std::max(50, interval_ms / 2);
        }
        else if (key == 'h')
        {
            help_screen();
        }
        else if (key == 'j' || key == 'k')
        {
            if (regs_view)
            {
                reg_scroll += (key == 'j') ? 1 : -1;
                if (reg_scroll < 0) reg_scroll = 0;
            }
            else
            {
                view_scroll += (key == 'j') ? 1 : -1;
                if (view_scroll < 0) view_scroll = 0;
            }
        }
        if (g_winch)
        {
            g_winch = 0;
            render_tui(sampler.d, qpu, counters, page, regs_view, reg_scroll, paused, interval_ms, view_scroll);
        }
    }

    g_interactive = false;
    restore_term(term);
    if (getenv("VC4TOP_STATS") != nullptr && g_frames > 0)
        fprintf(stderr,
            "vc4top: frames=%llu render_avg=%.2f ms sys_avg=%.2f ms sample_avg=%.2f ms "
            "frame_write_calls=%.2f total_write_calls=%llu\n",
            static_cast<unsigned long long>(g_frames),
            static_cast<double>(g_render_us) / 1000.0 / static_cast<double>(g_frames),
            static_cast<double>(g_sys_us) / 1000.0 / static_cast<double>(g_frames),
            static_cast<double>(g_sample_us) / 1000.0 / static_cast<double>(g_frames),
            static_cast<double>(g_frame_write_calls) / static_cast<double>(g_frames),
            static_cast<unsigned long long>(g_write_calls));
    return 0;
}
