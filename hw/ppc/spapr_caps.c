/*
 * QEMU PowerPC pSeries Logical Partition capabilities handling
 *
 * Copyright (c) 2017 David Gibson, Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/hw_accel.h"
#include "exec/ram_addr.h"
#include "target/ppc/cpu.h"
#include "target/ppc/mmu-hash64.h"
#include "cpu-models.h"
#include "kvm_ppc.h"

#include "hw/ppc/spapr.h"

typedef struct sPAPRCapPossible {
    int num;            /* size of vals array below */
    const char *help;   /* help text for vals */
    /*
     * Note:
     * - because of the way compatibility is determined vals MUST be ordered
     *   such that later options are a superset of all preceding options.
     * - the order of vals must be preserved, that is their index is important,
     *   however vals may be added to the end of the list so long as the above
     *   point is observed
     */
    const char *vals[];
} sPAPRCapPossible;

typedef struct sPAPRCapabilityInfo {
    const char *name;
    const char *description;
    int index;

    /* Getter and Setter Function Pointers */
    ObjectPropertyAccessor *get;
    ObjectPropertyAccessor *set;
    const char *type;
    /* Possible values if this is a custom string type */
    sPAPRCapPossible *possible;
    /* Make sure the virtual hardware can support this capability */
    void (*apply)(sPAPRMachineState *spapr, uint8_t val, Error **errp);
    void (*cpu_apply)(sPAPRMachineState *spapr, PowerPCCPU *cpu,
                      uint8_t val, Error **errp);
} sPAPRCapabilityInfo;

static void spapr_cap_get_bool(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    bool value = spapr_get_cap(spapr, cap->index) == SPAPR_CAP_ON;

    visit_type_bool(v, name, &value, errp);
}

