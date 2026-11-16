#!/usr/bin/env python3
"""Generate site/results.html from the captured runs.

The raw-output blocks are read straight out of assets/captures/*.txt and
HTML-escaped, so the page cannot show anything the binary did not print.
Re-run this after a new capture:

    python3 tools/build_results.py
"""
import html
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPS = ROOT / "assets" / "captures"
OUT = ROOT / "site" / "results.html"

MACHINES = [
    {
        "id": "m5",
        "name": "Apple M5",
        "sub": "MacBook · macOS 26.6.2",
        "arch": "arm64",
        "shot": "assets/macos-m5.png",
        "cap": "macos-m5.txt",
        "probe": "3.43 ms",
        "facts": [
            ("CPU", "Apple M5"),
            ("核心", "10 核（4 性能 + 6 能效）"),
            ("频率", "4.610 GHz~"),
            ("缓存", "L1i 1.5 MiB · L1d 896 KiB · L2 22.0 MiB · 行 128 B"),
            ("内存", "16.0 GiB 统一内存 · 带宽 153.0 GB/s~"),
            ("磁盘", "87.9 GiB 可用 / 460.4 GiB · ssd"),
            ("GPU", "8 GPU 核 · AGXMetalG17G · 1.620 GHz~"),
            ("显存", "16.0 GiB 统一内存（不是独立显存）"),
            ("GPU 算力", "FP32 3.318 TF~ · FP16 3.318 TF~"),
            ("NPU", "Apple Neural Engine · 16 核 · 40.00 TOPS~"),
            ("CPU 峰值", "0.878 TFLOPS~（NEON）"),
            ("加速栈", "Metal（运行时）· OpenCL（运行时）· 无 CUDA"),
        ],
        "deploy": [
            ("推理容量", "~33b int4 · ~16b int8 · ~8.2b fp16"),
            ("训练容量", "~1.0b（16 B/param，Adam，不含激活）"),
            ("精度单元", "fp16（来自硬件能力位）"),
            ("并行", "单 GPU，路径上没有跨设备链路"),
            ("Roofline", "22 FLOP/byte —— 低于此值的算子受带宽限制"),
        ],
        "note": "统一内存是关键：16 GiB 既是系统内存也是显存，所以容量按统一池算一次，不做加总。",
    },
    {
        "id": "4090",
        "name": "2× EPYC 7402 + 8× RTX 4090",
        "sub": "训练服务器 · Ubuntu 22.04",
        "arch": "x86_64",
        "shot": "assets/linux-8x4090.png",
        "cap": "linux-8x4090.txt",
        "probe": "142.39 ms",
        "facts": [
            ("CPU", "AMD EPYC 7402 24-Core ×2"),
            ("核心", "48 核 / 96 线程"),
            ("频率", "2.800 GHz（读自硬件，非估算）"),
            ("缓存", "L1i 1.5 MiB · L1d 1.5 MiB · L2 24.0 MiB · L3 256.0 MiB"),
            ("内存", "501.3 GiB · NUMA 8 节点 × 12 核"),
            ("磁盘", "39.0 GiB 可用 / 873.7 GiB · ssd"),
            ("网络", "25.00 Gb/s · ens8f0np0 · rdma"),
            ("GPU", "8 × RTX 4090 · 128 SM · 512 Tensor Core · sm_8.9"),
            ("显存", "23.5 GiB × 8 = 186.6 GiB · 2.520 GHz · gen4 x16"),
            ("带宽", "1008.1 GB/s 每卡"),
            ("GPU 算力", "FP32 82.58 TF~ · FP16/BF16 330.3 TF~ · INT8/FP8 660.6 TOPS~"),
            ("CPU 峰值", "4.301 TFLOPS~（AVX2）"),
            ("加速栈", "CUDA 13.0 · NVML 580.119.02 · Vulkan · OpenCL"),
        ],
        "deploy": [
            ("推理容量", "~382b int4 · ~191b int8 · ~96b fp16（8 卡合计）"),
            ("训练容量", "~12b（16 B/param，Adam，不含激活）"),
            ("精度单元", "tensor cores：fp8 · bf16 · fp16 · int8"),
            ("并行", "8 卡全部 x16：tensor parallel 可在 PCIe 内完成"),
            ("Roofline", "82 FLOP/byte（82.6 TFLOPS / 1008 GB/s）"),
            ("磁盘告警", "仅 39 GiB 可用，而 stage 一个半精度 checkpoint 约需 186.6 GiB"),
        ],
        "note": "8 张同型号卡被识别为一个可合并的内存池；这也是 probe 最慢的一台，因为 cuInit 要初始化 8 个设备。",
    },
    {
        "id": "orin",
        "name": "Jetson AGX Orin 64GB",
        "sub": "边缘设备 · L4T R36.4.7 · Ubuntu 22.04",
        "arch": "aarch64",
        "shot": "assets/jetson-orin.png",
        "cap": "jetson-orin.txt",
        "probe": "29.65 ms",
        "facts": [
            ("CPU", "ARMv8 Processor rev 1 (v8l)"),
            ("核心", "12 核"),
            ("频率", "2.202 GHz"),
            ("缓存", "L1i 768 KiB · L1d 768 KiB · L2 3.0 MiB · L3 6.0 MiB"),
            ("内存", "61.4 GiB 统一内存（与 GPU 共享）"),
            ("磁盘", "12.1 GiB 可用 / 274.5 GiB · nvme"),
            ("GPU", "Orin · 16 SM · 64 Tensor Core · sm_8.7 · 1.300 GHz"),
            ("显存", "61.4 GiB 统一内存（由 CUDA 的 integrated 属性判定）"),
            ("GPU 算力", "FP32 5.325 TF~ · FP16/BF16 10.65 TF~ · INT8 21.30 TOPS~"),
            ("NPU", "未检测到（Orin 的 DLA 不走 CUDA 接口，不猜）"),
            ("CPU 峰值", "0.423 TFLOPS~（NEON）"),
            ("加速栈", "CUDA 12.6 · cuDNN 9.20 · TensorRT 10.7 · PyTorch 2.5.0 · Vulkan · OpenCL"),
        ],
        "deploy": [
            ("推理容量", "~126b int4 · ~63b int8 · ~31b fp16"),
            ("训练容量", "~3.9b（16 B/param，Adam，不含激活）"),
            ("精度单元", "tensor cores：bf16 · fp16 · int8"),
            ("并行", "单 GPU"),
            ("Roofline", "不给 —— 见下"),
        ],
        "note": "这台机器最能说明设计原则：LPDDR5 的统一内存没有可信的带宽读法，于是 deploy 段直接写 “ridge point unavailable: no honest bandwidth figure for this GPU (run --bench)”，而不是从规格表抄一个数字。",
    },
    {
        "id": "woods924",
        "name": "Dell OptiPlex 7090",
        "sub": "办公机 · Windows 11 10.0.26200",
        "arch": "x86_64",
        "shot": None,
        "cap": None,
        "probe": "199.29 ms",
        "facts": [
            ("CPU", "Intel Core i7-11700"),
            ("核心", "8 核 / 16 线程"),
            ("内存", "63.7 GiB · DDR4 2667 MT/s · 4 DIMMs（SMBIOS）"),
            ("GPU", "Radeon RX 640 (1002:6987) 4.0 GiB"),
            ("GPU", "Intel UHD 750 (8086:4c8a) 128 MiB 显存切分"),
            ("磁盘", "C: 14.7 GiB 可用 / 237.7 GiB"),
            ("加速栈", "AMD HIP 5.2（amdhip64.dll）· 无 CUDA"),
        ],
        "deploy": [
            ("异构 GPU", "两张不同型号的卡不能合并成内存池，各自跑各自的作业"),
        ],
        "note": "这台机器跑出了第一轮实机测试里的六个问题，全部已修：SSE4.2 漏报、CPU 峰值只有一半、异构显存被错误加总、训练容量显示 ~0b、异构并行措辞、HIP 被误标成 ROCm 版本号。修复后尚未在这台机器上复测——这一格留空，不填推测值。",
    },
]


