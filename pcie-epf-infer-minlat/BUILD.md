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

```bash
scp $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko root@<EP_IP>:/userdata/ep_test/
scp $KDIR/drivers/misc/infer_rc.ko ./inferlat root@<RC_IP>:/userdata/ep_test/
```

## EP 板加载

```bash
# 两板成对重启后，先做 EP 初始化（不要再用 pci_epf_test）
insmod /userdata/ep_test/pci_epf_infer.ko

cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start

dmesg | tail -20
# 期望: doorbell bar=... off=0x... 以及 DMA ready / prealloc
```

## RC 板加载

```bash
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1
lspci -n -s 01:00.0
# 期望: 1ef1:0301

insmod /userdata/ep_test/infer_rc.ko
ls /dev/infer_rc0

./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
```

## 常见错误

| 现象 | 原因 |
| --- | --- |
| `Unknown symbol bst_pcie_ep_db_*` | 用了纯 out-of-tree；改用 `install-into-kernel.sh` |
| `disagrees about version of symbol` | `kernelrelease` 不是 `6.6.64-rt47` |
| `No such device` / magic 不对 | EP 未 start，或仍绑着 pci_epf_test |
| DMA channel 失败 | 需按 pci-epf-test 的 filter 微调（见 dmesg） |
