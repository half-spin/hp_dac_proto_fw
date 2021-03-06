#include "project.h"
#include "audio/audio_out.h"
#include "usb/usb.h"
#include "sync/sync.h"
#include "comm/comm.h"
#include "knobs/knobs.h"
#include "pre_post_processing/samplemanagement.h"
#include "volume/volume.h"

#define ENABLE_BOOTLOAD (0u)

#define ever    (;;)

#define RX_BUF_SIZE         (256u)
#define RX_TRANSFER_SIZE    (128u)
#define TX_BUF_SIZE         (256u)
#define TX_TRANSFER_SIZE    (128u)

volatile uint8_t mute_toggle = 1;
volatile comm comm_main;

CY_ISR_PROTO(mute_button);
CY_ISR_PROTO(bootload_isr);
CY_ISR_PROTO(txdoneisr);
CY_ISR_PROTO(spyisr);

int main(void)
{
    CyGlobalIntEnable;

    // Enable mute button ISR. Configuring audio will automatically unmute as well.
    mute_isr_StartEx(mute_button);
    mute_Write(mute_toggle);

    // Set up analog inputs
    knobs_start();
    
    // Set up volume control
    volume_start();
    
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
    uint16_t update_interval = 0, range = 0, i = 0;
    int32_t sample = 0;
//    uint8_t error = 0;
//    uint8_t rx_status = 0;
//    uint16_t rx_size = 0, packet_size = 0;

    comm_main = comm_create(uart_config);
    
    UART_Start();
    tx_isr_StartEx(txdoneisr);
    spy_isr_StartEx(spyisr);
    
    rx_spy_start(COMM_DELIM, RX_TRANSFER_SIZE);
    comm_start(comm_main);
    
//    bs_isr_StartEx(bs_done_isr);
    i2s_isr_StartEx(i2s_done_isr);
    audio_out_init(config);
    audio_out_start();
    
    boot_isr_StartEx(bootload_isr);

//    sync_init();
    // USB audio will put incoming data into the audio processing buffer.
    usb_start(audio_out_process, AUDIO_OUT_PROCESS_SIZE);
    
    for ever
    {
        // USB Handler
        if (USBFS_GetConfiguration()) {
            usb_service();
        }
        // Feedback was updated. Send telemetry.
        if (fb_update_flag && audio_out_active) {
            fb_update_flag = 0;
            // Audio out buffer size is modified in an isr,
            // so we'll just grab a quick copy
            int_status = CyEnterCriticalSection();
            log_dat = volume_multiplier;
            CyExitCriticalSection(int_status);
            // Send the data over the UART
            comm_send(comm_main, (uint8_t*)&log_dat, sizeof(log_dat));
        }
        // Process audio
        if (audio_out_update_flag) {
            audio_out_update_flag = 0;
            int_status = CyEnterCriticalSection();
            range = audio_out_count;
            CyExitCriticalSection(int_status);
            
            i = 0;
            while (range > 0) {
                sample = get_audio_sample_from_bytestream(&audio_out_process[i]);
                sample = apply_volume_filter_to_sample(sample);
                return_sample_to_bytestream(sample, &audio_out_process[i]);
                range -= 3;
                i += 3;
            }
            // Put processed bytes into byte swap dma transfer and send it out.
            audio_out_transmit();
        }

        // New update from adc.
        if (knob_status & KNOB_STS_NEW) {
            knob_status &= ~KNOB_STS_NEW;
            //set the volume multiplier based on the new knob value
            // Don't update too frequently.
            if (update_interval++ == 42) {
                update_interval = 0;
                // Use log knob for vol
                set_volume_multiplier(knobs[0]);
            }
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
