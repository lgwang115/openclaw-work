# 编译与部署（针对 linux-6.6.64-rt47）

完整板测流程、平台坑与实测数据见 **`BRINGUP.md`**。本文只覆盖编译、拷板、加载顺序与常见编译/加载错误。

## 推荐：装进内核树再编（能链上 BST 门铃符号）

在开发机上：

```bash
# 0. 已确认
#    make -s ARCH=arm64 kernelrelease  →  6.6.64-rt47
#    modules_prepare 已跑过（没有就先跑）
#    若缺 Module.symvers：从板子拷到 $KDIR/

cd /path/to/pcie-epf-infer-minlat
chmod +x install-into-kernel.sh

./install-into-kernel.sh /home/bst/workspace/linux-6.6
```

脚本会：

1. 把 `pci_epf_infer.c` / `infer_proto.h` 拷到 `drivers/pci/endpoint/functions/`
2. 把 `infer_rc.c` / `infer_proto.h` 拷到 `drivers/misc/`
3. 在对应 `Makefile` 里追加 `obj-m += ...`
4. 编出：
   - `.../functions/pci_epf_infer.ko`
   - `.../misc/infer_rc.ko`
   - `./inferlat`

为什么要进树编：`bst_pcie_ep_db_*` 多半没有 `EXPORT_SYMBOL`，`pci_epf_test` 是 `=y` 内置所以能调；外部 `.ko` 会报 `Unknown symbol`。

## 手动编译（等价于脚本）

```bash
KDIR=~/workspace/linux-6.6
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# EP
cp pci_epf_infer.c infer_proto.h $KDIR/drivers/pci/endpoint/functions/
grep -q pci_epf_infer.o $KDIR/drivers/pci/endpoint/functions/Makefile || \
  echo 'obj-m += pci_epf_infer.o' >> $KDIR/drivers/pci/endpoint/functions/Makefile
make -C $KDIR M=drivers/pci/endpoint/functions pci_epf_infer.ko

# RC
cp infer_rc.c infer_proto.h $KDIR/drivers/misc/
grep -q infer_rc.o $KDIR/drivers/misc/Makefile || \
  echo 'obj-m += infer_rc.o' >> $KDIR/drivers/misc/Makefile
make -C $KDIR M=drivers/misc infer_rc.ko

# 用户态（需能找到同目录的 infer_proto.h）
${CROSS_COMPILE}gcc -O2 -Wall -o inferlat inferlat.c
```

## 拷到板子

拓扑对称：**A/B 两板都要** `pci_epf_infer.ko` + `infer_rc.ko` + `inferlat`。

```bash
KDIR=~/workspace/linux-6.6
for IP in <A_IP> <B_IP>; do
  ssh root@$IP 'mkdir -p /userdata/ep_test'
  scp $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
      $KDIR/drivers/misc/infer_rc.ko ./inferlat \
      root@$IP:/userdata/ep_test/
done

# 建议核对 md5，避免板子上还是旧 ko
md5sum $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
       $KDIR/drivers/misc/infer_rc.ko
ssh root@<A_IP> 'md5sum /userdata/ep_test/*.ko'
ssh root@<B_IP> 'md5sum /userdata/ep_test/*.ko'
```

## 成对重启后的加载顺序

1. **两板一起 reboot**（软 remove/rescan 不够恢复 EP BAR 掩码时）
2. **两板同时**做本板 EP init + `echo 1 > /dev/pci_epf_infer_ctl`
3. 确认两边 dmesg 都有 `ctrl BAR1 programmed` 和 `reprogram done magic=0x494e4652`
4. **再**各自 remove/rescan 对端设备并 `insmod infer_rc.ko`
5. `./inferlat /dev/infer_rc0 w 100` 与 `r 100`

### EP 侧

```bash
insmod /userdata/ep_test/pci_epf_infer.ko
ls /dev/pci_epf_infer_ctl

cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
sleep 1
echo 1 > /dev/pci_epf_infer_ctl
dmesg | grep -E 'reprogram|ctrl BAR|eDMA' | tail -5
# 期望: ctrl BAR1 programmed ...
#       eDMA channels: tx=dma1chan0 rx=dma1chan8
#       reprogram done magic=0x494e4652 db=0:0xe00
```

### RC 侧（对端 EP 已 ready）

```bash
rmmod infer_rc 2>/dev/null
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null || true
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1
lspci -n -s 01:00.0
# 期望: 1ef1:0301

cd /userdata/ep_test
insmod ./infer_rc.ko
ls /dev/infer_rc0
dmesg | grep infer_rc | tail -5
# 期望: infer regs on BAR1 ...
#       doorbell mapped BAR0+0xe00
#       infer_rc ready ... /dev/infer_rc0

./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
```

### 关于 `busybox devmem` 验 magic

- 控制面在 **BAR1**（`resource` 第 2 行），不是 BAR0。
- **`insmod infer_rc` 之前**设备通常尚未打开 Memory Space，对 BAR1 做 `devmem` 也常得到 `0xFFFFFFFF`——**这不代表 magic 没写上**。
- 以 `infer_rc` 的 dmesg 为准：`infer regs on BAR1` 即成功。若一定要手动读，先 `insmod`（或手动 enable 设备）后再 `devmem`。

## 常见错误

| 现象 | 原因 |
| --- | --- |
| `Unknown symbol bst_pcie_ep_db_*` | 用了纯 out-of-tree；改用 `install-into-kernel.sh` |
| `disagrees about version of symbol` | `kernelrelease` 不是 `6.6.64-rt47` |
| `Module.symvers is missing` | 从板子拷 `Module.symvers` 到 `$KDIR/` |
| `BAR0 magic=0x0` / probe `-22` | **旧模块把 ctrl 放在 BAR0**；更新后应在 **BAR1**。BAR0 读 0/FFFFFFFF 正常 |
| `devmem` BAR1 = `0xFFFFFFFF`，但随后 `infer_rc` 成功 | enable 前读无效；忽略，以 dmesg 为准 |
| `no BAR with magic` 且未 `echo 1 > ctl` | BST `start` 清掉了 BAR/ATU；必须 post-start reprogram |
| `remove: No such file or directory` | 设备尚未枚举，可忽略后直接 `rescan` |
| Region 全是 `[disabled]` 且 Command 无 Memory | 等 `infer_rc` 的 `pcim_enable_device`（日志 `enabling device`） |
| 成对重启后 BAR 尺寸异常 | 软 remove/rescan 不够；两板一起 reboot 再枚举 |
| 板子 ko 行为像旧版 | md5 与开发机不一致；重新 scp |
