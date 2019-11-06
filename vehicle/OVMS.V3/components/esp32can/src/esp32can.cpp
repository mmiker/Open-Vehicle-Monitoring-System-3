/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
;
; Portions of this are based on the work of Thomas Barth, and licensed
; under MIT license.
; Copyright (c) 2017, Thomas Barth, barth-dev.de
; https://github.com/ThomasBarth/ESP32-CAN-Driver
*/

#include "ovms_log.h"
static const char *TAG = "esp32can";

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include "esp32can.h"
#include "esp32can_regdef.h"
#include "ovms_peripherals.h"

esp32can* MyESP32can = NULL;

static portMUX_TYPE esp32can_spinlock = portMUX_INITIALIZER_UNLOCKED;
#define ESP32CAN_ENTER_CRITICAL()       portENTER_CRITICAL(&esp32can_spinlock)
#define ESP32CAN_EXIT_CRITICAL()        portEXIT_CRITICAL(&esp32can_spinlock)
#define ESP32CAN_ENTER_CRITICAL_ISR()   portENTER_CRITICAL_ISR(&esp32can_spinlock)
#define ESP32CAN_EXIT_CRITICAL_ISR()    portEXIT_CRITICAL_ISR(&esp32can_spinlock)

static inline uint32_t ESP32CAN_rxframe(esp32can *me, uint32_t interrupt, BaseType_t* task_woken)
  {
  static CAN_queue_msg_t msg;
  uint32_t current_ir = interrupt;

  // Note: behaviour of the ESP32 CAN controller differs from SJA1000 here.
  // From https://github.com/espressif/esp-idf/issues/4276#issuecomment-548753085:
  //  
  //  When bytes are received, they are written to the FIFO directly, and an
  //  overflow is not detected until the 64th byte is written. The bytes of
  //  the overflowing message will remain in the FIFO. The RMC should count
  //  all messages received (up to 64 messages) regardless of whether they
  //  were overflowing or not.
  //  
  //  Whenever you release the receiver buffer, and the buffer window shifts
  //  to an overflowed message, the Data overrun interrupt will be set.
  //  If that is the case, the message contents should be ignored, the
  //  clear data overrun command set, and the receiver buffer released again.
  //  Continue this process until RMC reaches zero, or until the buffer
  //  window rotates to a valid message.
  // 
  // Additional note:
  //  We really need to check the IR flags. RBS/DOS and RMC do not reflect
  //  "buffer has valid message", so deliver false duplicates after the
  //  overflow. Only RI reliably indicates validity.

  while (current_ir & (__CAN_IRQ_RX|__CAN_IRQ_DATA_OVERRUN))
    {
    // Invalid message from FIFO overrun?
    if (current_ir & __CAN_IRQ_DATA_OVERRUN)
      {
      // Clear data overrun, get next frame if any:
      MODULE_ESP32CAN->CMR.B.CDO = 1;
      MODULE_ESP32CAN->CMR.B.RRB = 1;
      // Count and remember error event for main handler:
      me->m_status.rxbuf_overflow++;
      }
    else
      {
      // Record the origin
      memset(&msg,0,sizeof(msg));
      msg.type = CAN_frame;
      msg.body.frame.origin = me;

      // get FIR
      msg.body.frame.FIR.U = MODULE_ESP32CAN->MBX_CTRL.FCTRL.FIR.U;

      // check if this is a standard or extended CAN frame
      if (msg.body.frame.FIR.B.FF == CAN_frame_std)
        {
        // Standard frame: Get Message ID
        msg.body.frame.MsgID = ESP32CAN_GET_STD_ID;
        // …deep copy data bytes
        for (int k=0 ; k<msg.body.frame.FIR.B.DLC ; k++)
          msg.body.frame.data.u8[k] = MODULE_ESP32CAN->MBX_CTRL.FCTRL.TX_RX.STD.data[k];
        }
      else
        {
        // Extended frame: Get Message ID
        msg.body.frame.MsgID = ESP32CAN_GET_EXT_ID;
        // …deep copy data bytes
        for (int k=0 ; k<msg.body.frame.FIR.B.DLC ; k++)
          msg.body.frame.data.u8[k] = MODULE_ESP32CAN->MBX_CTRL.FCTRL.TX_RX.EXT.data[k];
        }

      // Let the hardware know the frame has been read
      MODULE_ESP32CAN->CMR.B.RRB = 1;

      // Send frame to main CAN processor task
      xQueueSendFromISR(MyCan.m_rxqueue, &msg, task_woken);
      }

    current_ir = MODULE_ESP32CAN->IR.U & 0xff;
    interrupt |= current_ir;
    } // while (current_ir & __CAN_IRQ_RX)

  return interrupt;
  }

