#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "tdm.pio.h"

#define SAMPLE_BUF_SIZE 8
#define TDM_MCLK_PIN 15

PIO pio;
uint sm_clock, sm_tdm;

uint tdm_stream_pin = 11;
uint tdm_stream_pincount = 4;
uint tdm_stream_input_pin = 14;

uint input_channel, output_channel;

typedef struct doublebuffer
{
    uint32_t buf1[SAMPLE_BUF_SIZE], buf2[SAMPLE_BUF_SIZE];
    uint32_t *reading, *writing;
    uint32_t should_swap;
} doublebuffer;

void init_tdm();
void init_tdm_pio();
void init_mclk_pio();
void init_sample_dma();

void read_irq_dma_handler();
void write_irq_dma_handler();

void init_doublebuffer(doublebuffer *b);

doublebuffer input_buf, output_buf;

// use f_mclk = 2MHz this gives Fs = 7812.5Hz should be sufficient for our low speed application
// this puts the device in ssm (single speed mode)

int main()
{
    set_sys_clock_khz(128000, true); // set sys clock to 128MHz

    init_tdm();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    while (1)
    {
        pio_sm_put(pio, sm_tdm, 0x55555555);
        pio_sm_get(pio, sm_tdm);
        // gpio_put(PICO_DEFAULT_LED_PIN, 1);
        // sleep_ms(250);
        // gpio_put(PICO_DEFAULT_LED_PIN, 0);
        // sleep_ms(250);
    }
    return 0;
}

void init_tdm()
{
    pio = pio0;
    init_tdm_pio();
    init_mclk_pio();

    pio_sm_put_blocking(pio, sm_tdm, 254); // magic counter value needed for tdm
    // enable both sm's at the same time. This causes them to be synced.
    hw_set_bits(&pio->ctrl, (1 << (PIO_CTRL_SM_ENABLE_LSB + sm_tdm)) | (1 << (PIO_CTRL_SM_ENABLE_LSB + sm_clock)));

    // init_sample_dma();
}

void init_doublebuffer(doublebuffer *b)
{
    b->reading = b->buf1;
    b->writing = b->buf2;
    b->should_swap = 1;
}

void init_sample_dma()
{
    init_doublebuffer(&input_buf);
    init_doublebuffer(&output_buf);

    input_channel = dma_claim_unused_channel(true);
    dma_channel_config input_dma_c = dma_channel_get_default_config(input_channel);
    channel_config_set_read_increment(&input_dma_c, false);
    channel_config_set_write_increment(&input_dma_c, true);
    channel_config_set_transfer_data_size(&input_dma_c, DMA_SIZE_32);
    channel_config_set_dreq(&input_dma_c, pio_get_dreq(pio, sm_tdm, false));
    dma_channel_set_irq0_enabled(input_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, read_irq_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_configure(input_channel, &input_dma_c, NULL, &pio->rxf[sm_tdm], SAMPLE_BUF_SIZE, false);

    output_channel = dma_claim_unused_channel(true);
    dma_channel_config output_dma_c = dma_channel_get_default_config(output_channel);
    channel_config_set_read_increment(&output_dma_c, true);
    channel_config_set_write_increment(&output_dma_c, false);
    channel_config_set_transfer_data_size(&output_dma_c, DMA_SIZE_32);
    channel_config_set_dreq(&output_dma_c, pio_get_dreq(pio, sm_tdm, true));
    dma_channel_set_irq1_enabled(output_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_1, write_irq_dma_handler);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_configure(output_channel, &output_dma_c, &pio->txf[sm_tdm], NULL, SAMPLE_BUF_SIZE, false);
}

void init_tdm_pio()
{
    uint offset = pio_add_program(pio, &tdm_program);
    sm_tdm = pio_claim_unused_sm(pio, true);
    pio_sm_config c = tdm_program_get_default_config(offset);
    for (int i = 0; i < tdm_stream_pincount; i++)
    {
        pio_gpio_init(pio, tdm_stream_pin + i);
    }
    // see comment in init_mclk_pio. however this takes 4 cycles per clock not 2 as the other sm
    sm_config_set_clkdiv(&c, 16);

    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_out_shift(&c, true, true, 32);

    sm_config_set_sideset_pins(&c, tdm_stream_pin);
    pio_sm_set_consecutive_pindirs(pio, sm_tdm, tdm_stream_pin, tdm_stream_pincount, true);
    pio_sm_set_consecutive_pindirs(pio, sm_tdm, tdm_stream_input_pin, 1, false);

    pio_sm_init(pio, sm_tdm, offset, &c);
}

void init_mclk_pio()
{
    sm_clock = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &tdm_output_mclk_program);

    pio_gpio_init(pio, TDM_MCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm_clock, TDM_MCLK_PIN, 1, true);

    pio_sm_config c = tdm_output_mclk_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, TDM_MCLK_PIN);

    // sys clk at 128MHz target is 8Mhz so that the toggles produce a clock at 2MHz thus 128/8 = 16
    sm_config_set_clkdiv(&c, 16);
    pio_sm_init(pio, sm_clock, offset, &c);
}

void swap_doublebuffer(doublebuffer *b)
{
    if (b->should_swap)
    {
        uint32_t *tmp = b->reading;
        b->reading = b->writing;
        b->writing = tmp;
        b->should_swap = 0;
    }
}

void read_irq_dma_handler()
{
    swap_doublebuffer(&input_buf);
    dma_channel_set_write_addr(input_channel, input_buf.writing, true);
}

void write_irq_dma_handler()
{
    swap_doublebuffer(&output_buf);
    dma_channel_set_read_addr(output_channel, output_buf.reading, true);
}