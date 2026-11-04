// SPDX-License-Identifier: GPL-2.0-only
/*
 * Register model for test_mdss_boot_handoff.py; not hardware emulation.
 * It checks the order, scope and error propagation of the OPD2413 boot
 * scanout retirement that runs before the MDSS core reset: INTF stop, then
 * CTL reset while the CTLs still own the pipes, then the VBIF AXI halt.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef int64_t s64;
typedef int64_t ktime_t;
#define __iomem
#define BIT(x) (1u << (x))
#define GENMASK(h, l) ((~0u << (l)) & (~0u >> (31 - (h))))
#define ARRAY_SIZE(x) ((int)(sizeof(x) / sizeof((x)[0])))

static uint8_t mdp[0x40000], vbif[0x2008];
static ktime_t now;
static ktime_t ktime_get(void) { return now; }
static s64 ktime_us_delta(ktime_t a, ktime_t b) { return a - b; }
static void usleep_range(unsigned a, unsigned b) { now += a + (b - a) / 2; }
static void wmb(void) {}

static const unsigned ctl_base[6] = {0x15000, 0x16000, 0x17000, 0x18000, 0x19000, 0x1a000};

/* model state */
enum { RUNNING, STOPPING, STOPPED };
static int intf_state[2], intf_polls[2], stop_after_polls; /* <0: never stops */
static unsigned initially_running;
static unsigned ctl_flush_target;
static int ctl_resetting[6], ctl_polls[6];
static int ctl_soft_after; /* polls until a soft reset clears; <0: needs the override */
static bool override_works, override_held[6];
static unsigned ctl_reset_mask, ctl_override_mask;
static int ack_after_polls, halt_polls; /* <0: never acknowledged */
static u32 intf_config;
static int flush_after; /* polls until a CTL flush is consumed; <0: never */
static int ctl_flush_polls[6];
static bool ctl_flush_pending[6], ctl_intf_flush_written[6], config_cleared[2];
static unsigned flush_written_mask;
static bool flushes_settled; /* every CTL flush consumed or its wait ran out */
static u32 idle_bits;
static bool halt_requested, clk_forced, reset_done, released;
static bool intf_waits_done, ctl_resets_done;
static int write_count;

static u32 *reg(void *p) { return (u32 *)p; }

static int intf_index(unsigned offset) {
    if (offset >= 0x35000 && offset < 0x36000)
        return 0;
    if (offset >= 0x36000 && offset < 0x37000)
        return 1;
    return -1;
}

static int ctl_index(unsigned offset) {
    for (int i = 0; i < 6; i++)
        if (offset >= ctl_base[i] && offset < ctl_base[i] + 0x1000)
            return i;
    return -1;
}

/* every inherited timing engine has stopped or its wait ran out */
static bool intf_waits_finished(void) {
    for (int i = 0; i < 2; i++)
        if ((initially_running & BIT(i)) && intf_state[i] != STOPPED && intf_polls[i] < 50)
            return false;
    return true;
}

static bool all_ctl_resets_settled(void) {
    for (int i = 0; i < 6; i++)
        if (ctl_resetting[i] && (ctl_soft_after >= 0 || override_works))
            return false;
    return true;
}

static u32 readl(void *p) {
    uint8_t *b = p;
    /* the MDP blocks answer with an external abort right after the reset */
    assert(!reset_done || !(b >= mdp && b < mdp + sizeof(mdp)));
    if (b >= mdp && b < mdp + sizeof(mdp)) {
        unsigned offset = b - mdp;
        int i = intf_index(offset), c = ctl_index(offset);
        if (i >= 0 && (offset & 0xfff) == 0x26c)
            return intf_state[i] != STOPPED;
        if (c >= 0 && (offset & 0xfff) == 0x030)
            return ctl_resetting[c] ? 1 : 0;
        if (c >= 0 && (offset & 0xfff) == 0x018)
            return ctl_flush_pending[c] ? BIT(31) : 0;
    }
    if (b >= vbif && b < vbif + sizeof(vbif) && b - vbif == 0x20c) {
        u32 v = idle_bits;
        if (halt_requested && ack_after_polls >= 0 && halt_polls >= ack_after_polls)
            v |= 1;
        return v;
    }
    return *reg(p);
}

