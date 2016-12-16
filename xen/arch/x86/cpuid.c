#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <asm/cpuid.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/vmx/vmcs.h>
#include <asm/processor.h>
#include <asm/xstate.h>

const uint32_t known_features[] = INIT_KNOWN_FEATURES;
const uint32_t special_features[] = INIT_SPECIAL_FEATURES;

static const uint32_t pv_featuremask[] = INIT_PV_FEATURES;
static const uint32_t hvm_shadow_featuremask[] = INIT_HVM_SHADOW_FEATURES;
static const uint32_t hvm_hap_featuremask[] = INIT_HVM_HAP_FEATURES;
static const uint32_t deep_features[] = INIT_DEEP_FEATURES;

#define EMPTY_LEAF ((struct cpuid_leaf){})

struct cpuid_policy __read_mostly raw_policy,
    __read_mostly host_policy,
    __read_mostly pv_max_policy,
    __read_mostly hvm_max_policy;

static void cpuid_leaf(uint32_t leaf, struct cpuid_leaf *data)
{
    cpuid(leaf, &data->a, &data->b, &data->c, &data->d);
}

static void cpuid_count_leaf(uint32_t leaf, uint32_t subleaf,
                             struct cpuid_leaf *data)
{
    cpuid_count(leaf, subleaf, &data->a, &data->b, &data->c, &data->d);
}

static void sanitise_featureset(uint32_t *fs)
{
    /* for_each_set_bit() uses unsigned longs.  Extend with zeroes. */
    uint32_t disabled_features[
        ROUNDUP(FSCAPINTS, sizeof(unsigned long)/sizeof(uint32_t))] = {};
    unsigned int i;

    for ( i = 0; i < FSCAPINTS; ++i )
    {
        /* Clamp to known mask. */
        fs[i] &= known_features[i];

        /*
         * Identify which features with deep dependencies have been
         * disabled.
         */
        disabled_features[i] = ~fs[i] & deep_features[i];
    }

    for_each_set_bit(i, (void *)disabled_features,
                     sizeof(disabled_features) * 8)
    {
        const uint32_t *dfs = lookup_deep_deps(i);
        unsigned int j;

        ASSERT(dfs); /* deep_features[] should guarentee this. */

        for ( j = 0; j < FSCAPINTS; ++j )
        {
            fs[j] &= ~dfs[j];
            disabled_features[j] &= ~dfs[j];
        }
    }

    /*
     * Sort out shared bits.  We are constructing a featureset which needs to
     * be applicable to a cross-vendor case.  Intel strictly clears the common
     * bits in e1d, while AMD strictly duplicates them.
     *
     * We duplicate them here to be compatible with AMD while on Intel, and
     * rely on logic closer to the guest to make the featureset stricter if
     * emulating Intel.
     */
    fs[FEATURESET_e1d] = ((fs[FEATURESET_1d]  &  CPUID_COMMON_1D_FEATURES) |
                          (fs[FEATURESET_e1d] & ~CPUID_COMMON_1D_FEATURES));
}

static void recalculate_xstate(struct cpuid_policy *p)
{
    uint64_t xstates = XSTATE_FP_SSE;
    uint32_t xstate_size = XSTATE_AREA_MIN_SIZE;
    unsigned int i, Da1 = p->xstate.Da1;

    /*
     * The Da1 leaf is the only piece of information preserved in the common
     * case.  Everything else is derived from other feature state.
     */
    memset(&p->xstate, 0, sizeof(p->xstate));

    if ( !p->basic.xsave )
        return;

    if ( p->basic.avx )
    {
        xstates |= XSTATE_YMM;
        xstate_size = max(xstate_size,
                          xstate_offsets[_XSTATE_YMM] +
                          xstate_sizes[_XSTATE_YMM]);
    }

    if ( p->feat.mpx )
    {
        xstates |= XSTATE_BNDREGS | XSTATE_BNDCSR;
        xstate_size = max(xstate_size,
                          xstate_offsets[_XSTATE_BNDCSR] +
                          xstate_sizes[_XSTATE_BNDCSR]);
    }

    if ( p->feat.avx512f )
    {
        xstates |= XSTATE_OPMASK | XSTATE_ZMM | XSTATE_HI_ZMM;
        xstate_size = max(xstate_size,
                          xstate_offsets[_XSTATE_HI_ZMM] +
                          xstate_sizes[_XSTATE_HI_ZMM]);
    }

    if ( p->feat.pku )
    {
        xstates |= XSTATE_PKRU;
        xstate_size = max(xstate_size,
                          xstate_offsets[_XSTATE_PKRU] +
                          xstate_sizes[_XSTATE_PKRU]);
    }

    if ( p->extd.lwp )
    {
        xstates |= XSTATE_LWP;
        xstate_size = max(xstate_size,
                          xstate_offsets[_XSTATE_LWP] +
                          xstate_sizes[_XSTATE_LWP]);
    }

    p->xstate.max_size  =  xstate_size;
    p->xstate.xcr0_low  =  xstates & ~XSTATE_XSAVES_ONLY;
    p->xstate.xcr0_high = (xstates & ~XSTATE_XSAVES_ONLY) >> 32;

    p->xstate.Da1 = Da1;
    if ( p->xstate.xsaves )
    {
        p->xstate.xss_low   =  xstates & XSTATE_XSAVES_ONLY;
        p->xstate.xss_high  = (xstates & XSTATE_XSAVES_ONLY) >> 32;
    }
    else
        xstates &= ~XSTATE_XSAVES_ONLY;

    for ( i = 2; i < min(63ul, ARRAY_SIZE(p->xstate.comp)); ++i )
    {
        uint64_t curr_xstate = 1ul << i;

        if ( !(xstates & curr_xstate) )
            continue;

        p->xstate.comp[i].size   = xstate_sizes[i];
        p->xstate.comp[i].offset = xstate_offsets[i];
        p->xstate.comp[i].xss    = curr_xstate & XSTATE_XSAVES_ONLY;
        p->xstate.comp[i].align  = curr_xstate & xstate_align;
    }
}

