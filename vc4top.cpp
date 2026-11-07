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
const char* K_CYAN = "\033[36m";
const char* K_GREEN = "\033[32m";
const char* K_YELLOW = "\033[33m";
const char* K_RED = "\033[31m";
const char* K_MAGENTA = "\033[35m";

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

size_t visible_len(const std::string& s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == '\033')
        {
            ++i;
            if (i < s.size() && s[i] == '[')
            {
                ++i;
                while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                if (i < s.size()) ++i;
            }
            continue;
        }
        ++n;
        ++i;
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
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[')
            {
                ++j;
                while (j < s.size() && !std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
                if (j < s.size()) ++j;
            }
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        out.push_back(s[i]);
        ++vis;
        ++i;
    }
    if (g_color) out += K_RESET;
    return out;
}

std::string colorize(const char* color, const std::string& s)
{
    if (!g_color) return s;
    return std::string(color) + s + K_RESET;
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
        std::string content = b;
        if (content.find('\033') == std::string::npos)
            content = truncate_plain(content, static_cast<size_t>(std::max(4, width - 4)));
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
    return std::string(label) + " " + colorize(c, bar) + pbuf;
}

std::vector<std::string> system_panel(const SysData& s, int width)
{
    std::vector<std::string> b;
    b.push_back("time     " + truncate_plain(s.time, width - 12));
    b.push_back("temp     " + s.temp + "   throttle " + s.throttle + " (" + s.throttle_decoded + ")");
    b.push_back("clocks   arm " + s.arm_clk + "  core " + s.core_clk + "  v3d " + s.v3d_clk);
    b.push_back("codecs   h264 " + s.h264_clk + "  isp " + s.isp_clk + "  pixel " + s.pixel_clk);
    b.push_back("display  hdmi " + s.hdmi_clk + "  vec " + s.vec_clk + "  emmc " + s.emmc_clk);
    b.push_back("volts    core " + s.volts_core + "  sdram_c " + s.volts_sdram);
    b.push_back("memory   gpu " + s.gpu_mem + "  arm " + s.arm_mem + "  " + truncate_plain(s.cma, width - 20));
    b.push_back("v3d      " + s.v3d_state + " / " + s.v3d_control + "  undervolt " + s.undervolt);
    b.push_back("drm      " + truncate_plain(s.drm, width - 12));
    b.push_back("hdmi     " + s.hdmi_status + " " + truncate_plain(s.hdmi_mode, width - 20));
    b.push_back("hvs      underrun " + s.hvs_underrun + "  bo " +
        (s.bo_stats.empty() ? std::string("none") : truncate_plain(s.bo_stats, width - 26)));
    b.push_back("clients  " + s.clients);
    return b;
}

std::vector<std::string> qpu_panel(const QpuData& q, int width)
{
    (void)width; // lines are clipped by box()
    std::vector<std::string> b;
    char buf[256];
    if (!q.powered)
    {
        b.push_back(colorize(K_RED, "V3D POWERED OFF (0xdeadbeef)"));
        b.push_back("start vc4cl-v3d-power or write on to power/control");
        return b;
    }
    snprintf(buf, sizeof(buf), "ident    %d sl x %d qpu = %d qpu / %d tmu / %d sem",
        q.slices, q.qpus_per_slice, q.qpus, q.tmus, q.semaphores);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "config   vpm %dK  hdr %d  rev %d", q.vpm_kb, q.hdr, q.revision);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "ident0   0x%08x  ident1 0x%08x", q.ident0, q.ident1);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "srqcs    0x%08x  queue %u  req %u  done %u  qerr %u",
        q.srqcs, q.srqcs & 0x3F, (q.srqcs >> 8) & 0xFF, (q.srqcs >> 16) & 0xFF, (q.srqcs >> 7) & 1);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "vpmbase  0x%08x  reserved %u B of %dK  vpacntl 0x%08x",
        q.vpmbase, (q.vpmbase & 0x1F) * 256, q.vpm_kb, q.vpacntl);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "sqrsv    0x%08x / 0x%08x", q.sqrsv0, q.sqrsv1);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "srqpc    0x%08x  srqua 0x%08x", q.srqpc, q.srqua);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "srqul    0x%08x", q.srqul);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "intctl   0x%08x  intena 0x%08x", q.intctl, q.intena);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "intdis   0x%08x  pctre 0x%08x", q.intdis, q.pctre);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "pctrc    0x%08x  l2c 0x%08x", q.pctrc, q.l2cactl);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "slc      0x%08x", q.slcactl);
    b.push_back(buf);
    snprintf(buf, sizeof(buf), "errstat  0x%08x  %s", q.errstat, errstat_string(q.errstat).c_str());
    b.push_back(buf);
    return b;
}