static void writel(u32 v, void *p) {
    uint8_t *b = p;
    write_count++;
    assert(!reset_done || (b >= vbif && b < vbif + sizeof(vbif)));
    if (b >= mdp && b < mdp + sizeof(mdp)) {
        unsigned offset = b - mdp;
        int i = intf_index(offset), c = ctl_index(offset);
        if (i >= 0 && (offset & 0xfff) == 0x004) {
            /* only programmable fetch is cleared, before anything else */
            assert(v == (intf_config & ~BIT(31)) && (intf_config & BIT(31)));
            assert(flush_written_mask == 0 && !intf_waits_done);
            config_cleared[i] = true;
        } else if (i >= 0) {
            /* only the timing enable of the two DSI INTFs, only to stop */
            assert((offset & 0xfff) == 0x000 && v == 0);
            assert(!halt_requested && ctl_reset_mask == 0);
            /* the flush carrying prog fetch off was consumed first */
            if (flush_written_mask) {
                for (int k = 0; k < 6; k++)
                    assert(!(flush_written_mask & BIT(k)) || !ctl_flush_pending[k] ||
                           ctl_flush_polls[k] >= 50);
                flushes_settled = true;
            }
            if (intf_state[i] == RUNNING)
                intf_state[i] = STOPPING;
        } else if (c >= 0 && (offset & 0xfff) == 0x110) {
            /* the INTF bits of the running DSI INTFs, after the config */
            unsigned want = ((initially_running & 1) && config_cleared[0] ? BIT(1) : 0) |
                            ((initially_running & 2) && config_cleared[1] ? BIT(2) : 0);
            assert(v == want && want && !ctl_flush_pending[c]);
            ctl_intf_flush_written[c] = true;
        } else if (c >= 0 && (offset & 0xfff) == 0x018) {
            assert(v == BIT(31) && ctl_intf_flush_written[c]);
            flush_written_mask |= BIT(c);
            ctl_flush_pending[c] = true;
            ctl_flush_polls[c] = 0;
        } else if (c >= 0 && (offset & 0xfff) == 0x030) {
            /* CTL resets only after every INTF wait ended, before the halt */
            assert(v == 1 && intf_waits_finished() && !halt_requested);
            intf_waits_done = true;
            ctl_reset_mask |= BIT(c);
            ctl_resetting[c] = 1;
            ctl_polls[c] = 0;
        } else if (c >= 0 && (offset & 0xfff) == 0x060) {
            /* the override follows a soft reset of the same CTL */
            assert(ctl_reset_mask & BIT(c));
            if (v == 1) {
                override_held[c] = true;
            } else {
                assert(override_held[c]);
                override_held[c] = false;
                ctl_override_mask |= BIT(c);
                if (override_works)
                    ctl_resetting[c] = 0;
            }
        } else {
            assert(0 && "unexpected DPU write");
        }
    } else {
        unsigned offset = b - vbif;
        assert(b >= vbif && b < vbif + sizeof(vbif));
        if (offset == 0x004) {
            if (v & 1) {
                assert(!reset_done);
                clk_forced = true;
            } else {
                assert(reset_done && !halt_requested);
                clk_forced = false;
                released = true;
            }
        } else if (offset == 0x208) {
            if (v & 1) {
                /* clock forced, every INTF wait and CTL reset finished */
                assert(clk_forced && !reset_done && intf_waits_done);
                assert(ctl_reset_mask != 0 && all_ctl_resets_settled());
                ctl_resets_done = true;
                halt_requested = true;
            } else {
                assert(reset_done);
                halt_requested = false;
            }
        } else {
            assert(0 && "unexpected VBIF write");
        }
    }
    *reg(p) = v;
}

static void poll_once(void *p) {
    uint8_t *b = p;
    now += 10;
    if (b >= mdp && b < mdp + sizeof(mdp)) {
        unsigned offset = b - mdp;
        int i = intf_index(offset), c = ctl_index(offset);
        if (i >= 0) {
            /* every stop request precedes the first status poll */
            int requested = (intf_state[0] != RUNNING && (initially_running & 1)) +
                            (intf_state[1] != RUNNING && (initially_running & 2));
            assert(requested == __builtin_popcount(initially_running));
            intf_polls[i]++;
            if (intf_state[i] == STOPPING && stop_after_polls >= 0 &&
                intf_polls[i] > stop_after_polls)
                intf_state[i] = STOPPED;
        } else if (c >= 0 && (offset & 0xfff) == 0x018) {
            /* every flush is written before the first one is waited on */
            assert(flush_written_mask == ctl_flush_target);
            ctl_flush_polls[c]++;
            if (flush_after >= 0 && ctl_flush_polls[c] > flush_after)
                ctl_flush_pending[c] = false;
        } else if (c >= 0) {
            assert(intf_waits_done);
            ctl_polls[c]++;
            if (ctl_resetting[c] && ctl_soft_after >= 0 && ctl_polls[c] > ctl_soft_after)
                ctl_resetting[c] = 0;
        } else {
            assert(0 && "unexpected DPU poll");
        }
    } else {
        halt_polls++;
    }
}