static void __init calculate_raw_policy(void)
{
    struct cpuid_policy *p = &raw_policy;
    unsigned int i;

    cpuid_leaf(0, &p->basic.raw[0]);
    for ( i = 1; i < min(ARRAY_SIZE(p->basic.raw),
                         p->basic.max_leaf + 1ul); ++i )
    {
        switch ( i )
        {
        case 0x2: case 0x4: case 0x7: case 0xd:
            /* Multi-invocation leaves.  Deferred. */
            continue;
        }

        cpuid_leaf(i, &p->basic.raw[i]);
    }

    if ( p->basic.max_leaf >= 7 )
    {
        cpuid_count_leaf(7, 0, &p->feat.raw[0]);

        for ( i = 1; i < min(ARRAY_SIZE(p->feat.raw),
                             p->feat.max_subleaf + 1ul); ++i )
            cpuid_count_leaf(7, i, &p->feat.raw[i]);
    }

    if ( p->basic.max_leaf >= XSTATE_CPUID )
    {
        uint64_t xstates;

        cpuid_count_leaf(XSTATE_CPUID, 0, &p->xstate.raw[0]);
        cpuid_count_leaf(XSTATE_CPUID, 1, &p->xstate.raw[1]);

        xstates = ((uint64_t)(p->xstate.xcr0_high | p->xstate.xss_high) << 32) |
            (p->xstate.xcr0_low | p->xstate.xss_low);

        for ( i = 2; i < min(63ul, ARRAY_SIZE(p->xstate.raw)); ++i )
        {
            if ( xstates & (1ul << i) )
                cpuid_count_leaf(XSTATE_CPUID, i, &p->xstate.raw[i]);
        }
    }

    /* Extended leaves. */
    cpuid_leaf(0x80000000, &p->extd.raw[0]);
    for ( i = 1; i < min(ARRAY_SIZE(p->extd.raw),
                         p->extd.max_leaf + 1 - 0x80000000ul); ++i )
        cpuid_leaf(0x80000000 + i, &p->extd.raw[i]);

    p->x86_vendor = boot_cpu_data.x86_vendor;
}

static void __init calculate_host_policy(void)
{
    struct cpuid_policy *p = &host_policy;

    *p = raw_policy;

    p->basic.max_leaf =
        min_t(uint32_t, p->basic.max_leaf,   ARRAY_SIZE(p->basic.raw) - 1);
    p->feat.max_subleaf =
        min_t(uint32_t, p->feat.max_subleaf, ARRAY_SIZE(p->feat.raw) - 1);
    p->extd.max_leaf =
        min_t(uint32_t, p->extd.max_leaf,
              0x80000000u + ARRAY_SIZE(p->extd.raw) - 1);

    cpuid_featureset_to_policy(boot_cpu_data.x86_capability, p);
    recalculate_xstate(p);
}

