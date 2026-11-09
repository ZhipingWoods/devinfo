devinfo macOS 测试包
===================

通用二进制：单个文件同时含 arm64 和 x86_64，Apple Silicon 与 Intel Mac 都能跑。
零第三方依赖，只链接系统框架（IOKit / CoreFoundation / libc++）。

文件
----
devinfo              命令行工具（主程序）
libdevinfo.dylib     C ABI 插件（4 个导出符号）


直接跑
------
    chmod +x devinfo
    ./devinfo --table

其他参数：

    ./devinfo --json      输出 JSON（管道/重定向时是默认）
    ./devinfo --bench     额外测内存带宽，慢一点但给出实测带宽
    ./devinfo -v          版本
    ./devinfo -h          帮助

不带参数时：终端里跑默认出表格，重定向时默认出 JSON。


首次运行被 Gatekeeper 拦住怎么办
--------------------------------
这个二进制是 adhoc 签名、未公证。从网络传过来的文件会被打上 quarantine 标记，
双击会提示"无法验证开发者"。解除：

    xattr -d com.apple.quarantine devinfo
    xattr -d com.apple.quarantine libdevinfo.dylib

或者：右键 → 打开 → 仍要打开。

在本机用 git clone / 本地复制得到的文件没有 quarantine 标记，可直接运行。


验插件 ABI（不需要编译器）
--------------------------
macOS 自带 python3 就能测：

    python3 - <<'EOF'
    import ctypes
    lib = ctypes.CDLL("./libdevinfo.dylib")
    lib.devinfo_abi_version.restype = ctypes.c_int
    print("abi:", lib.devinfo_abi_version())

    lib.devinfo_probe_json.restype = ctypes.c_void_p
    lib.devinfo_probe_json.argtypes = [ctypes.c_uint]
    p = lib.devinfo_probe_json(0)
    print(ctypes.string_at(p, 160).decode("utf-8", "replace"))

    lib.devinfo_free.argtypes = [ctypes.c_void_p]
    lib.devinfo_free(p)
    EOF

期望：abi 打印 1，字符串以 {"schema":"devinfo/1" 开头。


输出里哪些是实测、哪些是估算
----------------------------
    ~   后缀带 ~ 的数字是从芯片规格表推算的，不是从硬件读的

实测项：核心数、缓存、内存容量、内存速率（SMBIOS）、磁盘、ISA 扩展、
        GPU 型号与 PCI ID、显存、统一内存标记、加速库是否存在
估算项：FP32/FP16/INT8 峰值、NPU 算力、GPU 带宽（部分平台）、
        deploy 段的所有推理/训练容量结论

deploy 段是从上面测到的东西推出来的，不是测出来的，文件头也这么写。


已知限制
--------
- 睿频上限：macOS 读不到，显示的 clock 是额定值（带 ~ 表示来自规格）
- Apple GPU 带宽：和统一内存共享，标注为推算
- 网络速率：macOS 上链路聚合/多网卡有歧义，默认不报