/* Bounded polling: 50 polls stand in for the timeout. */
#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)                        \
    ({                                                                                   \
        int __ret = -ETIMEDOUT;                                                          \
        (void)(sleep_us);                                                                \
        (void)(timeout_us);                                                              \
        for (int __n = 0; __n < 50; __n++) {                                             \
            poll_once(addr);                                                             \
            (val) = readl(addr);                                                         \
            if (cond) {                                                                  \
                __ret = 0;                                                               \
                break;                                                                   \
            }                                                                            \
        }                                                                                \
        __ret;                                                                           \
    })

/*__KERNEL_HELPERS__*/

struct scenario {
    const char *name;
    int running[2];
    int stop_after;
    unsigned ctl_active; /* CTLs reporting INTF/fetch activity */
    int soft_after;
    bool override_ok;
    int ack_after;
    bool sspp_survives;
    int expect_ret;
    enum mdss_boot_ctl_result expect_ctl;
    u32 cfg;
    int flush_after;
};

static void setup(const struct scenario *s) {
    memset(mdp, 0, sizeof(mdp));
    memset(vbif, 0, sizeof(vbif));
    initially_running = s->running[0] | s->running[1] << 1;
    for (int i = 0; i < 2; i++) {
        intf_state[i] = s->running[i] ? RUNNING : STOPPED;
        intf_polls[i] = 0;
    }
    for (int i = 0; i < 6; i++) {
        ctl_resetting[i] = ctl_polls[i] = 0;
        override_held[i] = false;
        if (s->ctl_active & BIT(i))
            *reg(mdp + ctl_base[i] + 0x0f4) = 0x6; /* INTF_1|INTF_2 */
    }
    /* the bootloader's four rects on DMA0..DMA3 */
    for (unsigned o = 0x24000; o <= 0x2a000; o += 0x2000) {
        *reg(mdp + o) = 0x09600350;
        *reg(mdp + o + 0x14) = 0xfc800000 + (o - 0x24000) / 0x2000 * 0xd40;
    }
    intf_config = s->cfg;
    if (s->running[0])
        *reg(mdp + 0x35004) = s->cfg;
    if (s->running[1])
        *reg(mdp + 0x36004) = s->cfg;
    flush_after = s->flush_after;
    for (int i = 0; i < 6; i++) {
        ctl_flush_polls[i] = 0;
        ctl_flush_pending[i] = ctl_intf_flush_written[i] = false;
    }
    config_cleared[0] = config_cleared[1] = false;
    flush_written_mask = 0;
    flushes_settled = false;
    ctl_flush_target = (s->cfg & BIT(31)) ? s->ctl_active : 0;
    *reg(mdp + 0x35170) = 0x0a4e0001;
    *reg(mdp + 0x350b0) = 2458;
    *reg(mdp + 0x360b0) = 2460;
    *reg(vbif + 0x004) = 0x2;
    stop_after_polls = s->stop_after;
    ctl_soft_after = s->soft_after;
    override_works = s->override_ok;
    ack_after_polls = s->ack_after;
    idle_bits = 0x30;
    halt_polls = 0;
    ctl_reset_mask = ctl_override_mask = 0;
    halt_requested = clk_forced = reset_done = released = false;
    intf_waits_done = ctl_resets_done = false;
    write_count = 0;
    now = 0;
}

static void core_reset(bool sspp_survives) {
    reset_done = true;
    if (!sspp_survives)
        for (unsigned o = 0x24000; o <= 0x2a000; o += 0x2000)
            *reg(mdp + o) = *reg(mdp + o + 0x14) = 0;
}