static void __init calculate_pv_max_policy(void)
{
    struct cpuid_policy *p = &pv_max_policy;
    uint32_t pv_featureset[FSCAPINTS];
    unsigned int i;

    *p = host_policy;
    cpuid_policy_to_featureset(p, pv_featureset);

    for ( i = 0; i < ARRAY_SIZE(pv_featureset); ++i )
        pv_featureset[i] &= pv_featuremask[i];

    /* Unconditionally claim to be able to set the hypervisor bit. */
    __set_bit(X86_FEATURE_HYPERVISOR, pv_featureset);

    sanitise_featureset(pv_featureset);
    cpuid_featureset_to_policy(pv_featureset, p);
    recalculate_xstate(p);
}

static void __init calculate_hvm_max_policy(void)
{
    struct cpuid_policy *p = &hvm_max_policy;
    uint32_t hvm_featureset[FSCAPINTS];
    unsigned int i;
    const uint32_t *hvm_featuremask;

    if ( !hvm_enabled )
        return;

    *p = host_policy;
    cpuid_policy_to_featureset(p, hvm_featureset);

    hvm_featuremask = hvm_funcs.hap_supported ?
        hvm_hap_featuremask : hvm_shadow_featuremask;

    for ( i = 0; i < ARRAY_SIZE(hvm_featureset); ++i )
        hvm_featureset[i] &= hvm_featuremask[i];

    /* Unconditionally claim to be able to set the hypervisor bit. */
    __set_bit(X86_FEATURE_HYPERVISOR, hvm_featureset);

    /*
     * Xen can provide an APIC emulation to HVM guests even if the host's APIC
     * isn't enabled.
     */
    __set_bit(X86_FEATURE_APIC, hvm_featureset);

    /*
     * On AMD, PV guests are entirely unable to use SYSENTER as Xen runs in
     * long mode (and init_amd() has cleared it out of host capabilities), but
     * HVM guests are able if running in protected mode.
     */
    if ( (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) && raw_policy.basic.sep )
        __set_bit(X86_FEATURE_SEP, hvm_featureset);

    /*
     * With VT-x, some features are only supported by Xen if dedicated
     * hardware support is also available.
     */
    if ( cpu_has_vmx )
    {
        if ( !cpu_has_vmx_mpx )
            __clear_bit(X86_FEATURE_MPX, hvm_featureset);

        if ( !cpu_has_vmx_xsaves )
            __clear_bit(X86_FEATURE_XSAVES, hvm_featureset);
    }

    sanitise_featureset(hvm_featureset);
    cpuid_featureset_to_policy(hvm_featureset, p);
    recalculate_xstate(p);
}

void __init init_guest_cpuid(void)
{
    calculate_raw_policy();
    calculate_host_policy();
    calculate_pv_max_policy();
    calculate_hvm_max_policy();
}

const uint32_t *lookup_deep_deps(uint32_t feature)
{
    static const struct {
        uint32_t feature;
        uint32_t fs[FSCAPINTS];
    } deep_deps[] = INIT_DEEP_DEPS;
    unsigned int start = 0, end = ARRAY_SIZE(deep_deps);

    BUILD_BUG_ON(ARRAY_SIZE(deep_deps) != NR_DEEP_DEPS);

    /* Fast early exit. */
    if ( !test_bit(feature, deep_features) )
        return NULL;

    /* deep_deps[] is sorted.  Perform a binary search. */
    while ( start < end )
    {
        unsigned int mid = start + ((end - start) / 2);

        if ( deep_deps[mid].feature > feature )
            end = mid;
        else if ( deep_deps[mid].feature < feature )
            start = mid + 1;
        else
            return deep_deps[mid].fs;
    }

    return NULL;
}