static void spapr_cap_set_bool(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    bool value;
    Error *local_err = NULL;

    visit_type_bool(v, name, &value, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    spapr->cmd_line_caps[cap->index] = true;
    spapr->eff.caps[cap->index] = value ? SPAPR_CAP_ON : SPAPR_CAP_OFF;
}


static void  spapr_cap_get_string(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    char *val = NULL;
    uint8_t value = spapr_get_cap(spapr, cap->index);

    if (value >= cap->possible->num) {
        error_setg(errp, "Invalid value (%d) for cap-%s", value, cap->name);
        return;
    }

    val = g_strdup(cap->possible->vals[value]);

    visit_type_str(v, name, &val, errp);
    g_free(val);
}

static void spapr_cap_set_string(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    Error *local_err = NULL;
    uint8_t i;
    char *val;

    visit_type_str(v, name, &val, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (!strcmp(val, "?")) {
        error_setg(errp, "%s", cap->possible->help);
        goto out;
    }
    for (i = 0; i < cap->possible->num; i++) {
        if (!strcasecmp(val, cap->possible->vals[i])) {
            spapr->cmd_line_caps[cap->index] = true;
            spapr->eff.caps[cap->index] = i;
            goto out;
        }
    }

    error_setg(errp, "Invalid capability mode \"%s\" for cap-%s", val,
               cap->name);
out:
    g_free(val);
}

static void spapr_cap_get_pagesize(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    uint8_t val = spapr_get_cap(spapr, cap->index);
    uint64_t pagesize = (1ULL << val);

    visit_type_size(v, name, &pagesize, errp);
}

static void spapr_cap_set_pagesize(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    uint64_t pagesize;
    uint8_t val;
    Error *local_err = NULL;

    visit_type_size(v, name, &pagesize, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (!is_power_of_2(pagesize)) {
        error_setg(errp, "cap-%s must be a power of 2", cap->name);
        return;
    }

    val = ctz64(pagesize);
    spapr->cmd_line_caps[cap->index] = true;
    spapr->eff.caps[cap->index] = val;
}

static void cap_htm_apply(sPAPRMachineState *spapr, uint8_t val, Error **errp)
{
    if (!val) {
        /* TODO: We don't support disabling htm yet */
        return;
    }
    if (tcg_enabled()) {
        error_setg(errp,
                   "No Transactional Memory support in TCG, try cap-htm=off");
    } else if (kvm_enabled() && !kvmppc_has_cap_htm()) {
        error_setg(errp,
"KVM implementation does not support Transactional Memory, try cap-htm=off"
            );
    }
}

static void cap_vsx_apply(sPAPRMachineState *spapr, uint8_t val, Error **errp)
{
    PowerPCCPU *cpu = POWERPC_CPU(first_cpu);
    CPUPPCState *env = &cpu->env;

    if (!val) {
        /* TODO: We don't support disabling vsx yet */
        return;
    }
    /* Allowable CPUs in spapr_cpu_core.c should already have gotten
     * rid of anything that doesn't do VMX */
    g_assert(env->insns_flags & PPC_ALTIVEC);
    if (!(env->insns_flags2 & PPC2_VSX)) {
        error_setg(errp, "VSX support not available, try cap-vsx=off");
    }
}

static void cap_dfp_apply(sPAPRMachineState *spapr, uint8_t val, Error **errp)
{
    PowerPCCPU *cpu = POWERPC_CPU(first_cpu);
    CPUPPCState *env = &cpu->env;

    if (!val) {
        /* TODO: We don't support disabling dfp yet */
        return;
    }
    if (!(env->insns_flags2 & PPC2_DFP)) {
        error_setg(errp, "DFP support not available, try cap-dfp=off");
    }
}

sPAPRCapPossible cap_cfpc_possible = {
    .num = 3,
    .vals = {"broken", "workaround", "fixed"},
    .help = "broken - no protection, workaround - workaround available,"
            " fixed - fixed in hardware",
};

static void cap_safe_cache_apply(sPAPRMachineState *spapr, uint8_t val,
                                 Error **errp)
{
    uint8_t kvm_val =  kvmppc_get_cap_safe_cache();

    if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp,
"Requested safe cache capability level not supported by tcg, try a different value for cap-cfpc");
    } else if (kvm_enabled() && (val > kvm_val)) {
        error_setg(errp,
"Requested safe cache capability level not supported by kvm, try cap-cfpc=%s",
                   cap_cfpc_possible.vals[kvm_val]);
    }
}

sPAPRCapPossible cap_sbbc_possible = {
    .num = 3,
    .vals = {"broken", "workaround", "fixed"},
    .help = "broken - no protection, workaround - workaround available,"
            " fixed - fixed in hardware",
};

static void cap_safe_bounds_check_apply(sPAPRMachineState *spapr, uint8_t val,
                                        Error **errp)
{
    uint8_t kvm_val =  kvmppc_get_cap_safe_bounds_check();

    if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp,
"Requested safe bounds check capability level not supported by tcg, try a different value for cap-sbbc");
    } else if (kvm_enabled() && (val > kvm_val)) {
        error_setg(errp,
"Requested safe bounds check capability level not supported by kvm, try cap-sbbc=%s",
                   cap_sbbc_possible.vals[kvm_val]);
    }
}

sPAPRCapPossible cap_ibs_possible = {
    .num = 4,
    /* Note workaround only maintained for compatibility */
    .vals = {"broken", "workaround", "fixed-ibs", "fixed-ccd"},
    .help = "broken - no protection, fixed-ibs - indirect branch serialisation,"
            " fixed-ccd - cache count disabled",
};

static void cap_safe_indirect_branch_apply(sPAPRMachineState *spapr,
                                           uint8_t val, Error **errp)
{
    uint8_t kvm_val = kvmppc_get_cap_safe_indirect_branch();

    if (val == SPAPR_CAP_WORKAROUND) { /* Can only be Broken or Fixed */
        error_setg(errp,
"Requested safe indirect branch capability level \"workaround\" not valid, try cap-ibs=%s",
                   cap_ibs_possible.vals[kvm_val]);
    } else if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp,
"Requested safe indirect branch capability level not supported by tcg, try a different value for cap-ibs");
    } else if (kvm_enabled() && val && (val != kvm_val)) {
        error_setg(errp,
"Requested safe indirect branch capability level not supported by kvm, try cap-ibs=%s",
                   cap_ibs_possible.vals[kvm_val]);
    }
}

#define VALUE_DESC_TRISTATE     " (broken, workaround, fixed)"

void spapr_check_pagesize(sPAPRMachineState *spapr, hwaddr pagesize,
                          Error **errp)
{
    hwaddr maxpagesize = (1ULL << spapr->eff.caps[SPAPR_CAP_HPT_MAXPAGESIZE]);

    if (!kvmppc_hpt_needs_host_contiguous_pages()) {
        return;
    }

    if (maxpagesize > pagesize) {
        error_setg(errp,
                   "Can't support %"HWADDR_PRIu" kiB guest pages with %"
                   HWADDR_PRIu" kiB host pages with this KVM implementation",
                   maxpagesize >> 10, pagesize >> 10);
    }
}