int main(void) {
    const struct scenario scenarios[] = {
        /* name, running, stop_after, ctl_active, soft_after, override_ok, ack_after,
         * sspp_survives, expect_ret, expect_ctl, cfg, flush_after */
        {"prog fetch off, one owning CTL", {1, 1}, 3, BIT(0), 2, true, 2, false, 0,
         CTL_RESET_SOFT, 0x80800000, 3},
        {"no inherited scanout", {0, 0}, 3, BIT(0), 2, true, 2, false, 0, CTL_RESET_NONE,
         0x80800000, 3},
        {"one INTF running", {0, 1}, 3, BIT(0), 2, true, 2, false, 0, CTL_RESET_SOFT,
         0x80800000, 3},
        {"prog fetch already off: no flush", {1, 1}, 3, BIT(0), 2, true, 2, false, 0,
         CTL_RESET_SOFT, 0x00800000, 3},
        {"no CTL claims it: no flush, all reset", {1, 1}, 3, 0, 2, true, 2, false, 0,
         CTL_RESET_SOFT, 0x80800000, 3},
        {"flush never consumed", {1, 1}, 3, BIT(0), 2, true, 2, false, -ETIMEDOUT,
         CTL_RESET_SOFT, 0x80800000, -1},
        {"soft reset pending: override", {1, 1}, 3, BIT(0), -1, true, 2, false, 0,
         CTL_RESET_FORCED, 0x80800000, 3},
        {"override fails too", {1, 1}, 3, BIT(0), -1, false, 2, false, -ETIMEDOUT,
         CTL_RESET_FAILED, 0x80800000, 3},
        {"engine never stops", {1, 1}, -1, BIT(0), 2, true, 2, false, -ETIMEDOUT,
         CTL_RESET_SOFT, 0x80800000, 3},
        {"no halt ack, ports idle", {1, 1}, 3, BIT(0), 2, true, -1, false, -ETIMEDOUT,
         CTL_RESET_SOFT, 0x80800000, 3},
    };

    for (int n = 0; n < ARRAY_SIZE(scenarios); n++) {
        const struct scenario *s = &scenarios[n];
        struct mdss_boot_quiesce q = {.mdp = mdp, .vbif = vbif};
        bool inherited = s->running[0] || s->running[1];

        setup(s);
        int ret = mdss_boot_quiesce_hw(&q);
        assert(ret == s->expect_ret);
        assert(q.active == initially_running);
        assert(q.ctl_result == s->expect_ctl);
        if (!inherited) {
            /* nothing inherited: no register is written at all */
            assert(write_count == 0 && !q.halted);
        } else {
            unsigned owners = s->ctl_active ? s->ctl_active : 0x3f;
            assert(q.halted && halt_requested && clk_forced && ctl_resets_done);
            assert(q.ctls == owners && ctl_reset_mask == owners);
            assert(ctl_override_mask == (s->soft_after < 0 ? owners : 0));
            assert(q.stopped == (s->stop_after < 0 ? 0 : q.active));
            assert(q.halt_acked == (s->ack_after >= 0));
            assert(q.sspp_live == 0xf0 && q.sspp_addr == 0xfc800000);
            assert(q.prog_fetch_start == 0x0a4e0001);
            unsigned pf = (s->cfg & BIT(31)) && s->ctl_active ? initially_running : 0;
            assert(q.prog_fetch_off == pf);
            assert(flush_written_mask == (pf ? s->ctl_active : 0u));
            assert(q.prog_fetch_ret == (pf && s->flush_after < 0 ? -ETIMEDOUT : 0));
            assert(!pf || flushes_settled);
            if (s->running[0])
                assert(q.config[0] == s->cfg && q.line[0] == 2458);
        }

        core_reset(s->sspp_survives);
        mdss_boot_release_hw(&q);
        if (inherited) {
            assert(released && !halt_requested && !clk_forced);
            assert(q.reset_ctrl0 == 1);
            /* the VBIF clock control bits other than the force are kept */
            assert(*reg(vbif + 0x004) == 0x2);
        } else {
            assert(!released && write_count == 0);
        }
        printf("ok: %s\n", s->name);
    }
    puts("PASS: untouched without an inherited scanout; programmable fetch cleared and "
         "flushed through the owning CTLs, and the flush consumed, before the stop; "
         "both DSI stops requested before "
         "any wait; CTLs that own the scanout reset only after the stop waits (override "
         "only when the soft reset stays pending); AXI halt only after the CTL resets, "
         "with the clock forced and the acknowledgment required; timeouts propagated; "
         "only the VBIF touched after the core reset; halt released only after the reset");
    return 0;
}