std::vector<std::string> utilization_panel(const std::vector<CounterValue>& counters, int width)
{
    double c[7] = {0, 0, 0, 0, 0, 0, 0};
    for (const auto& v : counters)
    {
        if (v.source >= 13 && v.source <= 19 && !v.powered_off)
            c[v.source - 13] += v.value;
    }
    double total = c[0] + c[1] + c[2] + c[3] + c[4] + c[5] + c[6];
    if (total <= 0) { c[0] = 1; total = 1; }
    const char* labels[7] = {"Idle", "Vertex", "Fragment", "Valid", "Stall TMU", "Stall SB", "Stall Vary"};
    int barw = std::max(10, std::min(30, width - 24));
    std::vector<std::string> b;
    for (int i = 0; i < 7; ++i)
        b.push_back(make_bar(labels[i], c[i] / total * 100.0, barw));
    return b;
}

std::vector<std::string> counters_panel(const std::vector<CounterValue>& counters, int page,
    int width, int rows)
{
    std::vector<std::string> b;
    char buf[256];
    snprintf(buf, sizeof(buf), "page %d/3   %s", page + 1,
        page == 0 ? "QPU/GPU primary (13-28)" : (page == 1 ? "FEP/TLB/PTB/PSE (0-15)" : "QPU/TMU/VPM/L2C (16-29)"));
    b.push_back(colorize(K_DIM, buf));
    const int name_w = std::max(12, width - 38);
    snprintf(buf, sizeof(buf), "%-3s %-*s %12s %12s", "ID", name_w, "NAME", "VALUE", "RATE/s");
    b.push_back(colorize(K_DIM, truncate_plain(buf, static_cast<size_t>(width - 4))));
    for (const auto& v : counters)
    {
        if (static_cast<int>(b.size()) >= rows) break;
        std::string name = truncate_plain(counter_name(v.source), static_cast<size_t>(name_w));
        if (v.powered_off)
            snprintf(buf, sizeof(buf), "%-3d %-*s %12s", v.source, name_w, name.c_str(), "POWERED");
        else
            snprintf(buf, sizeof(buf), "%-3d %-*s %12u %12.1f", v.source, name_w, name.c_str(), v.value, v.rate);
        b.push_back(buf);
    }
    if (page != 0)
        b.push_back(colorize(K_DIM, "counter page changed; zero counts are expected for the first sample"));
    return b;
}

std::vector<std::string> register_panel(int scroll, int width, int rows)
{
    std::vector<std::string> b;
    const int total = static_cast<int>(sizeof(registers) / sizeof(registers[0]));
    char buf[256];
    snprintf(buf, sizeof(buf), "registers %d..%d of %d   j/k scroll, g return",
        std::min(scroll, total), std::min(scroll + rows - 2, total), total);
    b.push_back(colorize(K_DIM, buf));
    for (int i = scroll; i < total && static_cast<int>(b.size()) < rows; ++i)
    {
        snprintf(buf, sizeof(buf), "%-12s 0x%08x  %s", registers[i].name, rd(registers[i].offset),
            truncate_plain(registers[i].desc, static_cast<size_t>(std::max(10, width - 32))).c_str());
        b.push_back(buf);
    }
    return b;
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
    }
}

void write_frame(const std::string& frame)
{
    fwrite(frame.data(), 1, frame.size(), stdout);
    fflush(stdout);
}

