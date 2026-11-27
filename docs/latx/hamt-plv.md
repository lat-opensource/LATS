# HAMT guest 特权级与 LoongArch PLV

执行翻译代码时，HAMT 可以将 x86 guest 的特权级映射到 LoongArch PLV。
设置 `hamtconfig` 的 `0x400` 位启用此功能。它要求使用 `hamtmode=1,hamtsubmode=1`
（见 [HAMT DA/PG](hamt-da-pg.md)）；只启用 DA/PG 不会启用 PLV。

## 执行特权级切换

guest CPL 记录在 `CPUX86State.hflags` 中。需要访问特权 CSR 的翻译器 helper 和
运行时代码在 PLV0 下执行。返回 guest 翻译代码前，入口代码恢复与 guest CPL 对应的
PLV；特权返回 helper 可以从 `hflags` 读取 CPL，并且只修改 `CRMD.PLV`。

翻译后的快速 SYSCALL 通过 HAMT 保留的 `BREAK` 进入 PLV0，快速 SYSRET 请求切换到
PLV3。快速 IRET 和中断返回分派使用新的 CPL：在 `BREAK` 路径上，CPL0 选择 PLV0，
非零 CPL 选择 PLV3。生成的 `BREAK` 处理代码识别这些内部编码，修改 `PRMD.PPLV`
中保存的返回特权级，将 `ERA` 移到 `BREAK` 后的指令，再通过 `ERTN` 恢复执行。
因此，PLV3 的翻译代码无需直接写特权 CSR。这条路径并不将 x86 的 ring 1 和 ring 2
分别映射到对应的 LoongArch 特权级。

## TLB 条目特权级的依据

执行指令的 CPL 和页的 PLV 来自不同的信息。页的 PLV 由遍历 x86 页表时逐级合成的
有效 U/S 权限决定，而不取决于触发 TLB 重填的那条指令的 CPL：

- 用户可访问页使用 PLV3；仅监管态可访问的页使用 PLV0，即使第一次访问来自监管态。
- 生成的重填代码分别计算两个半页的 U/S，将结果写入各自有效的 `TLBRELO0/1` 条目。
  用户态访问仅监管态可访问的页时会被拒绝，而不会根据当前 CPL 放行。
- C 语言实现的 HAMT 页表遍历和 x86 SoftMMU 转换也把相同的 PLV 记录在扩展保护位
  （`HAMT_PROT_*_PLV`）中。HAMT 的 C 写入路径从这些保护位提取 PLV，填写硬件 TLB 条目。

R/W 等 x86 分页规则由页表遍历处理。[PTW 权限标记](hamt-ptwflags.md) 可以另行禁止
某个 GPA 的直映访问或写入，但不会把仅监管态可访问的 guest PTE 变为用户可访问。

## 访存权限异常

LoongArch TLB 权限异常不会直接转换成 x86 缺页异常。HAMT 异常入口保存状态，
把故障地址和翻译代码的 PC 交给 `hamt_tlb_handler()`。处理函数识别访存类型，并尝试
用 C 路径遍历页表。若能安装允许该访问的映射，就恢复执行；否则区分 guest 缺页异常
和 SMC、VGA 或 MMIO 等仅由 HAMT 引入的限制。后者可能需要回退到普通 SoftMMU 路径，
而不是向 guest 注入异常。

例如，CPL3 读写一个存在但仅监管态可访问的页时，C 页表遍历返回 guest 权限错误
（`P=1`，`U/S` 和 `W/R` 由本次访问决定）。在 guest 异常快速路径上，HAMT 恢复
guest 状态、设置 `CR2`、递交 x86 `#PF`，并转到 guest 异常处理函数。异常递交本身
可能改变 CPL，因此 HAMT 在执行 `ERTN` 前按新的 CPL 更新 `PRMD.PPLV`。快速路径
不能完成的情况沿用软件 MMU 或翻译状态恢复路径，保留 x86 异常语义。

关键实现见 `target/i386/latx/optimization/sys-hamt-dmw.c`、
`target/i386/latx/optimization/sys-hamt-excp.c`、`target/i386/hamt/hamt-ptw.c`、
`target/i386/hamt/hamt-tlb.c` 和 `target/i386/hamt/hamt.c`。
