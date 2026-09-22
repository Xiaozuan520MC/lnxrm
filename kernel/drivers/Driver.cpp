/* Driver registry + the AHCI driver object. The heavy lifting lives in
 * drivers/ahci.c (C); this C++ layer owns discovery & lifetime. */
#include "Device.hpp"
#include <pci.h>
#include <console.h>
#include <List.hpp>

extern "C" {
int ahci_probe_fn(void *ctx, u8 bus, u8 dev, u8 fn, u16 vendor, u16 devid,
                  u8 base_class);
}

class AhciDevice : public Device {
public:
    using Device::Device;
    const char *name() const override { return "sata-ahci"; }
};

class AhciDriver final : public Driver {
public:
    AhciDriver() : Driver("ahci") {}

    Device *probe(u8 bus, u8 slot, u8 fn, u16 vendor, u16 devid,
                  u8 base_class) override
    {
        if (base_class != 1)
            return nullptr;
        u32 sub = pci_read(bus, slot, fn, 0x08);
        if (((sub >> 16) & 0xFF) != 6)
            return nullptr;
        if (!ahci_probe_fn(nullptr, bus, slot, fn, vendor, devid, base_class))
            return nullptr;
        return new AhciDevice(bus, slot, fn, vendor, devid, base_class);
    }
};

struct DriverNode {
    IntrusiveNode<DriverNode> link;
    Driver *drv = nullptr;
};

static IntrusiveList<DriverNode, &DriverNode::link> g_drivers;
static SimpleVec<Device *> g_devices;

void driver_register(Driver *d)
{
    DriverNode *n = new DriverNode;
    n->drv = d;
    g_drivers.push_front(n);
}

Device *driver_dispatch_pci(u8 bus, u8 slot, u8 fn, u16 vendor, u16 devid,
                            u8 base_class)
{
    Device *found = nullptr;
    g_drivers.for_each([&](DriverNode *n) {
        if (found)
            return;
        Device *dev =
            n->drv->probe(bus, slot, fn, vendor, devid, base_class);
        if (dev) {
            kprintf("[drv] %s bound to %02x:%02x.%d\n", n->drv->name(), bus,
                    slot, fn);
            g_devices.push_back(dev);
            found = dev;
        }
    });
    return found;
}
