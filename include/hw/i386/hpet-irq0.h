/* Experimental q35 legacy timer interrupt route. */
#ifndef HW_I386_HPET_IRQ0_H
#define HW_I386_HPET_IRQ0_H

void x86_hpet_irq0_enable(void);
void x86_hpet_irq0_revoke(void) __attribute__((weak));
void x86_hpet_irq0_revoke_timer(void) __attribute__((weak));
void x86_hpet_irq0_external_begin(void) __attribute__((weak));
void x86_hpet_irq0_external_end(void) __attribute__((weak));
bool x86_hpet_irq0_try_pulse(void) __attribute__((weak));
void x86_hpet_irq0_refresh(void) __attribute__((weak));
void x86_hpet_irq0_flush_stats(void) __attribute__((weak));
bool x86_hpet_irq0_try_cached(void) __attribute__((weak));
void x86_hpet_irq0_report(const char *phase) __attribute__((weak));
bool ioapic_hpet_irq0_route(uint32_t *addr, uint32_t *data);
void ioapic_hpet_irq0_stats(void);
void ioapic_hpet_irq0_add_edges(uint64_t count);

static inline void hpet_irq0_revoke_route(void)
{
    if (x86_hpet_irq0_revoke) {
        x86_hpet_irq0_revoke();
    }
}

static inline void hpet_irq0_revoke_timer(void)
{
    if (x86_hpet_irq0_revoke_timer) {
        x86_hpet_irq0_revoke_timer();
    }
}

static inline void hpet_irq0_external_begin(void)
{
    if (x86_hpet_irq0_external_begin) {
        x86_hpet_irq0_external_begin();
    }
}

static inline void hpet_irq0_external_end(void)
{
    if (x86_hpet_irq0_external_end) {
        x86_hpet_irq0_external_end();
    }
}

#endif