void recalculate_cpuid_policy(struct domain *d)
{
    struct cpuid_policy *p = d->arch.cpuid;
    const struct cpuid_policy *max =
        is_pv_domain(d) ? &pv_max_policy : &hvm_max_policy;
    uint32_t fs[FSCAPINTS], max_fs[FSCAPINTS];
    unsigned int i;

    p->basic.max_leaf   = min(p->basic.max_leaf,   max->basic.max_leaf);
    p->feat.max_subleaf = min(p->feat.max_subleaf, max->feat.max_subleaf);
    p->extd.max_leaf    = min(p->extd.max_leaf,    max->extd.max_leaf);

    cpuid_policy_to_featureset(p, fs);
    cpuid_policy_to_featureset(max, max_fs);

    /*
     * HVM domains using Shadow paging have further restrictions on their
     * available paging features.
     */
    if ( is_hvm_domain(d) && !hap_enabled(d) )
    {
        for ( i = 0; i < ARRAY_SIZE(max_fs); i++ )
            max_fs[i] &= hvm_shadow_featuremask[i];
    }

    /*
     * Allow the toolstack to set HTT, X2APIC and CMP_LEGACY.  These bits
     * affect how to interpret topology information in other cpuid leaves.
     */
    __set_bit(X86_FEATURE_HTT, max_fs);
    __set_bit(X86_FEATURE_X2APIC, max_fs);
    __set_bit(X86_FEATURE_CMP_LEGACY, max_fs);

    /*
     * 32bit PV domains can't use any Long Mode features, and cannot use
     * SYSCALL on non-AMD hardware.
     */
    if ( is_pv_32bit_domain(d) )
    {
        __clear_bit(X86_FEATURE_LM, max_fs);
        if ( boot_cpu_data.x86_vendor != X86_VENDOR_AMD )
            __clear_bit(X86_FEATURE_SYSCALL, max_fs);
    }

    /*
     * ITSC is masked by default (so domains are safe to migrate), but a
     * toolstack which has configured disable_migrate or vTSC for a domain may
     * safely select it, and needs a way of doing so.
     */
    if ( cpu_has_itsc && (d->disable_migrate || d->arch.vtsc) )
        __set_bit(X86_FEATURE_ITSC, max_fs);

    /* Clamp the toolstacks choices to reality. */
    for ( i = 0; i < ARRAY_SIZE(fs); i++ )
        fs[i] &= max_fs[i];

    if ( p->basic.max_leaf < XSTATE_CPUID )
        __clear_bit(X86_FEATURE_XSAVE, fs);

    sanitise_featureset(fs);

    /* Fold host's FDP_EXCP_ONLY and NO_FPU_SEL into guest's view. */
    fs[FEATURESET_7b0] &= ~special_features[FEATURESET_7b0];
    fs[FEATURESET_7b0] |= (host_policy.feat._7b0 &
                           special_features[FEATURESET_7b0]);

    cpuid_featureset_to_policy(fs, p);
    recalculate_xstate(p);
}

int init_domain_cpuid_policy(struct domain *d)
{
    unsigned int i;

    d->arch.cpuid = xmalloc(struct cpuid_policy);

    if ( !d->arch.cpuid )
        return -ENOMEM;

    *d->arch.cpuid = is_pv_domain(d) ? pv_max_policy : hvm_max_policy;

    recalculate_cpuid_policy(d);

    for ( i = 0; i < MAX_CPUID_INPUT; i++ )
    {
        d->arch.cpuid->legacy[i].input[0] = XEN_CPUID_INPUT_UNUSED;
        d->arch.cpuid->legacy[i].input[1] = XEN_CPUID_INPUT_UNUSED;
    }

    return 0;
}

static void domain_cpuid(const struct domain *d, uint32_t leaf,
                         uint32_t subleaf, struct cpuid_leaf *res)
{
    unsigned int i;

    for ( i = 0; i < MAX_CPUID_INPUT; i++ )
    {
        xen_domctl_cpuid_t *cpuid = &d->arch.cpuid->legacy[i];

        if ( (cpuid->input[0] == leaf) &&
             ((cpuid->input[1] == XEN_CPUID_INPUT_UNUSED) ||
              (cpuid->input[1] == subleaf)) )
        {
            *res = (struct cpuid_leaf){ cpuid->eax, cpuid->ebx,
                                        cpuid->ecx, cpuid->edx };
            return;
        }
    }
}

