#include "project.h"
#include "audio/audio_out.h"
#include "usb/usb.h"
#include "sync/sync.h"
#include "comm/comm.h"

#define ENABLE_BOOTLOAD (0u)

#define ever    (;;)

#define RX_BUF_SIZE         (1024u)
#define RX_TRANSFER_SIZE    (255u)
#define TX_BUF_SIZE         (1024u)
#define TX_TRANSFER_SIZE    (255u)

volatile uint8_t mute_toggle = 1;
volatile comm comm_main;
volatile uint8_t adc_channel = 0;
volatile uint8_t adc_done_flag = 0;

CY_ISR_PROTO(mute_button);
CY_ISR_PROTO(bootload_isr);
CY_ISR_PROTO(txdoneisr);
CY_ISR_PROTO(spyisr);

CY_ISR_PROTO(adcdone);

int main(void)
{
    CyGlobalIntEnable;
    // ADC Setup
    uint16_t adc_buf[3];
    uint8_t adc_dma_ch = DMA_ADC_DmaInitialize(2u, 1u, HI16(CYDEV_PERIPH_BASE), HI16(CYDEV_SRAM_BASE));
    uint8_t adc_dma_td[1];
    
    adc_dma_td[0] = CyDmaTdAllocate();
    CyDmaTdSetConfiguration(adc_dma_td[0], 6u, adc_dma_td[0], TD_INC_DST_ADR);
    CyDmaTdSetAddress(adc_dma_td[0], LO16((uint32_t)ADC_DEC_SAMP_PTR), LO16((uint32_t)&adc_buf[0]));
    CyDmaChSetInitialTd(adc_dma_ch, adc_dma_td[0]);
    CyDmaChEnable(adc_dma_ch, 1u);
    VDAC_pot_Start();
    Opamp_pot_Start();
    AMuxSeq_Start();
    AMuxSeq_Next();
    adc_isr_StartEx(adcdone);
    ADC_Start();
    ADC_StartConvert();
    
    // Enable mute button ISR. Configuring audio will automatically unmute as well.
    mute_isr_StartEx(mute_button);
    mute_Write(mute_toggle);
    
    // DMA Channels for audio out process.
    uint8_t usb_dma_ch, bs_dma_ch, i2s_dma_ch;
    usb_dma_ch = DMA_USB_DmaInitialize(1u, 1u, HI16(CYDEV_SRAM_BASE), HI16(CYDEV_PERIPH_BASE));
    bs_dma_ch = DMA_BS_DmaInitialize(1u, 1u, HI16(CYDEV_PERIPH_BASE), HI16(CYDEV_SRAM_BASE));
    i2s_dma_ch = DMA_I2S_DmaInitialize(1u, 1u, HI16(CYDEV_SRAM_BASE), HI16(CYDEV_PERIPH_BASE));
    
    // Buffer, dma, and register setup constants.
    audio_out_config config = {
        .usb_dma_ch = usb_dma_ch,
        .usb_dma_termout_en = DMA_USB__TD_TERMOUT_EN,
        .bs_dma_ch = bs_dma_ch,
        .bs_dma_termout_en = DMA_BS__TD_TERMOUT_EN,
        .i2s_dma_ch = i2s_dma_ch,
        .i2s_dma_termout_en = DMA_I2S__TD_TERMOUT_EN,
        .usb_buf = usb_out_buf,
        .bs_fifo_in = byte_swap_fifo_in_ptr,
        .bs_fifo_out = byte_swap_fifo_out_ptr
    };
    
    // buffers used by comm library
    uint8_t rx_buf[RX_BUF_SIZE];
    uint8_t tx_buf[TX_BUF_SIZE];

    // Comm dma initialization.
    uint8_t uart_tx_dma_ch = DMATxUart_DmaInitialize(1u, 1u, HI16(CYDEV_SRAM_BASE), HI16(CYDEV_PERIPH_BASE));
    uint8_t uart_rx_dma_ch = DMARxUART_DmaInitialize(1u, 1u, HI16(CYDEV_PERIPH_BASE), HI16(CYDEV_PERIPH_BASE));
    uint8_t spy_dma_ch = DMASpy_DmaInitialize(1u, 1u, HI16(CYDEV_PERIPH_BASE), HI16(CYDEV_SRAM_BASE));
    // Comm configuration settings.
    comm_config uart_config = {
        .uart_tx_ch = uart_tx_dma_ch,
        .uart_tx_n_td = COMM_MAX_TX_TD,
        .uart_tx_td_termout_en = DMATxUart__TD_TERMOUT_EN,
        .uart_tx_fifo = UART_TXDATA_PTR,
        .tx_buffer = tx_buf,
        .tx_capacity = TX_BUF_SIZE,
        .tx_transfer_size = TX_TRANSFER_SIZE,
        .uart_rx_ch = uart_rx_dma_ch,
        .uart_rx_fifo = UART_RXDATA_PTR,
        .spy_ch = spy_dma_ch,
        .spy_n_td = COMM_MAX_RX_TD,
        .rx_buffer = rx_buf,
        .rx_capacity = RX_BUF_SIZE,
        .rx_transfer_size = RX_TRANSFER_SIZE,
        .spy_service = rx_spy_service,
        .spy_resume = rx_spy_resume,
        .spy_fifo_in = rx_spy_fifo_in_ptr,
        .spy_fifo_out = rx_spy_fifo_out_ptr
    };
    
    uint8_t int_status;
    uint16_t log_dat;
    uint16_t read_ptr = 0;
//    uint8_t error = 0;
//    uint8_t rx_status = 0;
//    uint16_t rx_size = 0, packet_size = 0;

    comm_main = comm_create(uart_config);
    
    UART_Start();
    tx_isr_StartEx(txdoneisr);
    spy_isr_StartEx(spyisr);
    
    rx_spy_start(COMM_DELIM, RX_TRANSFER_SIZE);
    comm_start(comm_main);
    
    bs_isr_StartEx(bs_done_isr);
    i2s_isr_StartEx(i2s_done_isr);
    audio_out_init(config);
    audio_out_start();
    
    boot_isr_StartEx(bootload_isr);

//    sync_init();
    usb_start();
    
    for ever
    {
        // USB Handler
        if (USBFS_GetConfiguration()) {
            usb_service();
        }
        // Feedback was updated. Send telemetry.
//        if (fb_update_flag && audio_out_active) {
//            fb_update_flag = 0;
//            // Audio out buffer size is modified in an isr,
//            // so we'll just grab a quick copy
//            int_status = CyEnterCriticalSection();
//            log_dat = audio_out_buffer_size;
//            CyExitCriticalSection(int_status);
//            // Send the data over the UART
//            comm_send(comm_main, (uint8_t*)&log_dat, sizeof(log_dat));
//        }
        /* Byte swap transfer finished. Ready to process data.
         * audio_out_shadow is how many bytes need processing.
         * When you remove data update the read ptr and shadow
         * to account for processed bytes.
         * Whatever operation you perform must finish processing the data
         * within 1ms or we'll have an overrun issue.
         */
        if (audio_out_update_flag) {
            audio_out_update_flag = 0;
            // Like this.
            read_ptr += audio_out_shadow;
            if (read_ptr >= AUDIO_OUT_BUF_SIZE) {
                read_ptr = read_ptr - AUDIO_OUT_BUF_SIZE;
            }
        }
        if (adc_done_flag && adc_channel == 0) {
            adc_done_flag = 0;
            log_dat = adc_buf[2];
            comm_send(comm_main, (uint8_t*)&log_dat, sizeof(log_dat));
        }
    }
}

// Toggle the mute state whenever it is pushed.
CY_ISR(mute_button)
{
    mute_Write(mute_toggle ^= 1);
}

CY_ISR(bootload_isr)
{
    #if ENABLE_BOOTLOAD
    Bootloadable_Load();
    #endif
}

// Packet finished transmitting isr.
CY_ISR(txdoneisr)
{
    comm_tx_isr(comm_main);
}

// Delim, complete transfer, or flush isr.
CY_ISR(spyisr)
{   
    comm_rx_isr(comm_main);
}

CY_ISR(adcdone)
{
    AMuxSeq_Next();
    adc_done_flag = 1;
    adc_channel++;
    adc_channel = adc_channel == 3 ? 0u : adc_channel;
    ADC_StartConvert();
}
