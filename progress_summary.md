# DDIO Modify - Work Summary and Issues Found

**Date:** 2025-11-20
**Machine:** /users/inhochoi/ddio-modify
**Platform:** Linux 6.8.0-71-generic (Intel Ice Lake CPU)

---

## Current DDIO Status

### 1. MSR 0xc8b (LLC Ways Allocation)
```bash
sudo rdmsr 0xc8b
# Result: 0xc00 (0b1100 0000 0000)
```
- **Purpose:** Controls which LLC (Last Level Cache) ways DDIO can use
- **Current value:** 0xc00 (bit 10, 11 set)
- **Valid range:** 0x600 ~ 0x7ff
- **Status:** LLC ways are configured

### 2. PCI Register 0x180 bit 7 (DDIO Enable/Disable)

#### Mellanox ConnectX-6 Dx NIC (51:00.0)
- **Root Port:** 50:02.0 (Intel Ice Lake PCI Express Root Port A)
```bash
sudo setpci -s 50:02.0 0x180.l
# Result: 0x1901000b
# Binary: 0b0001 1001 0000 0001 0000 0000 0000 1011
# Lower byte: 0x0b = 0b00001011
# bit 7 = 0 → DDIO DISABLED ❌
# bit 3 = 1 → NoSnoopOpWrEn enabled
```

