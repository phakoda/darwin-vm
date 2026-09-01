# Running darwin-vm entirely on Linux

`darwin-vm` can build and run QEMU on Linux. This branch also makes the firmware-preparation steps (`get_files.sh` and `fix_perms.sh`) work on Linux, so a Mac is no longer required for the normal root-shell workflow.

The Linux path is based on the implementation and end-to-end testing in jprx/darwin-vm pull request #1. That PR reported successful `get_files.sh -> fix_perms.sh -> run.sh` boots for both `iPhone17,3` and `Mac16,10` guests on Ubuntu 24.04 x86_64.

## Host dependencies

Install the normal QEMU build dependencies plus `jq`, `wget`, Python 3, and these Linux-specific tools:

### ipsw

Build `ipsw` from source:

```bash
git clone https://github.com/blacktop/ipsw.git
cd ipsw
go build -o ipsw ./cmd/ipsw
sudo install -m 0755 ipsw /usr/local/bin/ipsw
```

### ldid

`ldid` replaces macOS `codesign` for the ad-hoc signing and CDHash extraction used by the iOS ramdisk preparation path.

```bash
sudo apt-get install -y libplist-dev
git clone https://github.com/ProcursusTeam/ldid.git
cd ldid
make
sudo install -m 0755 ldid /usr/local/bin/ldid
```

### linux-apfs-rw

The recovery `ramdisk.dmg` is a raw APFS container. Linux needs the out-of-tree `linux-apfs-rw` driver to modify it.

```bash
sudo apt-get install -y linux-headers-$(uname -r)
git clone https://github.com/linux-apfs/linux-apfs-rw.git
cd linux-apfs-rw
make
sudo modprobe libcrc32c
sudo insmod apfs.ko
```

The APFS driver's write support is experimental. Keep the original IPSW URL handy and treat generated `firmware/` files as disposable/rebuildable artifacts. The module must also be loaded again after a reboot unless you install it persistently yourself.

## Prepare firmware

From the repository root:

```bash
./get_files.sh
./fix_perms.sh firmware/ramdisk.dmg
```

For another device or IPSW:

```bash
DEVNAME="iPhone17,3" URL="https://example.invalid/Restore.ipsw" ./get_files.sh
./fix_perms.sh firmware/ramdisk.dmg
```

On Linux, `get_files.sh` uses:

- `linux-apfs-rw` instead of `hdiutil` to mount `ramdisk.dmg` read/write;
- `cp -a --remove-destination` instead of `ditto` when merging the iOS sysroot, because the APFS driver's experimental write path does not support the normal truncate-in-place overwrite behavior;
- `ldid` instead of `codesign` for ad-hoc signing and CDHash extraction.

`fix_perms.sh` writes numeric uid/gid `0:0`, which is equivalent to macOS `root:wheel` for the ownership checks relevant to the guest.

## Build QEMU

```bash
git submodule update --init
cd qemu-sptm
mkdir -p build
cd build
../configure --target-list=aarch64-softmmu
make -j"$(nproc)"
cd ../..
```

## Boot

```bash
./run.sh
```

The expected milestone for this Linux-support phase is the same as the current project baseline: Darwin reaches the root shell. Screen, GPU acceleration, Wi-Fi, Bluetooth, GUI apps, and SpringBoard are separate device-emulation milestones that build on top of this host-portability foundation.
