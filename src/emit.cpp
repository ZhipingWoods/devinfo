/* emit.cpp - JSON and human renderers.
 *
 * Any value that did not come straight off the hardware is marked. In JSON it
 * lands in an "estimated": [...] list; in the table it gets a trailing "~".
 */
#include "devinfo_core.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace devinfo {
namespace {

/* -------------------------------------------------------- json writing */
void jesc(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  o += '"';
}

std::string jnum(double v) {
  char b[40];
  if (v == (double)(long long)v && v < 1e15 && v > -1e15)
    std::snprintf(b, sizeof b, "%lld", (long long)v);
  else
    std::snprintf(b, sizeof b, "%.4g", v);
  return b;
}

/* ------------------------------------------------------- table helpers */
/* Capacities are conventionally binary; throughput stays decimal. */
std::string fmt_bytes(double b) {
  char buf[48];
  if (b >= 1024.0 * 1024 * 1024)      std::snprintf(buf, sizeof buf, "%.1f GiB", b / (1024.0 * 1024 * 1024));
  else if (b >= 1024.0 * 1024)        std::snprintf(buf, sizeof buf, "%.1f MiB", b / (1024.0 * 1024));
  else if (b >= 1024.0)               std::snprintf(buf, sizeof buf, "%.0f KiB", b / 1024.0);
  else                                std::snprintf(buf, sizeof buf, "%.0f B", b);
  return buf;
}

std::string fmt_count(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld", (long long)(v + 0.5));
  return buf;
}

std::string fmt_num(double v, const char* unit = "") {
  char buf[64];
  if (v >= 100)      std::snprintf(buf, sizeof buf, "%.1f%s", v, unit);
  else if (v >= 10)  std::snprintf(buf, sizeof buf, "%.2f%s", v, unit);
  else               std::snprintf(buf, sizeof buf, "%.3f%s", v, unit);
  return buf;
}

/* Only numbers that actually came from the builtin table get the marker. */
const char* mark(const Num& n) { return n.estimated ? "~" : ""; }

std::string pad(const std::string& s, size_t w) {
  std::string o = s;
  if (o.size() >= w) return o + ' ';   /* never lose the separator */
  while (o.size() < w) o += ' ';
  return o;
}

}  /* anonymous namespace */

/* ============================================================== JSON */
/* The cursor owns comma placement, so a field can be skipped for being
 * absent without ever leaving a dangling separator behind. */
namespace {
struct W {
  std::string& o;
  bool first = true;
  explicit W(std::string& s) : o(s) {}

  void sep() { if (!first) o += ','; first = false; }
  void key(const char* k) { sep(); jesc(o, k); o += ':'; }
  void raw(const char* k, const std::string& v) { key(k); o += v; }
  void boolean(const char* k, bool v) { key(k); o += v ? "true" : "false"; }

  void str(const char* k, const std::string& v) {
    if (v.empty()) return;
    key(k);
    jesc(o, v);
  }
  void num(const char* k, const Num& n, std::vector<std::string>& est, const std::string& path) {
    if (!n.has) return;
    key(k);
    o += jnum(n.v);
    if (n.estimated) est.push_back(path.empty() ? std::string(k) : path + "." + k);
  }
  void u64(const char* k, double v) { key(k); o += jnum(v); }
  void u32(const char* k, unsigned v) { key(k); o += jnum((double)v); }
};

void strlist(std::string& o, const std::vector<std::string>& items) {
  o += '[';
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) o += ',';
    jesc(o, items[i]);
  }
  o += ']';
}
}  /* anonymous namespace */

