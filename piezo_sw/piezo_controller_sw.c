#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/pio.h"
#include "tdm.pio.h"

PIO pio;
uint sm_clock, sm_tdm;

uint tdm_mclk_pin = 16;
uint tdm_stream_pin = 17;
uint tdm_stream_pincount = 3;

void init_pio();
void init_tdm_pio();
void init_mclk_pio();

int main()
{
    pll_init(pll_sys, 1, 1536 * MHZ, 6, 2); // set sys clock to 128MHz
    init_pio();

    return 0;
}

void init_pio()
{
    pio = pio0;
    init_tdm_pio();
    init_mclk_pio();
    // enable both sm's at the same time. This causes them to be synced.
    hw_set_bits(&pio->ctrl, (1 << (PIO_CTRL_SM_ENABLE_LSB + sm_tdm)) | (1 << (PIO_CTRL_SM_ENABLE_LSB + sm_clock)));
}

void init_tdm_pio()
{
    uint offset = pio_add_program(pio, &tdm_program);
    sm_tdm = pio_claim_unused_sm(pio, true);
}

void init_mclk_pio()
{
    uint offset = pio_add_program(pio, &tdm_output_mclk_program);
    pio_sm_config c = tdm_output_mclk_program_get_default_config(offset);
    pio_gpio_init(pio, tdm_mclk_pin);
    sm_config_set_sideset_pins(&c, tdm_mclk_pin);
    sm_clock = pio_claim_unused_sm(pio, true);
}