static void pv_cpuid(uint32_t leaf, uint32_t subleaf, struct cpuid_leaf *res)
{
    struct vcpu *curr = current;
    struct domain *currd = curr->domain;
    const struct cpuid_policy *p = currd->arch.cpuid;

    if ( !is_control_domain(currd) && !is_hardware_domain(currd) )
        domain_cpuid(currd, leaf, subleaf, res);
    else
        cpuid_count_leaf(leaf, subleaf, res);

    switch ( leaf )
    {
    case 0x00000001:
        res->c = p->basic._1c;
        res->d = p->basic._1d;

        if ( !is_pvh_domain(currd) )
        {
            const struct cpu_user_regs *regs = guest_cpu_user_regs();

            /*
             * Delete the PVH condition when HVMLite formally replaces PVH,
             * and HVM guests no longer enter a PV codepath.
             */

            /*
             * !!! OSXSAVE handling for PV guests is non-architectural !!!
             *
             * Architecturally, the correct code here is simply:
             *
             *   if ( curr->arch.pv_vcpu.ctrlreg[4] & X86_CR4_OSXSAVE )
             *       c |= cpufeat_mask(X86_FEATURE_OSXSAVE);
             *
             * However because of bugs in Xen (before c/s bd19080b, Nov 2010,
             * the XSAVE cpuid flag leaked into guests despite the feature not
             * being available for use), buggy workarounds where introduced to
             * Linux (c/s 947ccf9c, also Nov 2010) which relied on the fact
             * that Xen also incorrectly leaked OSXSAVE into the guest.
             *
             * Furthermore, providing architectural OSXSAVE behaviour to a
             * many Linux PV guests triggered a further kernel bug when the
             * fpu code observes that XSAVEOPT is available, assumes that
             * xsave state had been set up for the task, and follows a wild
             * pointer.
             *
             * Older Linux PVOPS kernels however do require architectural
             * behaviour.  They observe Xen's leaked OSXSAVE and assume they
             * can already use XSETBV, dying with a #UD because the shadowed
             * CR4.OSXSAVE is clear.  This behaviour has been adjusted in all
             * observed cases via stable backports of the above changeset.
             *
             * Therefore, the leaking of Xen's OSXSAVE setting has become a
             * defacto part of the PV ABI and can't reasonably be corrected.
             * It can however be restricted to only the enlightened CPUID
             * view, as seen by the guest kernel.
             *
             * The following situations and logic now applies:
             *
             * - Hardware without CPUID faulting support and native CPUID:
             *    There is nothing Xen can do here.  The hosts XSAVE flag will
             *    leak through and Xen's OSXSAVE choice will leak through.
             *
             *    In the case that the guest kernel has not set up OSXSAVE, only
             *    SSE will be set in xcr0, and guest userspace can't do too much
             *    damage itself.
             *
             * - Enlightened CPUID or CPUID faulting available:
             *    Xen can fully control what is seen here.  Guest kernels need
             *    to see the leaked OSXSAVE via the enlightened path, but
             *    guest userspace and the native is given architectural
             *    behaviour.
             *
             *    Emulated vs Faulted CPUID is distinguised based on whether a
             *    #UD or #GP is currently being serviced.
             */
            /* OSXSAVE clear in policy.  Fast-forward CR4 back in. */
            if ( (curr->arch.pv_vcpu.ctrlreg[4] & X86_CR4_OSXSAVE) ||
                 (regs->entry_vector == TRAP_invalid_op &&
                  guest_kernel_mode(curr, regs) &&
                  (read_cr4() & X86_CR4_OSXSAVE)) )
                res->c |= cpufeat_mask(X86_FEATURE_OSXSAVE);

            /*
             * At the time of writing, a PV domain is the only viable option
             * for Dom0.  Several interactions between dom0 and Xen for real
             * hardware setup have unfortunately been implemented based on
             * state which incorrectly leaked into dom0.
             *
             * These leaks are retained for backwards compatibility, but
             * restricted to the hardware domains kernel only.
             */
            if ( is_hardware_domain(currd) && guest_kernel_mode(curr, regs) )
            {
                /*
                 * MTRR used to unconditionally leak into PV guests.  They
                 * cannot MTRR infrastructure at all, and shouldn't be able to
                 * see the feature.
                 *
                 * Modern PVOPS Linux self-clobbers the MTRR feature, to avoid
                 * trying to use the associated MSRs.  Xenolinux-based PV dom0's
                 * however use the MTRR feature as an indication of the presence
                 * of the XENPF_{add,del,read}_memtype hypercalls.
                 */
                if ( cpu_has_mtrr )
                    res->d |= cpufeat_mask(X86_FEATURE_MTRR);

                /*
                 * MONITOR never leaked into PV guests, as PV guests cannot
                 * use the MONITOR/MWAIT instructions.  As such, they require
                 * the feature to not being present in emulated CPUID.
                 *
                 * Modern PVOPS Linux try to be cunning and use native CPUID
                 * to see if the hardware actually supports MONITOR, and by
                 * extension, deep C states.
                 *
                 * If the feature is seen, deep-C state information is
                 * obtained from the DSDT and handed back to Xen via the
                 * XENPF_set_processor_pminfo hypercall.
                 *
                 * This mechanism is incompatible with an HVM-based hardware
                 * domain, and also with CPUID Faulting.
                 *
                 * Luckily, Xen can be just as 'cunning', and distinguish an
                 * emulated CPUID from a faulted CPUID by whether a #UD or #GP
                 * fault is currently being serviced.  Yuck...
                 */
                if ( cpu_has_monitor && regs->entry_vector == TRAP_gp_fault )
                    res->c |= cpufeat_mask(X86_FEATURE_MONITOR);

                /*
                 * While MONITOR never leaked into PV guests, EIST always used
                 * to.
                 *
                 * Modern PVOPS will only parse P state information from the
                 * DSDT and return it to Xen if EIST is seen in the emulated
                 * CPUID information.
                 */
                if ( cpu_has_eist )
                    res->c |= cpufeat_mask(X86_FEATURE_EIST);
            }
        }

        if ( vpmu_enabled(curr) &&
             vpmu_is_set(vcpu_vpmu(curr), VPMU_CPU_HAS_DS) )
        {
            res->d |= cpufeat_mask(X86_FEATURE_DS);
            if ( cpu_has(&current_cpu_data, X86_FEATURE_DTES64) )
                res->c |= cpufeat_mask(X86_FEATURE_DTES64);
            if ( cpu_has(&current_cpu_data, X86_FEATURE_DSCPL) )
                res->c |= cpufeat_mask(X86_FEATURE_DSCPL);
        }
        break;

    case 0x0000000a: /* Architectural Performance Monitor Features (Intel) */
        if ( boot_cpu_data.x86_vendor != X86_VENDOR_INTEL ||
             !vpmu_enabled(curr) )
            goto unsupported;

        /* Report at most version 3 since that's all we currently emulate. */
        if ( (res->a & 0xff) > 3 )
            res->a = (res->a & ~0xff) | 3;
        break;

    case 0x80000001:
        res->c = p->extd.e1c;
        res->d = p->extd.e1d;

        /* If not emulating AMD, clear the duplicated features in e1d. */
        if ( p->x86_vendor != X86_VENDOR_AMD )
            res->d &= ~CPUID_COMMON_1D_FEATURES;

        /*
         * MTRR used to unconditionally leak into PV guests.  They cannot MTRR
         * infrastructure at all, and shouldn't be able to see the feature.
         *
         * Modern PVOPS Linux self-clobbers the MTRR feature, to avoid trying
         * to use the associated MSRs.  Xenolinux-based PV dom0's however use
         * the MTRR feature as an indication of the presence of the
         * XENPF_{add,del,read}_memtype hypercalls.
         */
        if ( is_hardware_domain(currd) && cpu_has_mtrr &&
             guest_kernel_mode(curr, guest_cpu_user_regs()) )
            res->d |= cpufeat_mask(X86_FEATURE_MTRR);
        break;

    case 0x80000007:
        res->d = p->extd.e7d;
        break;

    case 0x80000008:
        res->a = paddr_bits | (vaddr_bits << 8);
        res->b = p->extd.e8b;
        break;

    case 0x00000005: /* MONITOR/MWAIT */
    case 0x0000000b: /* Extended Topology Enumeration */
    case 0x8000000a: /* SVM revision and features */
    case 0x8000001b: /* Instruction Based Sampling */
    case 0x8000001c: /* Light Weight Profiling */
    case 0x8000001e: /* Extended topology reporting */
    unsupported:
        *res = EMPTY_LEAF;
        break;

    case 0x7:
    case XSTATE_CPUID:
        ASSERT_UNREACHABLE();
        /* Now handled in guest_cpuid(). */
    }
}