static void cap_hpt_maxpagesize_apply(sPAPRMachineState *spapr,
                                      uint8_t val, Error **errp)
{
    if (val < 12) {
        error_setg(errp, "Require at least 4kiB hpt-max-page-size");
        return;
    } else if (val < 16) {
        warn_report("Many guests require at least 64kiB hpt-max-page-size");
    }

    spapr_check_pagesize(spapr, qemu_getrampagesize(), errp);
}

static bool spapr_pagesize_cb(void *opaque, uint32_t seg_pshift,
                              uint32_t pshift)
{
    unsigned maxshift = *((unsigned *)opaque);

    assert(pshift >= seg_pshift);

    /* Don't allow the guest to use pages bigger than the configured
     * maximum size */
    if (pshift > maxshift) {
        return false;
    }

    /* For whatever reason, KVM doesn't allow multiple pagesizes
     * within a segment, *except* for the case of 16M pages in a 4k or
     * 64k segment.  Always exclude other cases, so that TCG and KVM
     * guests see a consistent environment */
    if ((pshift != seg_pshift) && (pshift != 24)) {
        return false;
    }

    return true;
}

static void cap_hpt_maxpagesize_cpu_apply(sPAPRMachineState *spapr,
                                          PowerPCCPU *cpu,
                                          uint8_t val, Error **errp)
{
    unsigned maxshift = val;

    ppc_hash64_filter_pagesizes(cpu, spapr_pagesize_cb, &maxshift);
}

static void cap_nested_kvm_hv_apply(sPAPRMachineState *spapr,
                                    uint8_t val, Error **errp)
{
    if (!val) {
        /* capability disabled by default */
        return;
    }

    if (tcg_enabled()) {
        error_setg(errp,
                   "No Nested KVM-HV support in tcg, try cap-nested-hv=off");
    } else if (kvm_enabled()) {
        if (!kvmppc_has_cap_nested_kvm_hv()) {
            error_setg(errp,
"KVM implementation does not support Nested KVM-HV, try cap-nested-hv=off");
        } else if (kvmppc_set_cap_nested_kvm_hv(val) < 0) {
                error_setg(errp,
"Error enabling cap-nested-hv with KVM, try cap-nested-hv=off");
        }
    }
}

sPAPRCapabilityInfo capability_table[SPAPR_CAP_NUM] = {
    [SPAPR_CAP_HTM] = {
        .name = "htm",
        .description = "Allow Hardware Transactional Memory (HTM)",
        .index = SPAPR_CAP_HTM,
        .get = spapr_cap_get_bool,
        .set = spapr_cap_set_bool,
        .type = "bool",
        .apply = cap_htm_apply,
    },
    [SPAPR_CAP_VSX] = {
        .name = "vsx",
        .description = "Allow Vector Scalar Extensions (VSX)",
        .index = SPAPR_CAP_VSX,
        .get = spapr_cap_get_bool,
        .set = spapr_cap_set_bool,
        .type = "bool",
        .apply = cap_vsx_apply,
    },
    [SPAPR_CAP_DFP] = {
        .name = "dfp",
        .description = "Allow Decimal Floating Point (DFP)",
        .index = SPAPR_CAP_DFP,
        .get = spapr_cap_get_bool,
        .set = spapr_cap_set_bool,
        .type = "bool",
        .apply = cap_dfp_apply,
    },
    [SPAPR_CAP_CFPC] = {
        .name = "cfpc",
        .description = "Cache Flush on Privilege Change" VALUE_DESC_TRISTATE,
        .index = SPAPR_CAP_CFPC,
        .get = spapr_cap_get_string,
        .set = spapr_cap_set_string,
        .type = "string",
        .possible = &cap_cfpc_possible,
        .apply = cap_safe_cache_apply,
    },
    [SPAPR_CAP_SBBC] = {
        .name = "sbbc",
        .description = "Speculation Barrier Bounds Checking" VALUE_DESC_TRISTATE,
        .index = SPAPR_CAP_SBBC,
        .get = spapr_cap_get_string,
        .set = spapr_cap_set_string,
        .type = "string",
        .possible = &cap_sbbc_possible,
        .apply = cap_safe_bounds_check_apply,
    },
    [SPAPR_CAP_IBS] = {
        .name = "ibs",
        .description =
            "Indirect Branch Speculation (broken, fixed-ibs, fixed-ccd)",
        .index = SPAPR_CAP_IBS,
        .get = spapr_cap_get_string,
        .set = spapr_cap_set_string,
        .type = "string",
        .possible = &cap_ibs_possible,
        .apply = cap_safe_indirect_branch_apply,
    },
    [SPAPR_CAP_HPT_MAXPAGESIZE] = {
        .name = "hpt-max-page-size",
        .description = "Maximum page size for Hash Page Table guests",
        .index = SPAPR_CAP_HPT_MAXPAGESIZE,
        .get = spapr_cap_get_pagesize,
        .set = spapr_cap_set_pagesize,
        .type = "int",
        .apply = cap_hpt_maxpagesize_apply,
        .cpu_apply = cap_hpt_maxpagesize_cpu_apply,
    },
    [SPAPR_CAP_NESTED_KVM_HV] = {
        .name = "nested-hv",
        .description = "Allow Nested KVM-HV",
        .index = SPAPR_CAP_NESTED_KVM_HV,
        .get = spapr_cap_get_bool,
        .set = spapr_cap_set_bool,
        .type = "bool",
        .apply = cap_nested_kvm_hv_apply,
    },
};

