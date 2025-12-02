/*
 * Changing DDIO State
 */

#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include<pci/pci.h>
}

#include <sys/io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>
#include <gflags/gflags.h>

#define PCI_VENDOR_ID_INTEL 0x8086
#define SKX_PERFCTRLSTS_0 0x180
#define SKX_use_allocating_flow_wr_MASK 0x80
#define SKX_nosnoopopwren_MASK 0x8

/*
 * Find the proper pci device (i.e., PCIe Root Port) based on the nic device
 * For instance, if the NIC is located on 0000:3a:00.0 (i.e., BDF)
 * 0x3a is the nic_bus (B)
 * 0x00 is the nic_device (D)
 * 0x0  is the nic_function (F)
 */

struct pci_access *pacc;

void init_pci_access(void) {
    pacc = pci_alloc(); /* Get the pci_access structure */
    pci_init(pacc);         /* Initialize the PCI library */
    pci_scan_bus(pacc); /* We want to get the list of devices */
}

static int search_count = 0;

struct pci_dev *
find_ddio_device(uint8_t nic_bus, int verbose) {
    struct pci_dev *dev;
    char namebuf[1024];

    search_count++;

    if (verbose) {
        printf("\n[SEARCH #%d] Looking for device on bus 0x%02x (searching for xx:00.0)...\n",
               search_count, nic_bus);
    }

    for (dev = pacc->devices; dev; dev = dev->next) {
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_NUMA_NODE | PCI_FILL_PHYS_SLOT | PCI_FILL_CLASS);

        // Log all devices on this bus for debugging
        if (verbose && dev->bus == nic_bus) {
            const char *device_name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
                                                      PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
            printf("[DEBUG] Found device: %02x:%02x.%x - Class: 0x%04x - %s\n",
                   dev->bus, dev->dev, dev->func, dev->device_class, device_name);
        }

        if (dev->func == 0 && dev->dev == 0 && dev->bus == nic_bus) {
            if (verbose) {
                const char *device_name = pci_lookup_name(pacc, namebuf, sizeof(namebuf),
                                                          PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
                printf("\n[FOUND] Selected device: %02x:%02x.%x\n", dev->bus, dev->dev, dev->func);
                printf("        Device Class: 0x%04x", dev->device_class);

                // Decode device class
                uint16_t class_code = dev->device_class >> 8;
                if (class_code == 0x02) {
                    printf(" (Network Controller)\n");
                } else if (class_code == 0x06) {
                    uint16_t subclass = dev->device_class & 0xFF;
                    if (subclass == 0x04) {
                        printf(" (PCI Bridge/Root Port)\n");
                    } else {
                        printf(" (PCI Bridge - subclass 0x%02x)\n", subclass);
                    }
                } else if (class_code == 0x08) {
                    printf(" (System Peripheral - IOMMU/VT-d)\n");
                } else {
                    printf(" (Class 0x%02x)\n", class_code);
                }

                printf("        Device Name: %s\n", device_name);

                // Check if this is actually a PCIe Root Port (class 0x06, subclass 0x04)
                uint16_t subclass = dev->device_class & 0xFF;
                if (class_code == 0x06 && subclass == 0x04) {
                    printf("        ✓ CORRECT: This is a PCI Bridge/Root Port\n");
                    printf("        ✓ DDIO register should be here!\n");
                } else if (class_code == 0x08) {
                    printf("        ✗ WRONG: This is a System Peripheral (IOMMU/VT-d)\n");
                    printf("        ✗ Expected: Class 0x06 Subclass 0x04 (PCI Bridge/Root Port)\n");
                    printf("        ✗ This is NOT a PCIe Root Port!\n");
                } else if (class_code == 0x02) {
                    printf("        ✗ WRONG: This is the NIC itself (Network Controller)\n");
                    printf("        ✗ Expected: Class 0x06 Subclass 0x04 (PCI Bridge/Root Port)\n");
                    printf("        ✗ DDIO register is NOT in the NIC!\n");
                    printf("        ✗ Should search parent bus (0x%02x) instead!\n", dev->bus - 1);
                } else {
                    printf("        ✗ WRONG: Unexpected device class 0x%02x/0x%02x\n", class_code, subclass);
                    printf("        ✗ Expected: Class 0x06 Subclass 0x04 (PCI Bridge/Root Port)\n");
                }
            }

            return dev;
        }
    }
    printf("\n[ERROR] Could not find device at bus %02x:00.0\n", nic_bus);
    return NULL;
}

/*
 * perfctrlsts_0
 * bit 3: NoSnoopOpWrEn -> Should be 1b
 * bit 7: Use_Allocating_Flow_Wr -> Should be 0b
 * Check p. 68 of IntelÂ® XeonÂ® Processor Scalable Family
 * Datasheet, Volume Two: Registers
 * May 2019
 * link: https://cdrdv2-public.intel.com/614073/614073_Intel%C2%AE%20Xeon%C2%AE%20Processor%20Scalable%20Fam_v005.pdf
 */