static void hvm_cpuid(uint32_t leaf, uint32_t subleaf, struct cpuid_leaf *res)
{
    struct vcpu *v = current;
    struct domain *d = v->domain;
    const struct cpuid_policy *p = d->arch.cpuid;

    domain_cpuid(d, leaf, subleaf, res);

    switch ( leaf )
    {
        unsigned int tmp;

    case 0x1:
        /* Fix up VLAPIC details. */
        res->b &= 0x00FFFFFFu;
        res->b |= (v->vcpu_id * 2) << 24;

        res->c = p->basic._1c;
        res->d = p->basic._1d;

        /* APIC exposed to guests, but Fast-forward MSR_APIC_BASE.EN back in. */
        if ( vlapic_hw_disabled(vcpu_vlapic(v)) )
            res->d &= ~cpufeat_bit(X86_FEATURE_APIC);

        /* OSXSAVE clear in policy.  Fast-forward CR4 back in. */
        if ( v->arch.hvm_vcpu.guest_cr[4] & X86_CR4_OSXSAVE )
            res->c |= cpufeat_mask(X86_FEATURE_OSXSAVE);

        /*
         * PSE36 is not supported in shadow mode.  This bit should be
         * unilaterally cleared.
         *
         * However, an unspecified version of Hyper-V from 2011 refuses
         * to start as the "cpu does not provide required hw features" if
         * it can't see PSE36.
         *
         * As a workaround, leak the toolstack-provided PSE36 value into a
         * shadow guest if the guest is already using PAE paging (and won't
         * care about reverting back to PSE paging).  Otherwise, knoble it, so
         * a 32bit guest doesn't get the impression that it could try to use
         * PSE36 paging.
         */
        if ( !hap_enabled(d) && !(hvm_pae_enabled(v) || hvm_long_mode_enabled(v)) )
            res->d &= ~cpufeat_mask(X86_FEATURE_PSE36);

        if ( vpmu_enabled(v) &&
             vpmu_is_set(vcpu_vpmu(v), VPMU_CPU_HAS_DS) )
        {
            res->d |= cpufeat_mask(X86_FEATURE_DS);
            if ( cpu_has(&current_cpu_data, X86_FEATURE_DTES64) )
                res->c |= cpufeat_mask(X86_FEATURE_DTES64);
            if ( cpu_has(&current_cpu_data, X86_FEATURE_DSCPL) )
                res->c |= cpufeat_mask(X86_FEATURE_DSCPL);
        }

        break;

    case 0xb:
        /* Fix the x2APIC identifier. */
        res->d = v->vcpu_id * 2;
        break;

    case 0x0000000a: /* Architectural Performance Monitor Features (Intel) */
        if ( boot_cpu_data.x86_vendor != X86_VENDOR_INTEL || !vpmu_enabled(v) )
        {
            *res = EMPTY_LEAF;
            break;
        }

        /* Report at most version 3 since that's all we currently emulate */
        if ( (res->a & 0xff) > 3 )
            res->a = (res->a & ~0xff) | 3;
        break;

    case 0x80000001:
        res->c = p->extd.e1c;
        res->d = p->extd.e1d;

        /* If not emulating AMD, clear the duplicated features in e1d. */
        if ( p->x86_vendor != X86_VENDOR_AMD )
            res->d &= ~CPUID_COMMON_1D_FEATURES;
        /* fast-forward MSR_APIC_BASE.EN if it hasn't already been clobbered. */
        else if ( vlapic_hw_disabled(vcpu_vlapic(v)) )
            res->d &= ~cpufeat_bit(X86_FEATURE_APIC);

        /*
         * PSE36 is not supported in shadow mode.  This bit should be
         * unilaterally cleared.
         *
         * However, an unspecified version of Hyper-V from 2011 refuses
         * to start as the "cpu does not provide required hw features" if
         * it can't see PSE36.
         *
         * As a workaround, leak the toolstack-provided PSE36 value into a
         * shadow guest if the guest is already using PAE paging (and won't
         * care about reverting back to PSE paging).  Otherwise, knoble it, so
         * a 32bit guest doesn't get the impression that it could try to use
         * PSE36 paging.
         */
        if ( !hap_enabled(d) && !(hvm_pae_enabled(v) || hvm_long_mode_enabled(v)) )
            res->d &= ~cpufeat_mask(X86_FEATURE_PSE36);

        /* SYSCALL is hidden outside of long mode on Intel. */
        if ( p->x86_vendor == X86_VENDOR_INTEL && !hvm_long_mode_enabled(v) )
            res->d &= ~cpufeat_mask(X86_FEATURE_SYSCALL);

        break;

    case 0x80000007:
        res->d = p->extd.e7d;
        break;

    case 0x80000008:
        res->a &= 0xff;
        tmp = d->arch.paging.gfn_bits + PAGE_SHIFT;
        if ( res->a > tmp )
            res->a = tmp;

        tmp = (p->basic.pae || p->basic.pse36) ? 36 : 32;
        if ( res->a < tmp )
            res->a = tmp;

        res->a |= (p->extd.lm ? vaddr_bits : 32) << 8;

        res->b = p->extd.e8b;
        break;

    case 0x8000001c:
        if ( !cpu_has_svm )
        {
            *res = EMPTY_LEAF;
            break;
        }

        if ( cpu_has_lwp && (v->arch.xcr0 & XSTATE_LWP) )
            /* Turn on available bit and other features specified in lwp_cfg. */
            res->a = (res->d & v->arch.hvm_svm.guest_lwp_cfg) | 1;
        else
            res->a = 0;
        break;

    case 0x7:
    case XSTATE_CPUID:
        ASSERT_UNREACHABLE();
        /* Now handled in guest_cpuid(). */
    }
}