static IRAM_ATTR void ESP32CAN_isr(void *pvParameters)
  {
  esp32can *me = (esp32can*)pvParameters;
  BaseType_t task_woken = pdFALSE;
  uint32_t interrupt;

  ESP32CAN_ENTER_CRITICAL_ISR();

  me->m_status.interrupts++;

  // Read interrupt status and clear flags
  while ((interrupt = MODULE_ESP32CAN->IR.U & 0xff) != 0)
    {
    // Handle RX frame(s) available interrupt:
    if ((interrupt & __CAN_IRQ_RX) != 0)
      {
      interrupt = ESP32CAN_rxframe(me, interrupt, &task_woken);
      }

    // Handle TX complete interrupt:
    if ((interrupt & __CAN_IRQ_TX) != 0)
      {
      // Request TxCallback:
      CAN_queue_msg_t msg;
      msg.type = CAN_txcallback;
      msg.body.bus = me;
      msg.body.frame = me->tx_frame;
      xQueueSendFromISR(MyCan.m_rxqueue, &msg, &task_woken);
      }

    // Handle error interrupts:
    uint8_t error_irqs = interrupt &
        (__CAN_IRQ_ERR						//0x4
        |__CAN_IRQ_DATA_OVERRUN	  //0x8
        |__CAN_IRQ_ERR_PASSIVE		//0x20
        |__CAN_IRQ_ARB_LOST			  //0x40
        |__CAN_IRQ_BUS_ERR				//0x80
        );
    if (error_irqs)
      {
      // Record error data:
      me->m_status.error_flags = error_irqs << 16 | (MODULE_ESP32CAN->SR.U & 0b11001111) << 8 | MODULE_ESP32CAN->ECC.B.ECC;
      me->m_status.errors_rx = MODULE_ESP32CAN->RXERR.U;
      me->m_status.errors_tx = MODULE_ESP32CAN->TXERR.U;
      // Request error log:
      CAN_queue_msg_t msg;
      msg.type = CAN_logerror;
      msg.body.bus = me;
      xQueueSendFromISR(MyCan.m_rxqueue, &msg, &task_woken);
      }

    // Handle wakeup interrupt:
    if ((interrupt & __CAN_IRQ_WAKEUP) != 0)
      {
      // Todo
      }
    }

  ESP32CAN_EXIT_CRITICAL_ISR();

  // Yield to minimize latency if we have woken up a higher priority task:
  if (task_woken == pdTRUE)
    {
    portYIELD_FROM_ISR();
    }
  }

static void ESP32CAN_init(void *pvParameters)
  {
  esp32can *me = (esp32can*) pvParameters;
  // Install CAN ISR:
  esp_intr_alloc(ETS_CAN_INTR_SOURCE, ESP_INTR_FLAG_IRAM|ESP_INTR_FLAG_LEVEL3, ESP32CAN_isr, me, NULL);
  vTaskDelete(NULL);
  }

esp32can::esp32can(const char* name, int txpin, int rxpin)
  : canbus(name)
  {
  m_txpin = (gpio_num_t)txpin;
  m_rxpin = (gpio_num_t)rxpin;
  MyESP32can = this;

  // Due to startup order, we can't talk to MAX7317 during
  // initialisation. So, we'll just enter reset mode for the
  // on-chip controller, and let housekeeping power us down
  // after startup.
  m_powermode = Off;
  MODULE_ESP32CAN->MOD.B.RM = 1;

  // Launch ISR allocator task on core 0:
  xTaskCreatePinnedToCore(ESP32CAN_init, "esp32can init", 2048, (void*)this, 23, NULL, CORE(0));
  }

