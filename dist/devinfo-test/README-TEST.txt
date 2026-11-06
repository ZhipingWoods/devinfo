devinfo Windows 测试包  (第 2 轮)
===============================

这一轮修了第 1 轮输出里暴露的 5 个问题，需要在真机上确认。

文件
----
devinfo-x64.exe    64 位命令行
devinfo-x86.exe    32 位命令行
devinfo-x64.dll    64 位 C ABI 插件

x64  SHA256 前16位: 8c5744d6fae7ce34
x86  SHA256 前16位: 959fe7ba3bb1d938


第 1 轮修了什么
--------------
1. ISA 缺 SSE4.2
   win32.cpp 里只写了 CPUID leaf1 ECX bit19 (SSE4.1)，漏了 bit20 (SSE4.2)。
   第 1 轮输出 "SSE2 SSE4.1 POPCNT" 中间缺一个，就是它。
   -> 这一轮应变成 "SSE2 SSE4.1 SSE4.2 POPCNT"。

2. CPU 峰值算成一半
   原来按每核 1 条向量流水线算，但 x86 从 AVX 起都是 2 条（Intel port0/5,
   AMD FP0/FP1）。i7-11700: 8 x 2.5 GHz x 32 = 0.640 是错的，
   正确是 8 x 2.5 GHz x 64 = 1.280 TFLOPS。
   -> 这一轮 peak FP32 应显示 1.280 TFLOPS~（原来 0.640）。

3. 显存把独显和核显加在一起
   RX 640 的 4.0 GiB + UHD 750 的 128 MiB = "4.1 GiB over all GPUs"，
   但这个和任何 kernel 都寻址不到。核显没有独立显存，那是系统内存的划拨。
   -> 这一轮应变成 "memory 4.0 GiB, the largest pool: 1 other GPU is a
      different model and cannot be pooled with it"。

4. "training ~0b"
   0.26b 被 %.0f 舍成 0，看起来像 bug。
   -> 这一轮应显示 "training ~0.26b"。

5. parallel 对异构显卡没给结论
   原来只打印 "2 GPUs"。
   -> 这一轮应显示 "2 GPUs of 2 different models: no tensor parallel across
      them, each runs its own job"。

6. rocm 谎报
   系统里有 amdhip64.dll（HIP 运行时，显卡驱动自带），但 Windows 上
   ROCm 不支持 Polaris，标成 "rocm 5.2" 是夸大。
   -> 这一轮应显示 "hip 5.2" 而不是 "rocm 5.2"。


跑这些
------
cd 到这个文件夹，然后：

    Unblock-File .\*

    .\devinfo-x64.exe --table

    .\devinfo-x86.exe --table

    cmd /c "devinfo-x64.exe --json > report.json"


另外请单独跑这一条
------------------
第 1 轮没出现 "(max 4.900 GHz)" 这个括号。我需要确认是 API 返回了基频，
还是调用直接失败了：

    .\devinfo-x64.exe --json | findstr clock

期望看到 clock_ghz 和 clock_max_ghz 两个字段。
如果 clock_max_ghz 是 null，说明 CallNtPowerInformation 这条路在这台机器上
拿不到睿频——那是 Windows 平台限制（用户态没有可靠的睿频来源），
不是我算错了，我会把说明写进文档，不再假装能读。


双击测试（第 1 轮没做）
----------------------
资源管理器里双击 devinfo-x64.exe，不要在 PowerShell 里跑。
窗口应停住、最后一行是 "press Enter to close"，并打印两行 report written:。
这是最初反馈的"一闪就没了"，这个分支至今没在真机上验过。