std::string to_json(const Report& r) {
  std::string o;
  o.reserve(4096);
  o += '{';

  {
    W w(o);
    w.raw("schema", "\"devinfo/1\"");
    w.u32("abi", 1);
    w.raw("level", "\"" + r.level + "\"");
    w.raw("probe_ms", jnum(r.probe_ms));
  }

  /* ---- host ---- */
  o += ",\"host\":{";
  {
    W h(o);
    h.str("os", r.os);
    h.str("os_version", r.os_version);
    h.str("kernel", r.kernel);
    h.str("arch", r.arch);
    h.str("model", r.model_id);
    h.str("env", r.env);
    h.str("hostname", r.host);
  }
  o += '}';

  /* ---- cpu ---- */
  o += ",\"cpu\":{";
  {
    const Cpu& c = r.cpu;
    std::vector<std::string> est;
    W x(o);
    x.str("vendor", c.vendor);
    x.str("model", c.model);
    x.str("arch", c.arch);

    o += ",\"cores\":{";
    {
      W cc(o);
      cc.u32("physical", c.physical);
      cc.u32("logical", c.logical);
      if (c.p_cores) cc.u32("p", c.p_cores);
      if (c.e_cores) cc.u32("e", c.e_cores);
      if (!c.p_name.empty()) cc.str("p_tier", c.p_name);
      if (!c.e_name.empty()) cc.str("e_tier", c.e_name);
    }
    o += '}';

    x.num("clock_ghz", c.clock_ghz, est, "");

    o += ",\"cache_bytes\":{";
    {
      W ck(o);
      ck.u32("l1i", c.l1i);
      ck.u32("l1d", c.l1d);
      ck.u32("l2", c.l2);
      ck.u32("l3", c.l3);
      ck.u32("line", c.line);
    }
    o += '}';

    o += ",\"memory\":{";
    {
      W m(o);
      if (c.mem_bytes.has) m.u64("total_bytes", c.mem_bytes.v);
      m.num("bandwidth_gbs", c.mem_bw_gbs, est, "memory");
      m.num("bandwidth_measured_gbs", c.mem_bw_measured_gbs, est, "memory");
    }
    o += '}';

    o += ",\"peak\":{";
    {
      W p(o);
      p.num("fp32_tflops", c.peak_fp32_tflops, est, "peak");
      p.num("fp64_tflops", c.peak_fp64_tflops, est, "peak");
      p.num("int8_tops", c.peak_int8_tops, est, "peak");
    }
    o += '}';

    if (!c.features.empty()) {
      o += ",\"features\":";
      strlist(o, c.features);
    }
    if (!est.empty()) {
      o += ",\"estimated\":";
      strlist(o, est);
    }
  }
  o += '}';

  /* ---- gpus ---- */
  o += ",\"gpus\":[";
  for (size_t i = 0; i < r.gpus.size(); ++i) {
    const Gpu& g = r.gpus[i];
    std::vector<std::string> est;
    if (i) o += ',';
    o += '{';
    {
      W x(o);
      x.str("vendor", g.vendor);
      x.str("name", g.name);
      x.str("arch", g.arch);
      x.str("core_unit", g.core_unit);
      x.num("cores", g.cores, est, "");
      x.num("tensor_cores", g.tensor_cores, est, "");
      if (g.tensor_cores.has) {
        std::string k = "tensor_cores_per_" + g.core_unit;
        x.u32(k.c_str(), g.tc_per_core);
      }
      x.num("clock_ghz", g.clock_ghz, est, "");
      x.num("vram_bytes", g.vram_bytes, est, "");
      x.boolean("unified_memory", g.unified);
      x.num("mem_bandwidth_gbs", g.mem_bw_gbs, est, "");

      o += ",\"peak\":{";
      {
        W p(o);
        p.num("fp64_tflops", g.fp64_tflops, est, "peak");
        p.num("fp32_tflops", g.fp32_tflops, est, "peak");
        p.num("tf32_tflops", g.tf32_tflops, est, "peak");
        p.num("bf16_tflops", g.bf16_tflops, est, "peak");
        p.num("fp16_tflops", g.fp16_tflops, est, "peak");
        p.num("fp16_tflops_sparse", g.fp16_tflops_sparse, est, "peak");
        p.num("fp8_tflops", g.fp8_tflops, est, "peak");
        p.num("int8_tops", g.int8_tops, est, "peak");
      }
      o += '}';

      x.str("method", g.method);
      if (!g.pci_id.empty()) x.str("pci", g.pci_id);
    }
    if (!est.empty()) {
      o += ",\"estimated\":";
      strlist(o, est);
    }
    o += '}';
  }
  o += ']';

  /* ---- npus ---- */
  o += ",\"npus\":[";
  for (size_t i = 0; i < r.npus.size(); ++i) {
    const Npu& n = r.npus[i];
    std::vector<std::string> est;
    if (i) o += ',';
    o += '{';
    {
      W x(o);
      x.str("vendor", n.vendor);
      x.str("name", n.name);
      x.str("arch", n.arch);
      x.num("cores", n.cores, est, "");
      o += ",\"peak\":{";
      {
        W p(o);
        p.num("int8_tops", n.int8_tops, est, "peak");
        p.num("fp16_tflops", n.fp16_tflops, est, "peak");
      }
      o += '}';
      x.str("method", n.method);
    }
    if (!est.empty()) {
      o += ",\"estimated\":";
      strlist(o, est);
    }
    o += '}';
  }
  o += ']';

  /* ---- accelerators ---- */
  o += ",\"accelerators\":{";
  for (size_t i = 0; i < r.accels.size(); ++i) {
    const Accel& a = r.accels[i];
    if (i) o += ',';
    jesc(o, a.name);
    o += ':';
    if (!a.available) {
      o += "{\"available\":false}";
      continue;
    }
    o += "{\"available\":true";
    if (!a.category.empty()) { o += ",\"category\":"; jesc(o, a.category); }
    if (!a.version.empty()) { o += ",\"version\":"; jesc(o, a.version); }
    if (!a.detail.empty()) { o += ",\"detail\":"; jesc(o, a.detail); }
    o += '}';
  }
  o += '}';

  o += ",\"warnings\":";
  strlist(o, r.warnings);
  o += "}\n";
  return o;
}