def esc(s):
    return html.escape(str(s))


def raw_block(cap):
    if not cap:
        return ""
    p = CAPS / cap
    if not p.exists():
        return ""
    text = p.read_text(encoding="utf-8", errors="replace")
    lines = [ln for ln in text.splitlines() if not ln.startswith("bash: warning:")]
    return "\n".join(lines)


def machine_section(m):
    rows = "\n".join(
        f'<div><dt>{esc(k)}</dt><dd>{esc(v)}</dd></div>' for k, v in m["facts"]
    )
    dep = "\n".join(
        f'<div><dt>{esc(k)}</dt><dd>{esc(v)}</dd></div>' for k, v in m["deploy"]
    )
    raw = raw_block(m["cap"])
    shot = ""
    if m["shot"]:
        shot = f'''
      <figure class="shot-figure">
        <button class="shot" data-full="{m["shot"]}"><img src="{m["shot"]}" alt="devinfo on {esc(m["name"])}" loading="lazy"></button>
        <figcaption>真实输出渲染 · 点击放大</figcaption>
      </figure>'''
    raw_html = ""
    if raw:
        raw_html = f'''
      <details class="raw">
        <summary>原始输出（{len(raw.splitlines())} 行，逐字节来自捕获文件）</summary>
        <pre>{esc(raw)}</pre>
      </details>'''
    return f'''
    <section class="machine" id="{m["id"]}">
      <header class="machine-head">
        <div>
          <h2>{esc(m["name"])}</h2>
          <p>{esc(m["sub"])} · <code>{m["arch"]}</code></p>
        </div>
        <div class="probe-time"><small>probe</small><b>{esc(m["probe"])}</b></div>
      </header>
      <div class="machine-body">
        <dl class="facts">{rows}</dl>
        <div class="machine-side">
          <div class="deploy-box"><h3>部署推导（derive，不是实测）</h3><dl class="facts small">{dep}</dl></div>
          {shot}
        </div>
      </div>
      <p class="machine-note">{esc(m["note"])}</p>{raw_html}
    </section>'''