static sPAPRCapabilities default_caps_with_cpu(sPAPRMachineState *spapr,
                                               const char *cputype)
{
    sPAPRMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);
    sPAPRCapabilities caps;

    caps = smc->default_caps;

    if (!ppc_type_check_compat(cputype, CPU_POWERPC_LOGICAL_2_07,
                               0, spapr->max_compat_pvr)) {
        caps.caps[SPAPR_CAP_HTM] = SPAPR_CAP_OFF;
        caps.caps[SPAPR_CAP_CFPC] = SPAPR_CAP_BROKEN;
    }

    if (!ppc_type_check_compat(cputype, CPU_POWERPC_LOGICAL_2_06_PLUS,
                               0, spapr->max_compat_pvr)) {
        caps.caps[SPAPR_CAP_SBBC] = SPAPR_CAP_BROKEN;
    }

    if (!ppc_type_check_compat(cputype, CPU_POWERPC_LOGICAL_2_06,
                               0, spapr->max_compat_pvr)) {
        caps.caps[SPAPR_CAP_VSX] = SPAPR_CAP_OFF;
        caps.caps[SPAPR_CAP_DFP] = SPAPR_CAP_OFF;
        caps.caps[SPAPR_CAP_IBS] = SPAPR_CAP_BROKEN;
    }

    /* This is for pseries-2.12 and older */
    if (smc->default_caps.caps[SPAPR_CAP_HPT_MAXPAGESIZE] == 0) {
        uint8_t mps;

        if (kvmppc_hpt_needs_host_contiguous_pages()) {
            mps = ctz64(qemu_getrampagesize());
        } else {
            mps = 34; /* allow everything up to 16GiB, i.e. everything */
        }

        caps.caps[SPAPR_CAP_HPT_MAXPAGESIZE] = mps;
    }

    return caps;
}

int spapr_caps_pre_load(void *opaque)
{
    sPAPRMachineState *spapr = opaque;

    /* Set to default so we can tell if this came in with the migration */
    spapr->mig = spapr->def;
    return 0;
}

int spapr_caps_pre_save(void *opaque)
{
    sPAPRMachineState *spapr = opaque;

    spapr->mig = spapr->eff;
    return 0;
}

/* This has to be called from the top-level spapr post_load, not the
 * caps specific one.  Otherwise it wouldn't be called when the source
 * caps are all defaults, which could still conflict with overridden
 * caps on the destination */
