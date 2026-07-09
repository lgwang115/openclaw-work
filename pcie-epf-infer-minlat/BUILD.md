# 编译与部署（针对 linux-6.6.64-rt47）

## 推荐：装进内核树再编（能链上 BST 门铃符号）

在开发机上：

```bash
# 0. 已确认
#    make -s ARCH=arm64 kernelrelease  →  6.6.64-rt47
#    modules_prepare 已跑过（没有就先跑）

cd /path/to/pcie-epf-infer-minlat
chmod +x install-into-kernel.sh

./install-into-kernel.sh /home/bst/workspace/linux-6.6
```

脚本会：

1. 把 `pci_epf_infer.c` / `infer_proto.h` 拷到 `drivers/pci/endpoint/functions/`
2. 把 `infer_rc.c` / `infer_proto.h` 拷到 `drivers/misc/`
3. 在对应 `Makefile` 里追加 `obj-m += ...`
4. 编出：
   - `.../functions/pci_epf_infer.ko`  → 装 **EP 板**
   - `.../misc/infer_rc.ko`           → 装 **RC 板**
   - `./inferlat`                     → RC 板用户态

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

# 用户态
${CROSS_COMPILE}gcc -O2 -Wall -o inferlat inferlat.c
```

## 拷到板子

**两板都要更新 EP + RC 模块**（拓扑是双向的）：

```bash
scp $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
    $KDIR/drivers/misc/infer_rc.ko ./inferlat \
    root@<A_IP>:/userdata/ep_test/
scp $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
    $KDIR/drivers/misc/infer_rc.ko ./inferlat \
    root@<B_IP>:/userdata/ep_test/
```

## 成对重启后的加载顺序

1. **两板同时**做本板 EP init + `echo 1 > /dev/pci_epf_infer_ctl`
2. 确认两边 dmesg 都有 `ctrl BAR1 programmed` 和 `reprogram done magic=0x494e4652`
3. **再**各自 remove/rescan 对端设备并 `insmod infer_rc.ko`

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
dmesg | grep -E 'reprogram|ctrl BAR' | tail -5
```

### RC 侧（对端 EP 已 ready）

```bash
rmmod infer_rc 2>/dev/null
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1
lspci -n -s 01:00.0
# 期望: 1ef1:0301

# 验 magic：读 BAR1（resource 第 2 行），不是 BAR0
BAR1=$(awk 'NR==2 {print $1}' /sys/bus/pci/devices/0000:01:00.0/resource)
# 去掉前导零后给 busybox，例如 0x0000000900b00000 → 0x900b00000
busybox devmem $BAR1 32
# 期望: 0x494E4652

insmod /userdata/ep_test/infer_rc.ko
ls /dev/infer_rc0
./inferlat /dev/infer_rc0 w 100
```

## 常见错误

| 现象 | 原因 |
| --- | --- |
| `Unknown symbol bst_pcie_ep_db_*` | 用了纯 out-of-tree；改用 `install-into-kernel.sh` |
| `disagrees about version of symbol` | `kernelrelease` 不是 `6.6.64-rt47` |
| `Module.symvers is missing` | 从板子拷 `Module.symvers` 到 `$KDIR/` |
| `BAR0 magic=0x0` / 全 BAR magic 不对 | **旧模块把 ctrl 放在 BAR0**；更新后应在 **BAR1** 看到 magic。BAR0 读 0/FFFFFFFF 正常 |
| `devmem` 得到 `0xFFFFFFFF` 且地址是 BAR0 | 用错了 BAR；改读 resource **第 2 行**（BAR1） |
| `no BAR with magic` 且未 `echo 1 > ctl` | BST `start` 清掉了 BAR/ATU；必须 post-start reprogram |
| Region 全是 `[disabled]` 且 Command 无 Memory | 先 `setpci` retrain / 等 `infer_rc` `pcim_enable_device` |
| 成对重启后 BAR 尺寸异常 | 软 remove/rescan 不够；两板一起 reboot 再枚举 |
