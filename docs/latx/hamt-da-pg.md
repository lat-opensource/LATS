# HAMT DA/PG 模式

## 寻址模型

HAMT DMW 有两种子模式。旧版子模式 0 通过在 guest ASID 和 ASID 0 之间切换，
区分翻译代码与运行时的地址空间。子模式 1 则让翻译器运行时使用 LoongArch
直接地址模式（DA），让 x86 guest 地址空间使用分页模式（PG）。这样，普通的
guest 虚拟地址读写仍走硬件 TLB，进入或离开运行时时也不必仅为此切换 ASID。

| 执行上下文 | CRMD.DA | CRMD.PG | 寻址方式 |
| --- | --- | --- | --- |
| 运行时和 C helper | 1 | 0 | 直接寻址，ASID 不参与地址转换 |
| 执行 guest 翻译代码 | 0 | 1 | 由 ASID 选择 guest 虚拟地址转换上下文 |

在 PG 模式下，翻译代码仍可通过 DMW 别名访问翻译器自己的内存。跨模式的代码
使用对应的地址别名；切换模式并不会把 guest 虚拟地址重新解释成运行时指针。
生成的模式切换代码通过带掩码的 `CSRXCHG` 修改 `CRMD`，只更新 DA 和 PG。
特权级独立管理，见 [HAMT PLV](hamt-plv.md)。

## 模式切换与地址空间

- 使用 DMW 子模式 1 时，KVM 将 HAMT vCPU 初始化为 DA。
- 从本机运行时代码进入翻译代码，或从 helper 返回时，先恢复 PG 再继续执行翻译代码。
  调用 helper 或退出到运行时时切换到 DA。
- 同步例外和定时器中断进入 C 前保存 `CRMD` 并切换到 DA。普通 `PRMD` 不保存
  DA/PG，正常返回被打断的代码时从已保存的 `CRMD` 恢复这两位；例如定时器中断
  若打断了处于 DA 的 helper，返回时仍是 DA。
  若处理函数调用 `cpu_loop_exit()`，则不恢复故障指令的 DA/PG，而是在原有 `PRMD`
  恢复后、`ERTN` 前将当前 `CRMD` 的 DA/PG 设置为 DA，不额外修改 IE。
  纯汇编完成的 TLB 重填不走这条 C 路径。
- `hamt_switch_pcid()` 在 DA 模式下选定 guest ASID 并写入硬件，供下一次进入 PG 时使用。
  旧版 DMW 仍使用显式切换 guest ASID 和 ASID 0 的进出 helper。DA/PG 模式下，
  填写 guest TLB 不会顺带把 ASID 恢复为 0。

`hamtmode=1,hamtsubmode=1` 选择此模式，也是系统模拟版本的默认配置。
`hamtsubmode=0` 选择旧版 DMW。启用 DA/PG 本身不会启用 PLV，也不会决定使用哪种重填路径。

## 代码位置

- `target/i386/hamt/dune-kvm-init-csr.c`：初始化 vCPU 的 CSR 状态。
- `target/i386/latx/optimization/sys-hamt-dmw.c`：生成模式切换和旧版 ASID 切换代码。
- `target/i386/latx/translator/sys-helper.c`、
  `target/i386/latx/optimization/sys-staticcs.c` 和
  `target/i386/latx/optimization/sys-hamt-excp.c`：跨模式的调用和返回路径。
- `target/i386/hamt/hamt-tlb.c`：选择 guest ASID 并维护 TLB。