int spapr_caps_post_migration(sPAPRMachineState *spapr)
{
    int i;
    bool ok = true;
    sPAPRCapabilities dstcaps = spapr->eff;
    sPAPRCapabilities srccaps;

    srccaps = default_caps_with_cpu(spapr, MACHINE(spapr)->cpu_type);
    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        /* If not default value then assume came in with the migration */
        if (spapr->mig.caps[i] != spapr->def.caps[i]) {
            srccaps.caps[i] = spapr->mig.caps[i];
        }
    }

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        sPAPRCapabilityInfo *info = &capability_table[i];

        if (srccaps.caps[i] > dstcaps.caps[i]) {
            error_report("cap-%s higher level (%d) in incoming stream than on destination (%d)",
                         info->name, srccaps.caps[i], dstcaps.caps[i]);
            ok = false;
        }

        if (srccaps.caps[i] < dstcaps.caps[i]) {
            warn_report("cap-%s lower level (%d) in incoming stream than on destination (%d)",
                         info->name, srccaps.caps[i], dstcaps.caps[i]);
        }
    }

    return ok ? 0 : -EINVAL;
}

/* Used to generate the migration field and needed function for a spapr cap */
#define SPAPR_CAP_MIG_STATE(sname, cap)                 \
static bool spapr_cap_##sname##_needed(void *opaque)    \
{                                                       \
    sPAPRMachineState *spapr = opaque;                  \
                                                        \
    return spapr->cmd_line_caps[cap] &&                 \
           (spapr->eff.caps[cap] !=                     \
            spapr->def.caps[cap]);                      \
}                                                       \
                                                        \
const VMStateDescription vmstate_spapr_cap_##sname = {  \
    .name = "spapr/cap/" #sname,                        \
    .version_id = 1,                                    \
    .minimum_version_id = 1,                            \
    .needed = spapr_cap_##sname##_needed,               \
    .fields = (VMStateField[]) {                        \
        VMSTATE_UINT8(mig.caps[cap],                    \
                      sPAPRMachineState),               \
        VMSTATE_END_OF_LIST()                           \
    },                                                  \
}

SPAPR_CAP_MIG_STATE(htm, SPAPR_CAP_HTM);
SPAPR_CAP_MIG_STATE(vsx, SPAPR_CAP_VSX);
SPAPR_CAP_MIG_STATE(dfp, SPAPR_CAP_DFP);
SPAPR_CAP_MIG_STATE(cfpc, SPAPR_CAP_CFPC);
SPAPR_CAP_MIG_STATE(sbbc, SPAPR_CAP_SBBC);
SPAPR_CAP_MIG_STATE(ibs, SPAPR_CAP_IBS);
SPAPR_CAP_MIG_STATE(nested_kvm_hv, SPAPR_CAP_NESTED_KVM_HV);

void spapr_caps_init(sPAPRMachineState *spapr)
{
    sPAPRCapabilities default_caps;
    int i;

    /* Compute the actual set of caps we should run with */
    default_caps = default_caps_with_cpu(spapr, MACHINE(spapr)->cpu_type);

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        /* Store the defaults */
        spapr->def.caps[i] = default_caps.caps[i];
        /* If not set on the command line then apply the default value */
        if (!spapr->cmd_line_caps[i]) {
            spapr->eff.caps[i] = default_caps.caps[i];
        }
    }
}

void spapr_caps_apply(sPAPRMachineState *spapr)
{
    int i;

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        sPAPRCapabilityInfo *info = &capability_table[i];

        /*
         * If the apply function can't set the desired level and thinks it's
         * fatal, it should cause that.
         */
        info->apply(spapr, spapr->eff.caps[i], &error_fatal);
    }
}

void spapr_caps_cpu_apply(sPAPRMachineState *spapr, PowerPCCPU *cpu)
{
    int i;

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        sPAPRCapabilityInfo *info = &capability_table[i];

        /*
         * If the apply function can't set the desired level and thinks it's
         * fatal, it should cause that.
         */
        if (info->cpu_apply) {
            info->cpu_apply(spapr, cpu, spapr->eff.caps[i], &error_fatal);
        }
    }
}

void spapr_caps_add_properties(sPAPRMachineClass *smc, Error **errp)
{
    Error *local_err = NULL;
    ObjectClass *klass = OBJECT_CLASS(smc);
    int i;

    for (i = 0; i < ARRAY_SIZE(capability_table); i++) {
        sPAPRCapabilityInfo *cap = &capability_table[i];
        const char *name = g_strdup_printf("cap-%s", cap->name);
        char *desc;

        object_class_property_add(klass, name, cap->type,
                                  cap->get, cap->set,
                                  NULL, cap, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }

        desc = g_strdup_printf("%s", cap->description);
        object_class_property_set_description(klass, name, desc, &local_err);
        g_free(desc);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
}
