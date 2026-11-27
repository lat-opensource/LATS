#ifndef APIC_H
#define APIC_H


/* apic.c */
void apic_deliver_irq(uint8_t dest, uint8_t dest_mode, uint8_t delivery_mode,
                      uint8_t vector_num, uint8_t trigger_mode);
int apic_accept_pic_intr(DeviceState *s);
void apic_deliver_pic_intr(DeviceState *s, int level);
void apic_deliver_nmi(DeviceState *d);
int apic_get_interrupt(DeviceState *s);
void cpu_set_apic_base(DeviceState *s, uint64_t val);
uint64_t cpu_get_apic_base(DeviceState *s);
void cpu_set_apic_tpr(DeviceState *s, uint8_t val);
uint8_t cpu_get_apic_tpr(DeviceState *s);
void apic_init_reset(DeviceState *s);
void apic_set_id(DeviceState *dev, uint8_t id);
void apic_ipi_quiesce(void);
void apic_set_msi_paused(bool paused);
void apic_ipi_resume(void);
void apic_ipi_unplug(DeviceState *dev);
void apic_sipi(DeviceState *s);
void apic_poll_irq(DeviceState *d);
bool apic_try_set_local_tpr(DeviceState *dev, uint8_t value);
int apic_try_ack_local_hard(DeviceState *dev);
int apic_try_ack_pic_or_local_hard(DeviceState *dev, CPUState *cs);
bool apic_try_poll_irq(DeviceState *dev, int request);
void apic_designate_bsp(DeviceState *d, bool bsp);
int apic_get_highest_priority_irr(DeviceState *dev);
void apic_local_deliver_timer(void);

/* pc.c */
DeviceState *cpu_get_current_apic(void);

#endif