int ddio_status(uint8_t nic_bus) {
    uint32_t val;
    if (!pacc)
        init_pci_access();

    struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
    if (!dev) {
        printf("No device found!\n");
        exit(1);
    }
    val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
    printf("perfctrlsts_0 val: 0x%" PRIx32 "\n", val);
    printf("NoSnoopOpWrEn val: 0x%" PRIx32 "\n", val & SKX_nosnoopopwren_MASK);
    printf("Use_Allocating_Flow_Wr val: 0x%" PRIx32 "\n", val & SKX_use_allocating_flow_wr_MASK);
    if (val & SKX_use_allocating_flow_wr_MASK)
        return 1;
    else
        return 0;
}

void ddio_enable(uint8_t nic_bus) {
    uint32_t val, val_after;
    if (!pacc)
        init_pci_access();

    if (!ddio_status(nic_bus)) {
        struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
        if (!dev) {
            printf("No device found!\n");
            exit(1);
        }

        printf("\n[ENABLE] Attempting to enable DDIO...\n");
        val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[ENABLE] Register 0x180 BEFORE write: 0x%08x\n", val);
        printf("[ENABLE]   - bit 7 (DDIO): %s\n", (val & 0x80) ? "ON" : "OFF");

        pci_write_long(dev, SKX_PERFCTRLSTS_0, val | SKX_use_allocating_flow_wr_MASK);

        val_after = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[ENABLE] Register 0x180 AFTER write:  0x%08x\n", val_after);
        printf("[ENABLE]   - bit 7 (DDIO): %s\n", (val_after & 0x80) ? "ON" : "OFF");

        if ((val_after & 0x80) == 0) {
            printf("[ENABLE] ✗ FAILED: Register write did not take effect!\n");
            printf("[ENABLE]    This likely means we're writing to the wrong device.\n");
        } else {
            printf("[ENABLE] ✓ SUCCESS: DDIO is now enabled!\n");
        }
    } else {
        printf("DDIO was already enabled!\n");
    }
}

void ddio_disable(uint8_t nic_bus) {
    uint32_t val, val_after;
    if (!pacc)
        init_pci_access();

    if (ddio_status(nic_bus)) {
        struct pci_dev *dev = find_ddio_device(nic_bus, 0);  // quiet mode
        if (!dev) {
            printf("No device found!\n");
            exit(1);
        }

        printf("\n[DISABLE] Attempting to disable DDIO...\n");
        val = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[DISABLE] Register 0x180 BEFORE write: 0x%08x\n", val);
        printf("[DISABLE]   - bit 7 (DDIO): %s\n", (val & 0x80) ? "ON" : "OFF");

        pci_write_long(dev, SKX_PERFCTRLSTS_0, val & (~SKX_use_allocating_flow_wr_MASK));

        val_after = pci_read_long(dev, SKX_PERFCTRLSTS_0);
        printf("[DISABLE] Register 0x180 AFTER write:  0x%08x\n", val_after);
        printf("[DISABLE]   - bit 7 (DDIO): %s\n", (val_after & 0x80) ? "ON" : "OFF");

        if ((val_after & 0x80) != 0) {
            printf("[DISABLE] ✗ FAILED: Register write did not take effect!\n");
            printf("[DISABLE]    This likely means we're writing to the wrong device.\n");
        } else {
            printf("[DISABLE] ✓ SUCCESS: DDIO is now disabled!\n");
        }
    } else {
        printf("DDIO was already disabled\n");
    }
}

void print_dev_info(struct pci_dev *dev) {
    if (!dev) {
        printf("No device found!\n");
        exit(1);
    }
    unsigned int c;
    char namebuf[1024], *name;
    printf("========================\n");
    printf("%04x:%02x:%02x.%d vendor=%04x device=%04x class=%04x irq=%d (pin %d) base0=%lx \n",
           dev->domain, dev->bus, dev->dev, dev->func, dev->vendor_id, dev->device_id,
           dev->device_class, dev->irq, c, (long) dev->base_addr[0]);
    name = pci_lookup_name(pacc, namebuf, sizeof(namebuf), PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
    printf(" (%s)\n", name);
    printf("========================\n");
}

/* Define nic_bus and ddio_state */
DEFINE_bool(enable, false, "Enable or Disable DDIO");
DEFINE_uint32(nic_bus, 0x3a, "NIC bus number");

/**
 * Usage: ./ddio_modify --enable=true --nic_bus=0x3a
 * Please
 */

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_pci_access();

    struct pci_dev *dev = find_ddio_device(FLAGS_nic_bus, 1);  // verbose mode
    print_dev_info(dev);

    if (FLAGS_enable) {
        ddio_enable(FLAGS_nic_bus);
    } else {
        ddio_disable(FLAGS_nic_bus);
    }

    pci_cleanup(pacc); /* Close everything */
    return 0;
}