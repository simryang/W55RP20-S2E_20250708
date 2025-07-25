/**
 * Copyright (c) 2022 WIZnet Co.,Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * ----------------------------------------------------------------------------------------------------
 * Includes
 * ----------------------------------------------------------------------------------------------------
 */
#include <stdio.h>
#include "WIZnet_board.h"
#include "port_common.h"

#include "wizchip_conf.h"
#include "w5x00_spi.h"
#if ((DEVICE_BOARD_NAME == W55RP20_S2E) || (DEVICE_BOARD_NAME == W232N) || (DEVICE_BOARD_NAME == IP20))
#include "wiznet_spi_pio.h"
#endif

/**
 * ----------------------------------------------------------------------------------------------------
 * Macros
 * ----------------------------------------------------------------------------------------------------
 */
/* SPI */
#if ((DEVICE_BOARD_NAME == W55RP20_S2E) || (DEVICE_BOARD_NAME == W232N) || (DEVICE_BOARD_NAME == IP20))
#define USE_SPI_PIO
#elif (DEVICE_BOARD_NAME == WIZ5XXSR_RP)
/* SPI */
#define SPI_PORT spi0
/* Use SPI DMA */
#define USE_SPI_DMA // if you want to use SPI DMA, uncomment.
#endif

/**
 * ----------------------------------------------------------------------------------------------------
 * Variables
 * ----------------------------------------------------------------------------------------------------
 */

#ifdef USE_SPI_DMA
static uint dma_tx;
static uint dma_rx;
static dma_channel_config dma_channel_config_tx;
static dma_channel_config dma_channel_config_rx;
#endif

extern xSemaphoreHandle wizchip_critical_sem;

#ifdef USE_SPI_PIO
wiznet_spi_config_t g_spi_config = {
    .data_in_pin = WIZCHIP_PIN_MISO,
    .data_out_pin = WIZCHIP_PIN_MOSI,
    .cs_pin = WIZCHIP_PIN_CS,
    .clock_pin = WIZCHIP_PIN_SCK,
    .reset_pin = WIZCHIP_PIN_RST,
    .clock_div_major = 2,  // 4,
    .clock_div_minor = 0,
};

wiznet_spi_handle_t spi_handle;
#endif
/**
 * ----------------------------------------------------------------------------------------------------
 * Functions
 * ----------------------------------------------------------------------------------------------------
 */
static inline void wizchip_select(void)
{
    gpio_put(WIZCHIP_PIN_CS, 0);
}

static inline void wizchip_deselect(void)
{
    gpio_put(WIZCHIP_PIN_CS, 1);
}

void wizchip_reset()
{
    gpio_init(WIZCHIP_PIN_RST);
    gpio_pull_up(WIZCHIP_PIN_RST);
    gpio_set_dir(WIZCHIP_PIN_RST, GPIO_OUT);
    sleep_ms(5);

    gpio_put(WIZCHIP_PIN_RST, 0);
    sleep_ms(100);
 
    gpio_put(WIZCHIP_PIN_RST, 1);
    sleep_ms(100);
    bi_decl(bi_1pin_with_name(WIZCHIP_PIN_RST, "W5x00 RESET"));
}

#ifndef USE_SPI_PIO
static uint8_t wizchip_read(void)
{
    uint8_t rx_data = 0;
    uint8_t tx_data = 0xFF;

    spi_read_blocking(SPI_PORT, tx_data, &rx_data, 1);

    return rx_data;
}

static void wizchip_write(uint8_t tx_data)
{
    spi_write_blocking(SPI_PORT, &tx_data, 1);
}