esp32can::~esp32can()
  {
  MyESP32can = NULL;
  }

esp_err_t esp32can::Start(CAN_mode_t mode, CAN_speed_t speed)
  {
  switch (speed)
    {
    case CAN_SPEED_33KBPS:
    case CAN_SPEED_83KBPS:
      /* XXX not yet */
      ESP_LOGW(TAG,"%d not supported",speed);
      return ESP_FAIL;
    default:
      break;
    }

  canbus::Start(mode, speed);

  double __tq; // Time quantum

  m_mode = mode;
  m_speed = speed;

#ifdef CONFIG_OVMS_COMP_MAX7317
  // Power up the matching SN65 transciever
  MyPeripherals->m_max7317->Output(MAX7317_CAN1_EN, 0);
#endif // #ifdef CONFIG_OVMS_COMP_MAX7317

  // Enable module
  DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_CAN_CLK_EN);
  DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_CAN_RST);

  // Configure TX pin
  gpio_set_level(MyESP32can->m_txpin, 1);
  gpio_set_direction(MyESP32can->m_txpin,GPIO_MODE_OUTPUT);
  gpio_matrix_out(MyESP32can->m_txpin,CAN_TX_IDX,0,0);
  gpio_pad_select_gpio(MyESP32can->m_txpin);

  // Configure RX pin
  gpio_set_direction(MyESP32can->m_rxpin,GPIO_MODE_INPUT);
  gpio_matrix_in(MyESP32can->m_rxpin,CAN_RX_IDX,0);
  gpio_pad_select_gpio(MyESP32can->m_rxpin);

  ESP32CAN_ENTER_CRITICAL();

  // Set to PELICAN mode
  MODULE_ESP32CAN->CDR.B.CAN_M=0x1;

  // Synchronization jump width is the same for all baud rates
  MODULE_ESP32CAN->BTR0.B.SJW=0x1;

  // TSEG2 is the same for all baud rates
  MODULE_ESP32CAN->BTR1.B.TSEG2=0x1;

  // select time quantum and set TSEG1
  switch (MyESP32can->m_speed)
    {
    case CAN_SPEED_1000KBPS:
      MODULE_ESP32CAN->BTR1.B.TSEG1=0x4;
      __tq = 0.125;
      break;
    default:
      MODULE_ESP32CAN->BTR1.B.TSEG1=0xc;
      __tq = ((float)1000/MyESP32can->m_speed) / 16;
    }

  // Set baud rate prescaler
  MODULE_ESP32CAN->BTR0.B.BRP=(uint8_t)round((((APB_CLK_FREQ * __tq) / 2) - 1)/1000000)-1;

  /* Set sampling
   * 1 -> triple; the bus is sampled three times; recommended for low/medium speed buses     (class A and B) where filtering spikes on the bus line is beneficial
   * 0 -> single; the bus is sampled once; recommended for high speed buses (SAE class C)*/
  MODULE_ESP32CAN->BTR1.B.SAM=0x1;

  // Enable all interrupts
  MODULE_ESP32CAN->IER.U = 0xff;

  // No acceptance filtering, as we want to fetch all messages
  MODULE_ESP32CAN->MBX_CTRL.ACC.CODE[0] = 0;
  MODULE_ESP32CAN->MBX_CTRL.ACC.CODE[1] = 0;
  MODULE_ESP32CAN->MBX_CTRL.ACC.CODE[2] = 0;
  MODULE_ESP32CAN->MBX_CTRL.ACC.CODE[3] = 0;
  MODULE_ESP32CAN->MBX_CTRL.ACC.MASK[0] = 0xff;
  MODULE_ESP32CAN->MBX_CTRL.ACC.MASK[1] = 0xff;
  MODULE_ESP32CAN->MBX_CTRL.ACC.MASK[2] = 0xff;
  MODULE_ESP32CAN->MBX_CTRL.ACC.MASK[3] = 0xff;

  // Set to normal mode
  MODULE_ESP32CAN->OCR.B.OCMODE=__CAN_OC_NOM;

  // Active/Listen Mode
  if (m_mode == CAN_MODE_LISTEN)
    MODULE_ESP32CAN->MOD.B.LOM = 1;
  else
    MODULE_ESP32CAN->MOD.B.LOM = 0;

  // Clear error counters
  MODULE_ESP32CAN->TXERR.U = 0;
  MODULE_ESP32CAN->RXERR.U = 0;
  (void)MODULE_ESP32CAN->ECC;

  // Clear interrupt flags
  (void)MODULE_ESP32CAN->IR.U;

  // clear statistics:
  ClearStatus();

  // Showtime. Release Reset Mode.
  MODULE_ESP32CAN->MOD.B.RM = 0;

  ESP32CAN_EXIT_CRITICAL();

  // And record that we are powered on
  pcp::SetPowerMode(On);

  return ESP_OK;
  }

