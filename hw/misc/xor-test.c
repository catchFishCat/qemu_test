// Create a file and add necessary #includes
/**These above includes will give access to the various APIs we will interact with to construct our device model.**/
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/register.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
/**
 * Define the model name and Err flags
 * In the above section,
 * the ERR_DEBUG logic defines a symbol for debugging but defines it to 0 to disable it by default.
 * This is useful for adding debug-only code that should be conditionally compiled in only by developers.
 * In this example, we will keep the debug on for checking what is going on when a user reads/writes to these registers.
 * **/
#ifndef XOR_TEST_ERR_DEBUG
#define XOR_TEST_ERR_DEBUG 1
#endif

#define TYPE_XOR_TEST "xlnx.xor-test"
#define XOR_TEST(obj) \
    OBJECT_CHECK(XorTestState, (obj), TYPE_XOR_TEST)

/**
 * Define registers
 * This defines some constant symbols for our two registers.
 * Note the offsets match our spec.
 * Check include/hw/register.h for the definition of REG32 macro to see exactly what it defines,
 * but it will define both register index offset as well as bus address offset for each.
 * The R_MAX definition is used to define the MATCHER register as the last register.
 * **/

REG32(XDATA, 0x0)
REG32(MATCHER, 0x4)

#define R_MAX (R_MATCHER + 1)

/**
 * Define the device state struct
 * This XorTestState is the device state.
 * The physical state of the device at any given time is captured in this struct.
 * The parent_obj field is used by QOM to implement the object-oriented inheritance.
 * We won’t use this at all - only core code uses this feature.
 * iomem and irq are our two external interfaces,
 * for the register interface and interrupt pin respectively.
 * regs is the raw state of our two registers.
 * We can index into this array directly to get either the xdata or the matcher register.
 * **/

typedef struct XorTestState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[R_MAX];
    RegisterInfo regs_info[R_MAX];
} XorTestState;

/**
 * Define irq function
 * This is a private function that our code logic can call to update the IRQ.
 * Remember from the spec that if XDATA matches MATCHER, the interrupt will assert.
 * This inspects the device state (s->regs) and causes this interrupt raise should they match.
 * **/
static void xor_test_update_irq(XorTestState *s)
{
    if (s->regs[R_XDATA] == s->regs[R_MATCHER])
    {
        qemu_irq_raise(s->irq);
    }
}

/**
 * Define the post write function for Matcher register
 * This function is going to be called after software writes to the MATCHER register.
 * It implements the needed side effects.
 * That is, as per the spec the interrupt is lowered for any write to the MATCHER register.
 * We also call xor_test_update_irq, as a change in the matcher value could now cause the matcher and xdata to match.
 * So we need to check for this condition. Note the update of s->regs[R_MATCHER] is not done here.
 * This will be done by the core register code for us.
 * **/

static void xor_test_matcher_post_write(RegisterInfo *reg, uint64_t val64)
{
    XorTestState *s = XOR_TEST(reg->opaque);

    qemu_irq_lower(s->irq);
    xor_test_update_irq(s);
}

/**
 * Define the pre-write function for Xdata register
 * This function is going to be called before software writes to the XDATA register.
 * It allows the insertion of logic involving the old value of the register as needed by the spec.
 * The argument val64 is the value as written by software.
 * In this code, we manually update Xdata as the XOR of the old value and new (as required by the spec).
 * We check xor_test_update_irq as this could cause the interrupt condition to go true.
 * We return the value written to the register as this is needed by the core code.
 * **/
static uint64_t xor_test_xdata_pre_write(RegisterInfo *reg, uint64_t val64)
{
    XorTestState *s = XOR_TEST(reg->opaque);

    s->regs[R_XDATA] = s->regs[R_XDATA] ^ val64;
    xor_test_update_irq(s);

    return s->regs[R_XDATA];
}

/**
 * Define the register block
 * This is the register block definition.
 *  It creates the register definitions for our two registers.
 *  The two register specific functions we just defined are defined as the pre/post write ops for our two registers as needed.
 *  The non-zero reset value (0xFFFFFFFF) of the MATCHER register is defined here.
 * **/
static RegisterAccessInfo xor_test_regs_info[] = {
    {
        .name = "XDATA",
        .addr = A_XDATA,
        .pre_write = xor_test_xdata_pre_write,
    },
    {
        .name = "MATCHER",
        .addr = A_MATCHER,
        .reset = 0xffffffff,
        .post_write = xor_test_matcher_post_write,
    },
};

/**
 * Define the reset function
 * This is our reset function, called when the device is reset (and at least once on machine creation).
 * The for loop instructs core code (register_reset) to reset all our register based on their defined reset values.
 * We also lower the interrupt as this makes sense on a reset.
 * **/
static void xor_test_reset(DeviceState *dev)
{
    XorTestState *s = XOR_TEST(dev);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(s->regs_info); ++i)
    {
        register_reset(&s->regs_info[i]);
    }
    qemu_irq_lower(s->irq);
}

/**
 * Define read/write handler
 * These are the MMIO (AXI) main read and write handlers.
 * They use the register_read and register_write functions to instruct core code to perform the read and write operations based on xor_test_regs_info.
 * This is standard stuff and can be copy-pasted as-is into most Xilinx device models.
 * **/
static const MemoryRegionOps xor_test_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/**
 * Define the init function
 * This is the device init function.
 * It initiates the device state when the device is created.
 * It sets up the dynamic device registers with the static config defined in xor_test_regs_info.
 * This is standard initialization and can be copy-pasted to all Xilinx devices with little changes.
 * It defines our registers interface and IRQ for use in the wider system (entity definition if you think in RTL).
 * **/
static void xor_test_init(Object *obj)
{
    XorTestState *s = XOR_TEST(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    RegisterInfoArray *reg_array;

    memory_region_init(&s->iomem, obj, TYPE_XOR_TEST,
                       R_MAX * 4);
    reg_array = register_init_block32(DEVICE(obj), xor_test_regs_info,
                                      ARRAY_SIZE(xor_test_regs_info),
                                      s->regs_info, s->regs,
                                      &xor_test_ops,
                                      XOR_TEST_ERR_DEBUG,
                                      R_MAX * 4);

    memory_region_add_subregion(&s->iomem, 0x00, &reg_array->mem);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

/**
 * Define class_init
 * The class init function defines our reset handler.
 * **/
static void xor_test_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = xor_test_reset;
}

/**
 * Define this model in an object form
 * This is the type of info defining this object in the inheritance hierarchy.
 * The interesting line is .parent, which defines this device as being a child class of the sysbus device abstraction.
 * **/
static const TypeInfo xor_test_info = {
    .name = TYPE_XOR_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XorTestState),
    .class_init = xor_test_class_init,
    .instance_init = xor_test_init,
};

/**
 * Register the model with QEMU core
 * This final logic registers the device model with the QEMU core.
 * System-level code can now lookup this device and instantiate it as an object.
 * **/
static void xor_test_register_types(void)
{
    type_register_static(&xor_test_info);
}

type_init(xor_test_register_types)