#### Intel X550 NIC (01:00.0)
- **Root Port:** 00:1c.0 (C620 Series Chipset PCI Express Root Port #1)
```bash
sudo setpci -s 00:1c.0 0x180.l
# Result: 0x00000000
# This chipset may not support or use different register
```

**Conclusion:** DDIO is currently DISABLED on the Mellanox NIC path

---

## Critical Issues Found

### Issue #1: Code Finds Wrong Device

**Problem:** The `find_ddio_device()` function searches for device at `bus:00.0` (func=0, dev=0), but actual PCIe Root Ports are at different addresses.

**Code Location:** ddio-modify.cpp:42-52
```cpp
struct pci_dev *find_ddio_device(uint8_t nic_bus) {
    for (dev = pacc->devices; dev; dev = dev->next) {
        if (dev->func == 0 && dev->dev == 0 && dev->bus == nic_bus) {
            return dev;
        }
    }
}
```

**What it finds vs What it should find:**

| NIC Bus | Code finds (xx:00.0) | Actual Root Port | Device Type |
|---------|---------------------|------------------|-------------|
| 0x50 | 50:00.0 | **50:02.0** | VT-d (wrong) vs Root Port (correct) |
| 0x00 | 00:00.0 | **00:1c.0** | VT-d (wrong) vs Root Port (correct) |
| 0x51 | 51:00.0 | **50:02.0** | NIC itself (wrong) vs Root Port (correct) |

### Issue #2: Register Writes Fail Silently

**Evidence from debug output:**
```bash
sudo ./build/ddio_modify --enable=true --nic_bus=0x50

# Output:
[DEBUG] Before write: 0xd0ffc001
[DEBUG] After write:  0xd0ffc001  # ← No change!
[DEBUG] Bit 7 value: 0x0           # ← Still 0
DDIO is enabled!                   # ← False claim
```

**Root cause:** Writing to wrong device (VT-d device instead of PCIe Root Port)

### Issue #3: Machine-Specific Device Addresses

The code assumes a specific PCIe topology, but this varies by machine:
- **Current machine:** Mellanox NIC at 51:00.0, Root Port at 50:02.0
- **Readme example:** NIC at unknown bus, assumes Root Port at 0x3a:00.0
- **Other machines:** Will have different PCIe hierarchy

---

## How to Verify DDIO Settings

### Method 1: Using MSR (LLC Ways)
```bash
# Install tools
sudo apt install msr-tools
sudo modprobe msr

# Check LLC ways allocation
sudo rdmsr 0xc8b
# Expected: value between 0x600 and 0x7ff

# Modify LLC ways
sudo wrmsr -a 0xc8b 0x600
```

### Method 2: Using PCI Register (Enable/Disable)
```bash
# Find your NIC and its root port
lspci | grep -i ethernet
lspci -s <NIC_BUS>:00.0 -vvv | grep "Bus:"

# Read DDIO register from root port
sudo setpci -s <ROOT_PORT> 0x180.l

# Check bit 7 of the result:
# bit 7 = 0 → DDIO disabled
# bit 7 = 1 → DDIO enabled
```

### Method 3: Using Current Code (Unreliable)
```bash
cd /users/inhochoi/ddio-modify/build
sudo ./ddio_modify --nic_bus=0x50

# Look at "Use_Allocating_Flow_Wr val"
# 0x0 = disabled
# 0x80 = enabled
```
**Note:** Current code may find wrong device, results are unreliable!

---

## PCIe Topology of Current Machine

```
00:00.0 - Intel Ice Lake Memory Map/VT-d
00:1c.0 - PCI Express Root Port #1
  └─ 01:00.0 - Intel X550 Ethernet Controller
  └─ 01:00.1 - Intel X550 Ethernet Controller

50:00.0 - Intel Ice Lake Memory Map/VT-d
50:02.0 - Ice Lake PCI Express Root Port A
  └─ 51:00.0 - Mellanox ConnectX-6 Dx  ← Target NIC
  └─ 51:00.1 - Mellanox ConnectX-6 Dx

89:00.0 - Intel Ice Lake Memory Map/VT-d
89:02.0 - Ice Lake PCI Express Root Port A
  └─ 8a:00.0 - Mellanox ConnectX-6 Lx
  └─ 8a:00.1 - Mellanox ConnectX-6 Lx
```

---

## What Needs to be Fixed

### Priority 1: Fix Device Discovery
**File:** ddio-modify.cpp:42-52

**Current logic:**
```cpp
if (dev->func == 0 && dev->dev == 0 && dev->bus == nic_bus)
```

**Options to fix:**
1. **Search for actual Root Port:** Look for PCI bridge device class (0x060400)
2. **Parse PCIe topology:** Use `lspci` to find parent bridge of NIC
3. **Accept explicit root port address:** Add `--root_port` parameter
4. **Search by device/function:** Try multiple addresses (00.0, 02.0, 1c.0, etc.)

### Priority 2: Verify Register Writes
**File:** ddio-modify.cpp:83-127

Already added debug output (not committed):
```cpp
val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
printf("[DEBUG] Before write: 0x%" PRIx32 "\n", val);
pci_write_long(dev, SKX_PERFCTRLSTS_0, val | SKX_use_allocating_flow_wr_MASK);
val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
printf("[DEBUG] After write: 0x%" PRIx32 "\n", val);
```

### Priority 3: Better Error Handling
- Check if register write succeeded
- Warn user if device type is not a PCI bridge
- Validate that device supports PERFCTRLSTS_0 register

### Priority 4: Documentation
- Update Readme.md with correct usage for Ice Lake platform
- Document how to find correct root port address
- Add troubleshooting section

---

## Code Changes Made (Not Committed)

**Modified file:** ddio-modify.cpp

**Changes:**
- Added debug printf statements in `ddio_enable()` function (lines 95-99)
- Added debug printf statements in `ddio_disable()` function (lines 118-122)
- These show before/after register values and bit 7 status

**To see changes:**
```bash
git diff ddio-modify.cpp
```

**Status:** NOT committed to git

---

## Recommended Next Steps

1. **Fix the device discovery logic** to find actual PCIe Root Ports
2. **Test on current machine** (Mellanox NIC at 51:00.0)
3. **Verify register writes succeed** (bit 7 actually changes)
4. **Test enable/disable cycle:**
   - Enable DDIO → verify bit 7 = 1
   - Disable DDIO → verify bit 7 = 0
   - Enable again → verify bit 7 = 1
5. **Document the correct PCIe topology detection method**
6. **Make code portable** across different Intel platforms

---

## Testing Commands for Next Session

```bash
# 1. Build project
cd /users/inhochoi/ddio-modify/build
cmake .. && make

# 2. Find your NIC's root port
lspci | grep -i ethernet
lspci -tv | grep -A 5 -B 5 "<YOUR_NIC_BUS>"

# 3. Manually verify DDIO status
sudo setpci -s <ROOT_PORT> 0x180.l
# Example: sudo setpci -s 50:02.0 0x180.l

# 4. Try to enable DDIO (will fail with current code)
sudo ./ddio_modify --enable=true --nic_bus=0x50

# 5. Verify nothing changed
sudo setpci -s 50:02.0 0x180.l
```

---

## Key Takeaways

1. **DDIO has two settings:**
   - MSR 0xc8b: Controls LLC ways (currently 0xc00) ✓
   - PCI reg 0x180 bit 7: Enable/disable (currently 0 = disabled) ❌

2. **Current code is broken:**
   - Finds wrong device (VT-d instead of Root Port)
   - Register writes fail silently
   - Claims success but nothing changes

3. **Fix requires:**
   - Better device discovery (find actual PCIe Root Ports)
   - Verify writes succeed
   - Handle different PCIe topologies per machine

4. **This is machine-specific:**
   - Different machines have different PCIe layouts
   - Root port addresses are not predictable
   - Code needs to discover topology dynamically