esp_err_t esp32can::Stop()
  {
  canbus::Stop();

  ESP32CAN_ENTER_CRITICAL();

  // Enter reset mode
  MODULE_ESP32CAN->MOD.B.RM = 1;

  ESP32CAN_EXIT_CRITICAL();

#ifdef CONFIG_OVMS_COMP_MAX7317
  // Power down the matching SN65 transciever
  MyPeripherals->m_max7317->Output(MAX7317_CAN1_EN, 1);
#endif // #ifdef CONFIG_OVMS_COMP_MAX7317

  // And record that we are powered down
  pcp::SetPowerMode(Off);

  return ESP_OK;
  }

esp_err_t esp32can::Write(const CAN_frame_t* p_frame, TickType_t maxqueuewait /*=0*/)
  {
  uint8_t __byte_i; // Byte iterator

  if (m_mode != CAN_MODE_ACTIVE)
    {
    ESP_LOGW(TAG,"Cannot write %s when not in ACTIVE mode",m_name);
    return ESP_OK;
    }

  ESP32CAN_ENTER_CRITICAL();

  // check if TX buffer is available:
  if(MODULE_ESP32CAN->SR.B.TBS == 0)
    {
    ESP32CAN_EXIT_CRITICAL();
    return QueueWrite(p_frame, maxqueuewait);
    }

  // copy frame information record
  MODULE_ESP32CAN->MBX_CTRL.FCTRL.FIR.U=p_frame->FIR.U;

  if (p_frame->FIR.B.FF==CAN_frame_std)
    { // Standard frame
    // Write message ID
    ESP32CAN_SET_STD_ID(p_frame->MsgID);
    // Copy the frame data to the hardware
    for (__byte_i=0 ; __byte_i<p_frame->FIR.B.DLC ; __byte_i++)
      MODULE_ESP32CAN->MBX_CTRL.FCTRL.TX_RX.STD.data[__byte_i]=p_frame->data.u8[__byte_i];
    }
  else
    { // Extended frame
    // Write message ID
    ESP32CAN_SET_EXT_ID(p_frame->MsgID);
    // Copy the frame data to the hardware
    for (__byte_i=0 ; __byte_i<p_frame->FIR.B.DLC ; __byte_i++)
      MODULE_ESP32CAN->MBX_CTRL.FCTRL.TX_RX.EXT.data[__byte_i]=p_frame->data.u8[__byte_i];
    }

  // Transmit frame
  MODULE_ESP32CAN->CMR.B.TR=1;

  ESP32CAN_EXIT_CRITICAL();

  // stats & logging:
  canbus::Write(p_frame, maxqueuewait);

  return ESP_OK;
  }

void esp32can::TxCallback(CAN_frame_t* p_frame, bool success)
  {
  canbus::TxCallback(p_frame, success);

  // TX buffer has become available; send next queued frame (if any):
  CAN_frame_t frame;
  if (xQueueReceive(m_txqueue, (void*)&frame, 0) == pdTRUE)
    Write(&frame, 0);
  }

void esp32can::SetPowerMode(PowerMode powermode)
  {
  pcp::SetPowerMode(powermode);
  switch (powermode)
    {
    case On:
      if (m_mode != CAN_MODE_OFF) Start(m_mode,m_speed);
      break;
    case Sleep:
    case DeepSleep:
    case Off:
      Stop();
      break;
    default:
      break;
    };
  }