void render_tui(const SysData& sys, const QpuData& qpu, const std::vector<CounterValue>& counters,
    int page, bool regs_view, int reg_scroll, bool paused)
{
    int rows = 40, cols = 120;
    get_terminal_size(rows, cols);
    if (rows < 12) rows = 12;
    if (cols < 60) cols = 60;

    std::vector<std::string> out;
    char buf[512];

    std::string title = std::string(K_BOLD) + "VC4TOP" + K_RESET + "  " + sys.time + "  " +
        colorize(K_YELLOW, sys.temp) + "  thr " + sys.throttle + " (" + sys.throttle_decoded + ")  " +
        (qpu.powered ? colorize(K_GREEN, "V3D ON") : colorize(K_RED, "V3D OFF")) +
        (paused ? colorize(K_MAGENTA, "  PAUSED") : "");
    out.push_back(pad_visible(truncate_visible(title, static_cast<size_t>(cols)), static_cast<size_t>(cols)));

    const int content_rows = rows - 2;
    if (regs_view)
    {
        auto body = register_panel(reg_scroll, cols - 4, content_rows - 2);
        auto boxed = box("V3D REGISTERS", body, cols);
        for (const auto& l : boxed) if (static_cast<int>(out.size()) < rows - 1) out.push_back(l);
    }
    else if (cols >= 100)
    {
        const int left_w = std::max(60, cols * 50 / 100);
        const int right_w = cols - left_w - 1;
        auto sys_box = box("SYSTEM / POWER", system_panel(sys, left_w - 4), left_w);
        auto qpu_box = box("QPU / SCHEDULER / VPM", qpu_panel(qpu, left_w - 4), left_w);
        auto util_box = box("UTILIZATION", utilization_panel(counters, left_w - 4), left_w);
        std::vector<std::string> left;
        left.insert(left.end(), sys_box.begin(), sys_box.end());
        left.insert(left.end(), qpu_box.begin(), qpu_box.end());
        left.insert(left.end(), util_box.begin(), util_box.end());

        const int crows = std::max(6, content_rows - 3);
        auto cnt_box = box("COUNTERS", counters_panel(counters, page, right_w - 4, crows), right_w);
        std::vector<std::string> right = cnt_box;
        std::vector<std::string> notes = {
            "counter pages",
            "  a / [ / ]  page 1: 13-28 primary",
            "              page 2: 0-15 FEP/TLB/PTB/PSE",
            "              page 3: 16-29 QPU/TMU/VPM/L2C",
            "",
            "V3D counters are 3D-pipeline oriented.",
            "Raw QPU compute may leave idle/valid at 0.",
            "Use --poll for SRQCS/SRQPC activity and",
            "--once / --regs for the full wide dump.",
        };
        auto notes_box = box("NOTES", notes, right_w);
        right.insert(right.end(), notes_box.begin(), notes_box.end());
        std::vector<std::string> composed;
        compose_columns(left, right, left_w, 1, composed);
        for (const auto& l : composed) if (static_cast<int>(out.size()) < rows - 1) out.push_back(l);
    }
    else
    {
        auto sys_box = box("SYSTEM", system_panel(sys, cols - 4), cols);
        auto qpu_box = box("QPU", qpu_panel(qpu, cols - 4), cols);
        auto util_box = box("UTILIZATION", utilization_panel(counters, cols - 4), cols);
        auto cnt_box = box("COUNTERS", counters_panel(counters, page, cols - 4, 10), cols);
        for (const auto* v : {&sys_box, &qpu_box, &util_box, &cnt_box})
            for (const auto& l : *v) if (static_cast<int>(out.size()) < rows - 1) out.push_back(l);
    }

    snprintf(buf, sizeof(buf), "q quit  a/[ ] counters  g registers  p pause  +/- interval  h help  %s",
        regs_view ? "j/k scroll" : "");
    out.push_back(pad_visible(truncate_visible(colorize(K_DIM, buf), static_cast<size_t>(cols)), static_cast<size_t>(cols)));

    while (static_cast<int>(out.size()) < rows) out.push_back(std::string(cols, ' '));
    if (static_cast<int>(out.size()) > rows) out.resize(rows);

    std::string frame = "\033[H";
    for (const auto& l : out) frame += pad_visible(l, static_cast<size_t>(cols)) + "\033[K\n";
    write_frame(frame);
}