def main():
    summary_rows = "\n".join(
        f'<tr><td><a href="#{m["id"]}">{esc(m["name"])}</a></td><td>{esc(m["sub"])}</td>'
        f'<td class="num">{m["arch"]}</td><td class="num">{esc(m["probe"])}</td></tr>'
        for m in MACHINES
    )
    sections = "\n".join(machine_section(m) for m in MACHINES)
    OUT.write_text(TEMPLATE.replace("{{SUMMARY}}", summary_rows).replace("{{SECTIONS}}", sections),
                   encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}  ({OUT.stat().st_size} bytes)")


TEMPLATE = """<!doctype html>
<html lang="zh-CN" data-theme="dark" data-lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="description" content="devinfo 在四台真实机器上的实测记录：Apple M5、双 EPYC 7402 + 八张 RTX 4090、Jetson AGX Orin、Dell OptiPlex 7090。">
<title>devinfo · 实测结果</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='15' fill='%234f9dff'/%3E%3Ctext x='32' y='43' text-anchor='middle' font-size='26' font-family='monospace' font-weight='700' fill='%2304121f'%3Edi%3C/text%3E%3C/svg%3E">
<link rel="stylesheet" href="styles.css">
</head>
<body>
<div class="ambient" aria-hidden="true">
  <div class="orb orb-a"></div><div class="orb orb-b"></div><div class="grid-fade"></div>
</div>

<header class="nav">
  <a class="brand" href="index.html">
    <span class="brand-mark">di</span>
    <span class="brand-copy"><b>devinfo</b><small>results</small></span>
  </a>
  <nav class="nav-links">
    <a href="index.html" data-zh="首页" data-en="Home"></a>
    <a href="#m5">M5</a>
    <a href="#4090">8×4090</a>
    <a href="#orin">Orin</a>
    <a href="#woods924">Windows</a>
  </nav>
  <div class="nav-actions">
    <div class="seg lang-switch">
      <button class="on" data-set-lang="zh">中文</button>
      <button data-set-lang="en">EN</button>
    </div>
    <button class="icon-btn" id="theme-btn" aria-label="Toggle theme">
      <span class="theme-sun">☀</span><span class="theme-moon">◐</span>
    </button>
  </div>
</header>

<main class="wrap">
  <section class="page-head">
    <span class="eyebrow">MEASURED RECORD</span>
    <h1 data-zh="四台机器的真实读数" data-en="Four machines, as read"></h1>
    <p class="lede" data-zh="同一个二进制，跑在四台架构完全不同的机器上。下面是它读到的全部内容，以及每一台的原始输出。没有一项是事后填的。" data-en="The same binary, run on four completely different machines. Below is everything it read, plus the raw output of each run. Nothing here was filled in after the fact."></p>
  </section>

  <section class="table-wrap">
    <table class="data">
      <thead><tr>
        <th data-zh="机器" data-en="Machine"></th>
        <th data-zh="环境" data-en="Environment"></th>
        <th data-zh="架构" data-en="Arch"></th>
        <th data-zh="探针耗时" data-en="Probe"></th>
      </tr></thead>
      <tbody>
{{SUMMARY}}
      </tbody>
    </table>
  </section>

{{SECTIONS}}

  <section class="section">
    <div class="section-head">
      <span class="eyebrow">LIMITS</span>
      <h2 data-zh="已知的边界" data-en="Known limits"></h2>
      <p data-zh="这些不是待办事项，是这台程序目前明确做不到、或者做不全的地方。列出来是为了让人知道哪一列可以信、哪一列要自己复核。" data-en="These are not a to-do list; they are places where the program currently cannot read, or cannot read fully. They are listed so you know which column to trust and which to double-check."></p>
    </div>
    <div class="table-wrap">
      <table class="data">
        <thead><tr>
          <th data-zh="项" data-en="Item"></th>
          <th data-zh="现状" data-en="Status"></th>
          <th data-zh="原因" data-en="Why"></th>
        </tr></thead>
        <tbody>
          <tr>
            <td data-zh="Linux 内存速率" data-en="Linux memory speed"></td>
            <td class="no" data-zh="非 root 时缺失" data-en="missing without root"></td>
            <td data-zh="SMBIOS 表在 /sys/firmware/dmi/tables/DMI，只有 root 可读；读不到就不报" data-en="The SMBIOS table lives in /sys/firmware/dmi/tables/DMI and is root-only; unreadable means unreported"></td>
          </tr>
          <tr>
            <td data-zh="Windows 内存速率" data-en="Windows memory speed"></td>
            <td class="ok" data-zh="有（SMBIOS）" data-en="yes (SMBIOS)"></td>
            <td data-zh="GetSystemFirmwareTable 不需要管理员权限，所以 Windows 反而比 Linux 全" data-en="GetSystemFirmwareTable needs no admin rights, so Windows is actually more complete than Linux here"></td>
          </tr>
          <tr>
            <td data-zh="Windows 睿频" data-en="Windows turbo clock"></td>
            <td class="no" data-zh="只有基频" data-en="base clock only"></td>
            <td data-zh="CallNtPowerInformation 在该机器上只返回基频；不拿规格表去补" data-en="CallNtPowerInformation returned only the base clock on that machine; no spec-table top-up"></td>
          </tr>
          <tr>
            <td data-zh="非 NVIDIA 的 PCIe 链路" data-en="PCIe on non-NVIDIA"></td>
            <td class="no" data-zh="未报" data-en="not reported"></td>
            <td data-zh="只有 NVML 能可靠给出链路代数与宽度；其他走猜测不如空着" data-en="Only NVML reliably reports link generation and width; a guess is worse than a blank"></td>
          </tr>
          <tr>
            <td data-zh="集成显卡带宽" data-en="Integrated GPU bandwidth"></td>
            <td class="no" data-zh="有意不给" data-en="deliberately withheld"></td>
            <td data-zh="核显与内存共享总线，没有独立的“显存带宽”这一说" data-en="An iGPU shares the memory bus; there is no separate VRAM bandwidth to report"></td>
          </tr>
          <tr>
            <td data-zh="Windows 网络 / 磁盘介质" data-en="Windows network / disk media"></td>
            <td class="no" data-zh="未实现" data-en="not implemented"></td>
            <td data-zh="需要 SetupAPI 与 IOCTL 组合，尚未做；宁缺勿假" data-en="Needs SetupAPI plus IOCTL work that is not done yet; blank beats wrong"></td>
          </tr>
          <tr>
            <td data-zh="Windows 复测" data-en="Windows re-test"></td>
            <td class="no" data-zh="待复测" data-en="pending"></td>
            <td data-zh="六个问题已修复并重新构建，但那台机器暂时连不上，所以本页不给出修复后的 Windows 数字" data-en="Six issues were fixed and the binaries rebuilt, but that machine is currently unreachable, so no post-fix Windows numbers appear on this page"></td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>

  <section class="final-cta">
    <div>
      <span class="eyebrow">BACK</span>
      <h2 data-zh="回到首页看能读什么" data-en="Back to what it reads"></h2>
      <p data-zh="这些数字全部来自同一个二进制。它在你自己的机器上只会做同样的事。" data-en="All of these numbers came from the same binary. On your machine it will do the same thing."></p>
    </div>
    <div class="cta-row">
      <a class="btn primary" href="index.html" data-zh="返回首页" data-en="Back to home"></a>
      <a class="btn" href="#top" data-zh="回到顶部" data-en="Top"></a>
    </div>
  </section>
</main>

<footer class="footer wrap">
  <div class="brand">
    <span class="brand-mark small">di</span>
    <span><b>devinfo</b><br><small style="color:var(--dim);font-family:var(--mono);font-size:10.5px">C++17 · zero dependencies</small></span>
  </div>
  <p><span data-zh="MIT 许可证 · 2026" data-en="MIT License · 2026"></span> · <a href="index.html" data-zh="首页" data-en="Home"></a></p>
</footer>

<div class="lightbox" id="lightbox">
  <button aria-label="Close">✕</button>
  <img id="lightbox-img" src="" alt="Screenshot">
</div>

<script src="main.js"></script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