/* ============================================================= table */
std::string to_table(const Report& r) {
  std::string o;
  char line[512];

  std::snprintf(line, sizeof line, "devinfo - %.2f ms  (%s)\n", r.probe_ms, r.level.c_str());
  o += line;
  std::snprintf(line, sizeof line, "%s %s | %s | %s\n",
                r.os.c_str(), r.os_version.c_str(), r.arch.c_str(), r.env.c_str());
  o += line;
  o += std::string(66, '-') + "\n";

  /* ---- CPU ---- */
  const Cpu& c = r.cpu;
  o += "CPU  " + (c.model.empty() ? std::string("unknown") : c.model) + "\n";
  {
    std::string cores = std::to_string(c.physical) + " cores";
    if (c.p_cores && c.e_cores)
      cores += " (" + std::to_string(c.p_cores) + "P + " + std::to_string(c.e_cores) + "E)";
    else if (c.p_cores && c.p_cores != c.physical)
      cores += " (" + std::to_string(c.p_cores) + "P)";
    if (c.logical > c.physical) cores += " / " + std::to_string(c.logical) + " threads";
    o += "     " + pad(cores, 30);
  }
  if (c.clock_ghz.has) {
    o += "clock " + fmt_num(c.clock_ghz.v, " GHz") + (c.clock_ghz.estimated ? "~" : "");
  }
  o += "  " + c.arch + "\n";

  {
    std::string cache;
    if (c.l1i) cache += "L1i " + fmt_bytes(c.l1i) + "  ";
    if (c.l1d) cache += "L1d " + fmt_bytes(c.l1d) + "  ";
    if (c.l2) cache += "L2 " + fmt_bytes(c.l2) + "  ";
    if (c.l3) cache += "L3 " + fmt_bytes(c.l3) + "  ";
    cache += "line " + std::to_string(c.line) + "B";
    o += "     " + cache + "\n";
  }

  {
    std::string mem;
    if (c.mem_bytes.has) mem += "RAM " + fmt_bytes(c.mem_bytes.v) + "  ";
    if (c.mem_bw_gbs.has) mem += "bw " + fmt_num(c.mem_bw_gbs.v, " GB/s") + (c.mem_bw_gbs.estimated ? "~" : "");
    if (c.mem_bw_measured_gbs.has) mem += "  measured " + fmt_num(c.mem_bw_measured_gbs.v, " GB/s");
    o += "     " + mem + "\n";
  }

  {
    std::string pk;
    auto push = [&pk](const std::string& s) { if (!pk.empty()) pk += "  "; pk += s; };
    if (c.peak_fp32_tflops.has) push("FP32 " + fmt_num(c.peak_fp32_tflops.v, " TFLOPS") + mark(c.peak_fp32_tflops));
    if (c.peak_fp64_tflops.has) push("FP64 " + fmt_num(c.peak_fp64_tflops.v, " TFLOPS") + mark(c.peak_fp64_tflops));
    if (c.peak_int8_tops.has)   push("INT8 " + fmt_num(c.peak_int8_tops.v, " TOPS") + mark(c.peak_int8_tops));
    if (!pk.empty()) o += "     peak " + pk + "\n";
  }

  if (!c.features.empty()) {
    /* Wrap rather than truncate: the interesting extensions (VNNI, AMX, SVE)
     * sort late in the list and a "..." would hide exactly the ones that
     * decide which kernel you can call. */
    const std::string indent = "     isa ";
    const size_t width = 76;
    std::string fl = indent;
    bool first = true;
    for (const auto& f : c.features) {
      if (!first && fl.size() + 1 + f.size() > width) {
        o += fl + "\n";
        fl = std::string(indent.size(), ' ');
      }
      if (!first) fl += ' ';
      fl += f;
      first = false;
    }
    o += fl + "\n";
  }

  /* ---- GPUs ---- */
  if (r.gpus.empty()) {
    o += "GPU  none detected\n";
  }
  for (size_t i = 0; i < r.gpus.size(); ++i) {
    const Gpu& g = r.gpus[i];
    std::snprintf(line, sizeof line, "GPU%d %s\n", (int)i, g.name.c_str());
    o += line;
    std::string head;
    auto hpush = [&head](const std::string& s) { if (!head.empty()) head += "  "; head += s; };
    if (!g.pci_id.empty()) hpush(g.pci_id);
    if (g.cores.has) hpush(fmt_count(g.cores.v) + " " + g.core_unit + "(s)");
    if (g.tensor_cores.has) hpush(fmt_count(g.tensor_cores.v) + " Tensor Cores");
    if (!g.arch.empty()) hpush(g.arch);
    if (!head.empty()) o += "     " + head + "\n";

    std::string mem;
    auto mpush = [&mem](const std::string& s) { if (!mem.empty()) mem += "  "; mem += s; };
    if (g.vram_bytes.has) mpush((g.unified ? "unified " : "vram ") + fmt_bytes(g.vram_bytes.v));
    if (g.clock_ghz.has)  mpush("clock " + fmt_num(g.clock_ghz.v, " GHz") + mark(g.clock_ghz));
    if (g.mem_bw_gbs.has) mpush("bw " + fmt_num(g.mem_bw_gbs.v, " GB/s") + mark(g.mem_bw_gbs));
    if (!mem.empty()) o += "     " + mem + "\n";

    std::string pk;
    auto push = [&pk](const std::string& s) { if (!pk.empty()) pk += "  "; pk += s; };
    if (g.fp32_tflops.has)        push("FP32 " + fmt_num(g.fp32_tflops.v, " TF") + mark(g.fp32_tflops));
    if (g.fp16_tflops.has)        push("FP16 " + fmt_num(g.fp16_tflops.v, " TF") + mark(g.fp16_tflops));
    if (g.bf16_tflops.has)        push("BF16 " + fmt_num(g.bf16_tflops.v, " TF") + mark(g.bf16_tflops));
    if (g.int8_tops.has)          push("INT8 " + fmt_num(g.int8_tops.v, " TOPS") + mark(g.int8_tops));
    if (g.fp8_tflops.has)         push("FP8 " + fmt_num(g.fp8_tflops.v, " TF") + mark(g.fp8_tflops));
    if (g.fp16_tflops_sparse.has)
      push("(sparse " + fmt_num(g.fp16_tflops_sparse.v, " TF") + ")" + mark(g.fp16_tflops_sparse));
    if (!pk.empty()) o += "     peak " + pk + "\n";
  }

  /* ---- NPUs ---- */
  if (r.npus.empty()) {
    o += "NPU  none detected\n";
  }
  for (size_t i = 0; i < r.npus.size(); ++i) {
    const Npu& n = r.npus[i];
    std::snprintf(line, sizeof line, "NPU%d %s (%s)", (int)i, n.name.c_str(), n.arch.c_str());
    o += line;
    if (n.cores.has) o += std::string("  ") + fmt_count(n.cores.v) + " cores";
    if (n.int8_tops.has) o += "  " + fmt_num(n.int8_tops.v, " TOPS") + mark(n.int8_tops);
    o += "\n";
  }

  /* ---- accelerators ---- */
  o += std::string(66, '-') + "\n";
  o += "accelerators\n";
  for (const auto& a : r.accels) {
    if (!a.available) continue;
    std::string ln = "     " + pad(a.name, 14) + pad(a.version, 14);
    if (!a.category.empty()) ln += a.category;
    o += ln + "\n";
  }
  {
    std::string missing;
    for (const auto& a : r.accels) {
      if (a.available) continue;
      if (a.name == "cuda" || a.name == "rocm" || a.name == "cann" || a.name == "vulkan" ||
          a.name == "opencl" || a.name == "sycl")
        missing += a.name + " ";
    }
    if (!missing.empty()) o += "     -- not present: " + missing + "--\n";
  }

  if (!r.warnings.empty()) {
    for (const auto& w : r.warnings) o += "WARN " + w + "\n";
  }
  o += "     ~ = estimated from chip spec table, not read from hardware\n";

  /* strip trailing blanks line by line */
  std::string out;
  out.reserve(o.size());
  size_t i = 0;
  while (i < o.size()) {
    size_t nl = o.find('\n', i);
    size_t end = (nl == std::string::npos) ? o.size() : nl;
    size_t last = end;
    while (last > i && (o[last - 1] == ' ' || o[last - 1] == '\t')) --last;
    out.append(o, i, last - i);
    if (nl == std::string::npos) break;
    out += '\n';
    i = nl + 1;
  }
  return out;
}

}  /* namespace devinfo */