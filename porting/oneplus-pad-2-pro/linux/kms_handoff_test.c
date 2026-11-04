// SPDX-License-Identifier: GPL-2.0-only
/* Register/CTL model for test_kms_handoff.py; not hardware emulation. */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define __iomem
#define BIT(x) (1u << (x))
#define INTF_DSI 1
struct dpu_intf_cfg {
    int id, type;
    unsigned base;
};
struct dpu_mdss_cfg {
    int intf_count;
    struct dpu_intf_cfg *intf;
};
struct device {
    void *of_node;
};
struct platform_device {
    struct device dev;
};
struct dpu_hw_ctl;
struct dpu_hw_blk {
    int id;
};
struct ctl_ops {
    void (*clear_all_blendstages)(struct dpu_hw_ctl *);
    void (*set_active_fetch_pipes)(struct dpu_hw_ctl *, void *);
    void (*set_active_pipes)(struct dpu_hw_ctl *, void *);
    void (*set_active_lms)(struct dpu_hw_ctl *, void *);
    int (*reset)(struct dpu_hw_ctl *);
};
struct dpu_hw_ctl {
    struct dpu_hw_blk base;
    struct ctl_ops ops;
    int idx;
    unsigned cleared;
};
struct rm {
    struct dpu_hw_blk *ctl_blks[6];
};
struct dpu_kms {
    const struct dpu_mdss_cfg *catalog;
    struct platform_device *pdev;
    void *mmio;
    struct rm rm;
};
#define ARRAY_SIZE(x) ((int)(sizeof(x) / sizeof((x)[0])))
static struct dpu_hw_ctl ctls[6];
static int reset_failure = -1, reset_count;
static struct dpu_hw_ctl *to_dpu_hw_ctl(struct dpu_hw_blk *b) {
    return (struct dpu_hw_ctl *)b;
}
static void clear_blend(struct dpu_hw_ctl *c) { c->cleared |= 1; }
static void clear_fetch(struct dpu_hw_ctl *c, void *p) {
    assert(!p);
    c->cleared |= 2;
}
static void clear_pipes(struct dpu_hw_ctl *c, void *p) {
    assert(!p);
    c->cleared |= 4;
}
static void clear_lms(struct dpu_hw_ctl *c, void *p) {
    assert(!p);
    c->cleared |= 8;
}
static int reset_ctl(struct dpu_hw_ctl *c) {
    assert(c->cleared == 15);
    reset_count++;
    return c->idx == reset_failure ? -EIO : 0;
}
static u32 mmio[0x40000 / 4];
static bool compatible, stuck, board_compatible;
static unsigned disabled;
static bool of_device_is_compatible(void *n, const char *s) {
    (void)n;
    (void)s;
    return compatible;
}
static bool of_machine_is_compatible(const char *s) {
    (void)s;
    return board_compatible;
}
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v, void *p) {
    unsigned offset = (char *)p - (char *)mmio;
    assert(v == 0 && (offset == 0x35000 || offset == 0x36000));
    disabled |= offset == 0x35000 ? 1u : 2u;
    *(u32 *)p = v;
}
static int poll(void *p, u32 *status) {
    /* Both timing-enable requests must precede either status poll. */
    assert(disabled == 3);
    if (!stuck)
        *(u32 *)p = 0;
    *status = readl(p);
    return (*status & 1) ? -ETIMEDOUT : 0;
}
#define readl_poll_timeout(p, v, cond, delay, timeout) poll((p), &(v))
#define DRM_DEV_ERROR(...)                                                               \
    do {                                                                                 \
    } while (0)
#define pr_info(...) printf(__VA_ARGS__)

/*__KERNEL_HELPERS__*/

int main(void) {
    struct dpu_intf_cfg intf[] = {
        {0, 0, 0x34000}, {1, INTF_DSI, 0x35000}, {2, INTF_DSI, 0x36000}};
    struct dpu_mdss_cfg cat = {3, intf};
    struct platform_device pdev;
    struct dpu_kms kms = {.catalog = &cat, .pdev = &pdev, .mmio = mmio};
    for (int i = 0; i < 6; i++) {
        ctls[i] = (struct dpu_hw_ctl){
            .idx = i,
            .ops = {clear_blend, clear_fetch, clear_pipes, clear_lms, reset_ctl}};
        kms.rm.ctl_blks[i] = &ctls[i].base;
    }
    for (int scenario = 0; scenario < 6; scenario++) {
        memset(mmio, 0, sizeof(mmio));
        mmio[(0x34000 + 0x26c) / 4] = 1;
        mmio[(0x35000 + 0x26c) / 4] = scenario == 2 ? 0 : 1;
        mmio[(0x36000 + 0x26c) / 4] = 1;
        compatible = scenario != 0;
        board_compatible = scenario != 5;
        stuck = scenario == 3;
        disabled = 0;
        reset_count = 0;
        reset_failure = scenario == 4 ? 2 : -1;
        for (int i = 0; i < 6; i++)
            ctls[i].cleared = 0;
        int ret = dpu_kms_stop_firmware_scanout(&kms);
        assert(ret == (stuck ? -ETIMEDOUT : scenario == 4 ? -EIO : 0));
        assert(reset_count == (!compatible || !board_compatible || stuck ? 0
                               : scenario == 4                           ? 3
                                                                         : 6));
        assert(disabled == ((compatible && board_compatible) ? 3u : 0u));
        assert(mmio[(0x34000 + 0x26c) / 4] == 1);
    }
    puts("PASS: other SoCs/DP untouched, both INTFs stopped before CTLs, all CTL state "
         "cleared before reset, INTF/CTL failures propagated");
}