void guest_cpuid(const struct vcpu *v, uint32_t leaf,
                 uint32_t subleaf, struct cpuid_leaf *res)
{
    const struct domain *d = v->domain;
    const struct cpuid_policy *p = d->arch.cpuid;

    *res = EMPTY_LEAF;

    /*
     * First pass:
     * - Perform max_leaf/subleaf calculations.  Out-of-range leaves return
     *   all zeros, following the AMD model.
     * - Fill in *res for leaves no longer handled on the legacy path.
     * - Dispatch the virtualised leaves to their respective handlers.
     */
    switch ( leaf )
    {
    case 0 ... CPUID_GUEST_NR_BASIC - 1:
        if ( leaf > p->basic.max_leaf )
            return;

        switch ( leaf )
        {
        case 0x7:
            if ( subleaf > p->feat.max_subleaf )
                return;

            BUG_ON(subleaf >= ARRAY_SIZE(p->feat.raw));
            *res = p->feat.raw[subleaf];
            break;

        case XSTATE_CPUID:
            if ( !p->basic.xsave || subleaf >= ARRAY_SIZE(p->xstate.raw) )
                return;

            *res = p->xstate.raw[subleaf];
            break;

        default:
            goto legacy;
        }
        break;

    case 0x40000000 ... 0x400000ff:
        if ( is_viridian_domain(d) )
            return cpuid_viridian_leaves(v, leaf, subleaf, res);

        /*
         * Fallthrough.
         *
         * Intel reserve up until 0x4fffffff for hypervisor use.  AMD reserve
         * only until 0x400000ff, but we already use double that.
         */
    case 0x40000100 ... 0x400001ff:
        return cpuid_hypervisor_leaves(v, leaf, subleaf, res);

    case 0x80000000 ... 0x80000000 + CPUID_GUEST_NR_EXTD - 1:
        if ( leaf > p->extd.max_leaf )
            return;
        goto legacy;

    default:
        return;
    }

    /*
     * Skip dynamic adjustments if we are in the wrong context.
     *
     * All dynamic adjustments depends on current register state, which will
     * be stale if the vcpu is running elsewhere.  It is simpler, quicker, and
     * more reliable for the caller to do nothing (consistently) than to hand
     * back stale data which it can't use safely.
     */
    if ( v != current )
        return;

    /*
     * Second pass:
     * - Dynamic adjustments
     */
    switch ( leaf )
    {
    case 0x7:
        switch ( subleaf )
        {
        case 0:
            /* OSPKE clear in policy.  Fast-forward CR4 back in. */
            if ( (is_pv_domain(d)
                  ? v->arch.pv_vcpu.ctrlreg[4]
                  : v->arch.hvm_vcpu.guest_cr[4]) & X86_CR4_PKE )
                res->c |= cpufeat_mask(X86_FEATURE_OSPKE);
            break;
        }
        break;

    case XSTATE_CPUID:
        switch ( subleaf )
        {
        case 1:
            if ( p->xstate.xsaves )
            {
                /*
                 * TODO: Figure out what to do for XSS state.  VT-x manages
                 * host vs guest MSR_XSS automatically, so as soon as we start
                 * supporting any XSS states, the wrong XSS will be in
                 * context.
                 */
                BUILD_BUG_ON(XSTATE_XSAVES_ONLY != 0);

                /*
                 * Read CPUID[0xD,0/1].EBX from hardware.  They vary with
                 * enabled XSTATE, and appropraite XCR0|XSS are in context.
                 */
        case 0:
                res->b = cpuid_count_ebx(leaf, subleaf);
            }
            break;
        }
        break;
    }

    /* Done. */
    return;

 legacy:
    /* {hvm,pv}_cpuid() have this expectation. */
    ASSERT(v == current);
    (is_hvm_domain(d) ? hvm_cpuid : pv_cpuid)(leaf, subleaf, res);
}