#ifdef USE_SPI_DMA
static void wizchip_read_burst(uint8_t *pBuf, uint16_t len)
{
    uint8_t dummy_data = 0xFF;

    channel_config_set_read_increment(&dma_channel_config_tx, false);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          &dummy_data,               // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          pBuf,                      // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}

static void wizchip_write_burst(uint8_t *pBuf, uint16_t len)
{
    uint8_t dummy_data;

    channel_config_set_read_increment(&dma_channel_config_tx, true);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          pBuf,                      // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, false);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          &dummy_data,               // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}
#endif
#endif

static void wizchip_critical_section_lock(void)
{
    xSemaphoreTake(wizchip_critical_sem, portMAX_DELAY);
}

static void wizchip_critical_section_unlock(void)
{
    xSemaphoreGive(wizchip_critical_sem);
}

void wizchip_spi_initialize(uint32_t spi_clock)
{
#ifdef USE_SPI_PIO
    spi_handle = wiznet_spi_pio_open(&g_spi_config);
    (*spi_handle)->set_active(spi_handle);

#else
    spi_init(SPI_PORT, spi_clock);

    gpio_set_function(WIZCHIP_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(WIZCHIP_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(WIZCHIP_PIN_MISO, GPIO_FUNC_SPI);

    // make the SPI pins available to picotool
    bi_decl(bi_3pins_with_func(WIZCHIP_PIN_MISO, WIZCHIP_PIN_MOSI, WIZCHIP_PIN_SCK, GPIO_FUNC_SPI));

    // chip select is active-low, so we'll initialise it to a driven-high state
    gpio_init(WIZCHIP_PIN_CS);
    gpio_set_dir(WIZCHIP_PIN_CS, GPIO_OUT);
    gpio_put(WIZCHIP_PIN_CS, 1);

    // make the SPI pins available to picotool
    bi_decl(bi_1pin_with_name(WIZCHIP_PIN_CS, "W5x00 CHIP SELECT"));

#ifdef USE_SPI_DMA
    dma_tx = dma_claim_unused_channel(true);
    dma_rx = dma_claim_unused_channel(true);

    dma_channel_config_tx = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_channel_config_tx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_tx, DREQ_SPI0_TX);

    // We set the inbound DMA to transfer from the SPI receive FIFO to a memory buffer paced by the SPI RX FIFO DREQ
    // We coinfigure the read address to remain unchanged for each element, but the write
    // address to increment (so data is written throughout the buffer)
    dma_channel_config_rx = dma_channel_get_default_config(dma_rx);
    channel_config_set_transfer_data_size(&dma_channel_config_rx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_rx, DREQ_SPI0_RX);
    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
#endif
#endif
}

void wizchip_cris_initialize(void)
{
    wizchip_critical_sem = xSemaphoreCreateCounting((unsigned portBASE_TYPE)0x7fffffff, (unsigned portBASE_TYPE)1);
    reg_wizchip_cris_cbfunc(wizchip_critical_section_lock, wizchip_critical_section_unlock);
}

void wizchip_initialize(void)
{
#ifdef USE_SPI_PIO
    (*spi_handle)->frame_end();

    reg_wizchip_spi_cbfunc((*spi_handle)->read_byte, (*spi_handle)->write_byte);
    reg_wizchip_spiburst_cbfunc((*spi_handle)->read_buffer, (*spi_handle)->write_buffer);
    reg_wizchip_cs_cbfunc((*spi_handle)->frame_start, (*spi_handle)->frame_end);
#else

    /* CS function register */
    reg_wizchip_cs_cbfunc(wizchip_select, wizchip_deselect);

    /* SPI function register */
    reg_wizchip_spi_cbfunc(wizchip_read, wizchip_write);
#endif
#ifdef USE_SPI_DMA
    reg_wizchip_spiburst_cbfunc(wizchip_read_burst, wizchip_write_burst);
#endif
    /* W5x00 initialize */
#if (_WIZCHIP_ == W5100S)
    uint8_t memsize[2][4] = {{2, 2, 2, 2}, {2, 2, 2, 2}};
#elif (_WIZCHIP_ == W5500)
    uint8_t memsize[2][8] = {{2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 2, 2, 2, 2, 2}};
#endif

    if (ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1)
    {
        printf(" W5x00 initialized fail\n");
        return;
    }

#if 0
    /* Check PHY link status */
    do
    {
        if (ctlwizchip(CW_GET_PHYLINK, (void *)&temp) == -1)
        {
            printf(" Unknown PHY link status\n");

            return;
        }
    } while (temp == PHY_LINK_OFF);
#endif
}

void wizchip_check(void)
{
    uint8_t ver;
#if (_WIZCHIP_ == W5100S)
    /* Read version register */
    if (getVER() != 0x51)
    {
        printf(" ACCESS ERR : VERSION != 0x51, read value = 0x%02x\n", getVER());

        while (1)
            ;
    }
#elif (_WIZCHIP_ == W5500)
    /* Read version register */
    ver = getVERSIONR();
    printf("ver = 0x%02X\r\n", ver);
    if (ver != 0x04)
    {
        printf(" ACCESS ERR : VERSION != 0x04, read value = 0x%02x\n", getVERSIONR());

        while (1)
            ;
    }
#endif
}

/* Network */
void network_initialize(wiz_NetInfo net_info)
{
    ctlnetwork(CN_SET_NETINFO, (void *)&net_info);
}

void print_network_information(wiz_NetInfo net_info)
{
    uint8_t tmp_str[8] = {
        0,
    };

    ctlnetwork(CN_GET_NETINFO, (void *)&net_info);
    ctlwizchip(CW_GET_ID, (void *)tmp_str);

    if (net_info.dhcp == NETINFO_DHCP)
    {
        printf("====================================================================================================\n");
        printf(" %s network configuration : DHCP\n\n", (char *)tmp_str);
    }
    else
    {
        printf("====================================================================================================\n");
        printf(" %s network configuration : static\n\n", (char *)tmp_str);
    }

    printf(" MAC         : %02X:%02X:%02X:%02X:%02X:%02X\n", net_info.mac[0], net_info.mac[1], net_info.mac[2], net_info.mac[3], net_info.mac[4], net_info.mac[5]);
    printf(" IP          : %d.%d.%d.%d\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf(" Subnet Mask : %d.%d.%d.%d\n", net_info.sn[0], net_info.sn[1], net_info.sn[2], net_info.sn[3]);
    printf(" Gateway     : %d.%d.%d.%d\n", net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);
    printf(" DNS         : %d.%d.%d.%d\n", net_info.dns[0], net_info.dns[1], net_info.dns[2], net_info.dns[3]);
    printf("====================================================================================================\n\n");
}
