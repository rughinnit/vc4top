/*
 * vc4top - realtime TUI for the Broadcom VideoCore IV (V3D/QPU) block.
 *
 * Based on the original vc4top by Jonas Pfeil and the terminal fork by
 * FASTSHIFT. This version keeps the terminal-only, zero-runtime-dependency
 * design but widens the telemetry to the whole V3D block: all 30 performance
 * counter sources, the full documented register map, QPU scheduler/VPM state,
 * clocks/voltages/thermal/throttle, CMA/DRM/display state and a live TUI.
 *
 * Requires root (/dev/mem) and a powered V3D block. The vc4cl-v3d-power
 * service (or writing "on" to 3fc00000.v3d/power/control) keeps it alive.
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

volatile uint32_t* g_base = nullptr;
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_winch = 0;

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

void map_regs()
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        fprintf(stderr, "vc4top: open /dev/mem failed: %s (run as root)\n", strerror(errno));
        exit(1);
    }
    void* p = mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, VC4_BASE);
    close(fd);
    if (p == MAP_FAILED)
    {
        fprintf(stderr, "vc4top: mmap V3D failed: %s\n", strerror(errno));
        exit(1);
    }
    g_base = static_cast<volatile uint32_t*>(p);
}

inline uint32_t rd(uint32_t off) { return g_base[off / 4]; }
inline void wr(uint32_t off, uint32_t val) { g_base[off / 4] = val; }

bool v3d_powered() { return rd(V3D_IDENT0) != POWEROFF_VALUE; }

void collect_sys(SysData& d)
{
    d.time = cmd("date '+%Y-%m-%d %H:%M:%S %Z'");
    d.temp = after_eq(cmd("vcgencmd measure_temp"));
    d.throttle = after_eq(cmd("vcgencmd get_throttled"));
    {
        uint32_t v = static_cast<uint32_t>(strtoul(d.throttle.c_str(), nullptr, 16));
        const struct { int bit; const char* name; } bits[] = {
            {0, "under-voltage"}, {1, "freq-cap"}, {2, "throttled"}, {3, "temp-limit"},
            {16, "uv-past"}, {17, "freq-cap-past"}, {18, "thr-past"}, {19, "temp-past"}};
        std::string s;
        for (const auto& b : bits)
            if (v & (1u << b.bit)) { if (!s.empty()) s += ","; s += b.name; }
        d.throttle_decoded = s.empty() ? "none" : s;
    }
    d.volts_core = after_eq(cmd("vcgencmd measure_volts core"));
    d.volts_sdram = after_eq(cmd("vcgencmd measure_volts sdram_c"));
    d.arm_clk = after_eq(cmd("vcgencmd measure_clock arm"));
    d.core_clk = after_eq(cmd("vcgencmd measure_clock core"));
    d.v3d_clk = after_eq(cmd("vcgencmd measure_clock v3d"));
    d.h264_clk = after_eq(cmd("vcgencmd measure_clock h264"));
    d.isp_clk = after_eq(cmd("vcgencmd measure_clock isp"));
    d.pixel_clk = after_eq(cmd("vcgencmd measure_clock pixel"));
    d.hdmi_clk = after_eq(cmd("vcgencmd measure_clock hdmi"));
    d.vec_clk = after_eq(cmd("vcgencmd measure_clock vec"));
    d.emmc_clk = after_eq(cmd("vcgencmd measure_clock emmc"));
    d.gpu_mem = after_eq(cmd("vcgencmd get_mem gpu"));
    d.arm_mem = after_eq(cmd("vcgencmd get_mem arm"));
    {
        std::string total = cmd("awk '/^CmaTotal:/{print $2}' /proc/meminfo");
        std::string freeb = cmd("awk '/^CmaFree:/{print $2}' /proc/meminfo");
        if (total.empty() || freeb.empty())
            d.cma = "n/a";
        else
            d.cma = "CMA " + std::to_string(atol(total.c_str()) / 1024) + "M/" +
                std::to_string(atol(freeb.c_str()) / 1024) + "M free";
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

void collect_qpu(QpuData& q)
{
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

std::vector<CounterValue> sample_counters(int page, int interval_ms)
{
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

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

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

std::string make_bar(const std::string& label, double pct, int width)
{
    pct = std::max(0.0, std::min(100.0, pct));
    int filled = static_cast<int>(pct / 100.0 * width + 0.5);
    std::string bar = "[";
    bar += std::string(filled, '#');
    bar += std::string(std::max(0, width - filled), '.');
    bar += "]";
    const char* c = K_GREEN;
    if (pct >= 80.0) c = K_RED;
    else if (pct >= 50.0) c = K_YELLOW;
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), " %5.1f%%", pct);
    return label + " " + colorize(c, bar) + pbuf;
}

std::vector<std::string> utilization_lines(const std::vector<CounterValue>& counters, int width)
{
    static const char* labels[7] = {"Idle", "Vertex", "Fragment", "Valid", "Stall TMU", "Stall SB", "Stall Vary"};
    double c[7];
    double total = 0;
    utilization_values(counters, c, total);
    std::vector<std::string> b;
    const bool narrow = width < 96;
    const int barw = narrow ? 14 : std::max(16, std::min(30, width - 28));
    if (narrow)
    {
        std::vector<std::string> chunks;
        for (int i = 0; i < 7; ++i)
        {
            char pct[16];
            snprintf(pct, sizeof(pct), "%.1f%%", c[i] / total * 100.0);
            chunks.push_back(std::string(labels[i]) + " " + pct);
        }
        // pack percentage summary first, then small bars two per line
        std::string line;
        for (const auto& ch : chunks)
        {
            if (!line.empty() && visible_len(line) + 3 + visible_len(ch) > static_cast<size_t>(width))
            {
                b.push_back(line);
                line.clear();
            }
            if (!line.empty()) line += "  ";
            line += ch;
        }
        if (!line.empty()) b.push_back(line);
        std::string bars;
        for (int i = 0; i < 7; ++i)
        {
            std::string one = make_bar(labels[i], c[i] / total * 100.0, barw);
            if (!bars.empty() && visible_len(bars) + 3 + visible_len(one) > static_cast<size_t>(width))
            {
                b.push_back(bars);
                bars.clear();
            }
            if (!bars.empty()) bars += "  ";
            bars += one;
        }
        if (!bars.empty()) b.push_back(bars);
    }
    else
    {
        for (int i = 0; i < 7; ++i)
            b.push_back(make_bar(labels[i], c[i] / total * 100.0, barw));
    }
    for (auto& l : b) l = clip_vis(l, static_cast<size_t>(width));
    return b;
}

std::string page_title(int page)
{
    if (page == 0) return "page 1/3  QPU/GPU primary (13-28)";
    if (page == 1) return "page 2/3  FEP/TLB/PTB/PSE (0-15)";
    return "page 3/3  QPU/TMU/VPM/L2C (16-29)";
}

std::vector<std::string> counter_lines(const std::vector<CounterValue>& counters, int page, int width)
{
    std::vector<std::string> b;
    std::string title = page_title(page) + "  counts";
    if (visible_len(title) > static_cast<size_t>(width))
        title = page_title(page);
    if (visible_len(title) > static_cast<size_t>(width))
        title = "page " + std::to_string(page + 1) + "/3";
    b.push_back(colorize(K_DIM, clip_vis(title, static_cast<size_t>(width))));
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
    if (page != 0)
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

void write_frame(const std::string& frame)
{
    fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
}

std::vector<std::string> legend_lines()
{
    return {
        colorize(K_GREEN, "green") + " ok  " + colorize(K_YELLOW, "yellow") + " warn  " +
            colorize(K_RED, "red") + " error",
        colorize(K_DIM, "dim") + " idle/zero",
        "keys  q quit  a/[] page",
        "      g regs  j/k scroll",
        "      p pause  +/- int  h help",
        "flags --once  --all-counters",
        "      --regs  --poll [ms]",
        "      --frame  --no-color",
        "plain: NO_COLOR=1 or piped",
        "30 counter sources / 3 pages",
        "rate = counts/s over interval",
        "QPU counters are 3D-oriented;",
        "raw compute may leave idle/",
        "valid at zero.",
        "Use --poll for scheduler,",
        "--once/--regs for dumps."};
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

std::vector<std::string> wide_content(const SysData& sys, const QpuData& qpu,
    const std::vector<CounterValue>& counters, int page, int cols)
{
    std::vector<std::string> out;
    if (cols >= 136)
    {
        const int w1 = cols * 35 / 100;
        const int w2 = cols * 28 / 100;
        const int w3 = cols - w1 - w2 - 2;
        std::vector<std::string> c1;
        append(c1, box("SYSTEM / POWER", system_lines(sys, w1 - 4), w1));
        append(c1, box("UTILIZATION", utilization_lines(counters, w1 - 4), w1));
        std::vector<std::string> c2;
        append(c2, box("QPU / QUEUES / VPM", qpu_lines(qpu, w2 - 4), w2));
        std::vector<std::string> c3;
        append(c3, box("COUNTERS", counter_lines(counters, page, w3 - 4), w3));
        append(c3, box("LEGEND / HELP", legend_lines(), w3));
        compose_three(c1, c2, c3, w1, w2, out);
        return out;
    }

    const int left_w = std::max(46, std::min(68, cols * 46 / 100));
    const int right_w = cols - left_w - 1;
    std::vector<std::string> left;
    append(left, box("SYSTEM / POWER", system_lines(sys, left_w - 4), left_w));
    append(left, box("QPU / QUEUES / VPM", qpu_lines(qpu, left_w - 4), left_w));
    append(left, box("UTILIZATION", utilization_lines(counters, left_w - 4), left_w));
    std::vector<std::string> right;
    append(right, box("COUNTERS", counter_lines(counters, page, right_w - 4), right_w));
    append(right, box("LEGEND / HELP", legend_lines(), right_w));
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
    if (roomy) out.push_back(section_line("QPU / QUEUES / VPM", cols));
    for (const auto& l : qpu_lines(qpu, w)) out.push_back(" " + l);
    out.push_back(section_line("UTILIZATION", cols));
    for (const auto& l : utilization_lines(counters, w)) out.push_back(" " + l);
    out.push_back(section_line("COUNTERS", cols));
    for (const auto& l : counter_lines(counters, page, w)) out.push_back(" " + l);
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

std::string build_footer(bool regs_view, int first, int last, int total, int cols)
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
    return pad_visible(clip_vis(colorize(K_DIM, f), static_cast<size_t>(cols)), static_cast<size_t>(cols));
}

void render_tui(const SysData& sys, const QpuData& qpu, const std::vector<CounterValue>& counters,
    int page, bool regs_view, int& reg_scroll, bool paused, int interval_ms, int& view_scroll)
{
    int rows = 40, cols = 120;
    get_terminal_size(rows, cols);
    if (rows < 10) rows = 10;
    if (cols < 60) cols = 60;

    const bool tty = isatty(STDOUT_FILENO) != 0;
    const int avail = std::max(1, rows - 2);
    std::vector<std::string> content;
    int first = 0, last = 0, total = 0;

    if (regs_view)
    {
        auto all = register_lines(cols - 4);
        total = static_cast<int>(all.size());
        const int inner = std::max(1, rows - 4);
        const int max_scroll = std::max(0, total - inner);
        if (reg_scroll > max_scroll) reg_scroll = max_scroll;
        if (reg_scroll < 0) reg_scroll = 0;
        const int end = std::min(total, reg_scroll + inner);
        std::vector<std::string> slice(all.begin() + reg_scroll, all.begin() + end);
        char title[64];
        snprintf(title, sizeof(title), "V3D REGISTERS  %d-%d of %d", reg_scroll + 1, end, total);
        content = box(title, slice, cols);
        first = reg_scroll + 1;
        last = end;
    }
    else
    {
        content = (cols >= 100)
            ? wide_content(sys, qpu, counters, page, cols)
            : narrow_content(sys, qpu, counters, page, cols, rows);
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
    out.push_back(build_header(sys, qpu, paused, interval_ms, cols));
    for (const auto& l : content) out.push_back(pad_visible(clip_vis(l, static_cast<size_t>(cols)), static_cast<size_t>(cols)));
    out.push_back(build_footer(regs_view, first, last, total, cols));

    while (static_cast<int>(out.size()) < rows) out.push_back(std::string(cols, ' '));
    if (static_cast<int>(out.size()) > rows) out.resize(rows);

    std::string frame;
    if (tty) frame = "\033[H";
    for (const auto& l : out) frame += pad_visible(l, static_cast<size_t>(cols)) + (tty ? "\033[K\n" : "\n");
    write_frame(frame);
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

    if (all_counters)
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
    fputs("\033[?1049h\033[?25l", stdout);
    fflush(stdout);
    st.alt = true;
    return st;
}

void restore_term(const TermState& st)
{
    if (st.alt)
    {
        fputs("\033[?25h\033[?1049l", stdout);
        fflush(stdout);
    }
    if (st.valid) tcsetattr(STDIN_FILENO, TCSANOW, &st.orig);
}

int read_key()
{
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return 0;
}

void on_signal(int) { g_running = 0; }
void on_winch(int) { g_winch = 1; }

void help_screen()
{
    int rows = 40, cols = 120;
    get_terminal_size(rows, cols);
    std::vector<std::string> body = {
        "KEYS",
        "  q          quit",
        "  a / [ ]    cycle counter pages (13-28, 0-15, 16-29)",
        "  g          toggle full V3D register dump",
        "  j / k      scroll registers (g) or the live view",
        "  p          pause/resume sampling",
        "  + / -      increase/decrease sample interval",
        "  h          this help",
        "",
        "FLAGS",
        "  --once            wide snapshot, primary counters",
        "  --all-counters    all 30 sources (pages 2/3)",
        "  --regs            full V3D register map",
        "  --poll [ms]       CSV scheduler stream",
        "  --frame           render one TUI frame and exit",
        "  --no-color        disable ANSI colour",
        "  NO_COLOR=1        environment equivalent (also piped)",
        "",
        "Values: hex for registers, MHz for clocks, \xC2\xB0\x43 for temperature,",
        "counts and counts/s for counters, % for utilization.",
        "30 counter sources are spread over three pages.",
        "",
        "Any key returns."
    };
    int w = std::min(cols - 2, 72);
    auto boxed = box("VC4TOP HELP", body, w);
    std::string frame = "\033[H\033[2J";
    int start = std::max(0, (rows - static_cast<int>(boxed.size())) / 2);
    for (int i = 0; i < start; ++i) frame += "\n";
    for (const auto& l : boxed) frame += l + "\n";
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
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, nullptr);

    TermState term = setup_term();

    int page = 0;
    bool regs_view = false, paused = false;
    int reg_scroll = 0, view_scroll = 0;
    std::vector<CounterValue> counters;

    while (g_running)
    {
        SysData sys;
        QpuData qpu;
        collect_sys(sys);
        collect_qpu(qpu);

        if (!paused && !regs_view)
            counters = sample_counters(page, interval_ms);

        render_tui(sys, qpu, counters, page, regs_view, reg_scroll, paused, interval_ms, view_scroll);

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
        if (key == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    restore_term(term);
    return 0;
}