static void __init __maybe_unused build_assertions(void)
{
    BUILD_BUG_ON(ARRAY_SIZE(known_features) != FSCAPINTS);
    BUILD_BUG_ON(ARRAY_SIZE(special_features) != FSCAPINTS);
    BUILD_BUG_ON(ARRAY_SIZE(pv_featuremask) != FSCAPINTS);
    BUILD_BUG_ON(ARRAY_SIZE(hvm_shadow_featuremask) != FSCAPINTS);
    BUILD_BUG_ON(ARRAY_SIZE(hvm_hap_featuremask) != FSCAPINTS);
    BUILD_BUG_ON(ARRAY_SIZE(deep_features) != FSCAPINTS);

    /* Find some more clever allocation scheme if this trips. */
    BUILD_BUG_ON(sizeof(struct cpuid_policy) > PAGE_SIZE);

    BUILD_BUG_ON(sizeof(raw_policy.basic) !=
                 sizeof(raw_policy.basic.raw));
    BUILD_BUG_ON(sizeof(raw_policy.feat) !=
                 sizeof(raw_policy.feat.raw));
    BUILD_BUG_ON(sizeof(raw_policy.xstate) !=
                 sizeof(raw_policy.xstate.raw));
    BUILD_BUG_ON(sizeof(raw_policy.extd) !=
                 sizeof(raw_policy.extd.raw));
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