void print_snapshot(bool all_counters, bool regs)
{
    SysData sys;
    QpuData qpu;
    collect_sys(sys);
    collect_qpu(qpu);

    printf("============================================================\n");
    printf(" VC4TOP - VideoCore IV / V3D wide snapshot\n");
    printf("============================================================\n");
    printf("[ SYSTEM / POWER ]\n");
    printf("time:           %s\n", sys.time.c_str());
    printf("temp:           %s\n", sys.temp.c_str());
    printf("throttled:      %s (%s)\n", sys.throttle.c_str(), sys.throttle_decoded.c_str());
    printf("core volts:     %s\n", sys.volts_core.c_str());
    printf("sdram_c volts:  %s\n", sys.volts_sdram.c_str());
    printf("arm clock:      %s Hz\n", sys.arm_clk.c_str());
    printf("core clock:     %s Hz\n", sys.core_clk.c_str());
    printf("v3d clock:      %s Hz\n", sys.v3d_clk.c_str());
    printf("h264 clock:     %s Hz\n", sys.h264_clk.c_str());
    printf("isp clock:      %s Hz\n", sys.isp_clk.c_str());
    printf("pixel clock:    %s Hz\n", sys.pixel_clk.c_str());
    printf("hdmi clock:     %s Hz\n", sys.hdmi_clk.c_str());
    printf("vec clock:      %s Hz\n", sys.vec_clk.c_str());
    printf("emmc clock:     %s Hz\n", sys.emmc_clk.c_str());
    printf("gpu mem:        %s\n", sys.gpu_mem.c_str());
    printf("arm mem:        %s\n", sys.arm_mem.c_str());
    printf("cma:            %s\n", sys.cma.c_str());
    printf("v3d power:      %s / %s\n", sys.v3d_state.c_str(), sys.v3d_control.c_str());
    printf("undervoltage:   %s\n", sys.undervolt.c_str());
    printf("hvs underrun:   %s\n", sys.hvs_underrun.c_str());
    printf("drm:            %s\n", sys.drm.c_str());
    printf("hdmi:           %s %s\n", sys.hdmi_status.c_str(), sys.hdmi_mode.c_str());
    printf("bo_stats:       %s\n", sys.bo_stats.c_str());
    printf("\n[ QPU / SCHEDULER ]\n");
    printf("IDENT0:         0x%08x\n", qpu.ident0);
    printf("IDENT1:         0x%08x\n", qpu.ident1);
    printf("IDENT2:         0x%08x\n", qpu.ident2);
    printf("revision:       %d\n", qpu.revision);
    printf("slices:         %d\n", qpu.slices);
    printf("QPUs/slice:     %d (total %d)\n", qpu.qpus_per_slice, qpu.qpus);
    printf("TMUs/slice:     %d (total %d)\n", qpu.tmus_per_slice, qpu.tmus);
    printf("semaphores:     %d\n", qpu.semaphores);
    printf("VPM size:       %dK\n", qpu.vpm_kb);
    printf("HDR support:    %d\n", qpu.hdr);
    printf("SRQCS:          0x%08x queue=%u requests=%u completed=%u qerr=%u\n",
        qpu.srqcs, qpu.srqcs & 0x3F, (qpu.srqcs >> 8) & 0xFF, (qpu.srqcs >> 16) & 0xFF, (qpu.srqcs >> 7) & 1);
    printf("VPMBASE:        0x%08x reserved=%u bytes of %dK\n",
        qpu.vpmbase, (qpu.vpmbase & 0x1F) * 256, qpu.vpm_kb);
    printf("VPACNTL:        0x%08x\n", qpu.vpacntl);
    printf("SQRSV0/1:       0x%08x / 0x%08x\n", qpu.sqrsv0, qpu.sqrsv1);
    printf("SQCNTL:         0x%08x\n", qpu.sqcntl);
    printf("SRQPC/SRQUA:    0x%08x / 0x%08x (len 0x%08x)\n", qpu.srqpc, qpu.srqua, qpu.srqul);
    printf("INTCTL/ENA/DIS: 0x%08x / 0x%08x / 0x%08x\n", qpu.intctl, qpu.intena, qpu.intdis);
    printf("PCTRE/PCTRC:    0x%08x / 0x%08x\n", qpu.pctre, qpu.pctrc);
    printf("L2CACTL/SLCACTL:0x%08x / 0x%08x\n", qpu.l2cactl, qpu.slcactl);
    printf("ERRSTAT:        0x%08x %s\n", qpu.errstat, errstat_string(qpu.errstat).c_str());
    printf("\n");

    if (all_counters)
    {
        for (int page = 1; page <= 2; ++page)
        {
            auto c = sample_counters(page, DEFAULT_INTERVAL_MS);
            printf("[ COUNTERS page %d/3 ]\n", page + 1);
            for (const auto& v : c)
                printf("  %2d %-70s %10u %12.1f /s\n", v.source, counter_name(v.source), v.value, v.rate);
            printf("\n");
        }
    }
    else
    {
        auto c = sample_counters(0, DEFAULT_INTERVAL_MS);
        printf("[ PRIMARY COUNTERS ]\n");
        for (const auto& v : c)
            printf("  %2d %-70s %10u %12.1f /s\n", v.source, counter_name(v.source), v.value, v.rate);
        printf("\n");
    }

    if (regs)
    {
        printf("[ V3D REGISTER DUMP ]\n");
        const int n = static_cast<int>(sizeof(registers) / sizeof(registers[0]));
        for (int i = 0; i < n; ++i)
            printf("  %-12s 0x%08x  %s\n", registers[i].name, rd(registers[i].offset), registers[i].desc);
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
        "q          quit",
        "a / [ ]    cycle counter pages (13-28, 0-15, 16-29)",
        "g          toggle full V3D register dump",
        "j / k      scroll the register dump",
        "p          pause/resume sampling",
        "+ / -      increase/decrease sample interval",
        "h          this help",
        "",
        "Width: all 30 counter sources are available on three pages.",
        "The 16 V3D counters are 3D-pipeline oriented; raw QPU compute",
        "may leave QPU idle/valid counters at zero. Use --poll for the",
        "scheduler registers and --once/--regs for wide snapshots.",
        "",
        "Requires root (/dev/mem) and a powered V3D block.",
        "Any key returns."
    };
    int w = std::min(cols - 2, 70);
    auto boxed = box("VC4TOP HELP", body, w);
    std::string frame = "\033[H\033[2J";
    int start = std::max(0, (rows - static_cast<int>(boxed.size())) / 2);
    for (int i = 0; i < start; ++i) frame += "\n";
    for (const auto& l : boxed) frame += l + "\n";
    write_frame(frame);
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) != 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
            printf("               [--frame] [--no-color] [--interval ms]\n");
            printf("       vc4top                         realtime TUI\n");
            return 0;
        default:
            return 1;
        }
    }
    if (interval_ms < 50) interval_ms = 50;
    if (interval_ms > 10000) interval_ms = 10000;
    g_color = !no_color;

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
        render_tui(sys, qpu, counters, 0, false, 0, false);
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
    int reg_scroll = 0;
    std::vector<CounterValue> counters;

    while (g_running)
    {
        SysData sys;
        QpuData qpu;
        collect_sys(sys);
        collect_qpu(qpu);

        if (!paused && !regs_view)
        {
            counters = sample_counters(page, interval_ms);
        }
        else if (!paused && regs_view)
        {
            // no counter sampling while inspecting registers
        }

        if (!paused || counters.empty())
        {
            render_tui(sys, qpu, counters, page, regs_view, reg_scroll, paused);
        }
        else
        {
            render_tui(sys, qpu, counters, page, regs_view, reg_scroll, paused);
        }

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
        else if (regs_view && (key == 'j' || key == 'k'))
        {
            reg_scroll += (key == 'j') ? 1 : -1;
            if (reg_scroll < 0) reg_scroll = 0;
        }
        if (key == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    restore_term(term);
    return 0;
}
