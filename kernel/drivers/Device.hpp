#pragma once
#include <types.h>

/* C++ device/driver framework: PCI devices get matched against registered
 * drivers (drivers/Driver.cpp owns the registry). */

class Device {
public:
    Device(u8 bus, u8 slot, u8 fn, u16 vendor, u16 devid, u8 base_class)
        : bus_(bus), slot_(slot), fn_(fn), vendor_(vendor), devid_(devid),
          base_class_(base_class)
    {
    }
    virtual ~Device() {}

    u8 bus() const { return bus_; }
    u8 slot() const { return slot_; }
    u8 fn() const { return fn_; }
    u16 vendor() const { return vendor_; }
    u16 devid() const { return devid_; }
    u8 base_class() const { return base_class_; }

    virtual const char *name() const = 0;

private:
    u8 bus_, slot_, fn_;
    u16 vendor_, devid_;
    u8 base_class_;
};

class Driver {
public:
    explicit Driver(const char *name) : name_(name) {}
    virtual ~Driver() {}
    const char *name() const { return name_; }

    /* Return a bound Device when this driver handles the hardware. */
    virtual Device *probe(u8 bus, u8 slot, u8 fn, u16 vendor, u16 devid,
                          u8 base_class) = 0;

private:
    const char *name_;
};

void driver_register(Driver *d);
Device *driver_dispatch_pci(u8 bus, u8 slot, u8 fn, u16 vendor, u16 devid,
                            u8 base_class);
