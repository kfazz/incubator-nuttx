/****************************************************************************
 * boards/arm/stm32/stm32f429i-disco/src/stm32_highpri.c
 *
 *   Copyright (C) 2018, 2019 Gregory Nutt. All rights reserved.
 *   Author: Mateusz Szafoni <raiden00@railab.me>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/signal.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#include <arch/irq.h>
#include <arch/armv7-m/nvicpri.h>

#include "up_internal.h"
#include "ram_vectors.h"

#include "stm32_pwm.h"
#include "stm32_adc.h"
#include "stm32_dma.h"

#include <arch/board/board.h>
#ifdef CONFIG_STM32F429I_DISCO_HIGHPRI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

#ifndef CONFIG_ARCH_HIPRI_INTERRUPT
#  error CONFIG_ARCH_HIPRI_INTERRUPT is required
#endif

#ifndef CONFIG_ARCH_RAMVECTORS
#  error CONFIG_ARCH_RAMVECTORS is required
#endif

#ifndef CONFIG_ARCH_IRQPRIO
#  error CONFIG_ARCH_IRQPRIO is required
#endif

#ifndef CONFIG_ARCH_FPU
#  warning Set CONFIG_ARCH_FPU for hardware FPU support
#endif

#ifdef CONFIG_STM32_ADC1_DMA
#  if defined(CONFIG_STM32_TIM1_PWM)
#    define HIGHPRI_HAVE_TIM1
#  endif
#  if (CONFIG_STM32_ADC1_DMA_CFG != 1)
#    error ADC1 DMA must be configured in Circular Mode
#  endif
#  if !defined(HIGHPRI_HAVE_TIM1)
#    error "Needs TIM1 to trigger ADC DMA"
#  endif
#endif

#ifdef HIGHPRI_HAVE_TIM1
#  ifndef ADC1_EXTSEL_VALUE
#    error ADC1 EXTSEL have to be configured in board.h
#  endif
#endif

#if (CONFIG_STM32_ADC1_INJECTED_CHAN > 0)
#  if (CONFIG_STM32_ADC1_INJECTED_CHAN > 1)
#    error Max 1 injected channels supported for now
#  else
#    define HIGHPRI_HAVE_INJECTED
#  endif
#endif

#ifdef HIGHPRI_HAVE_INJECTED
#  define INJ_NCHANNELS CONFIG_STM32_ADC1_INJECTED_CHAN
#else
#  define INJ_NCHANNELS (0)
#endif

#ifndef CONFIG_STM32_ADC1_DMA
#  define REG_NCHANNELS (1)
#else
#  define REG_NCHANNELS (1)
#endif

#define ADC1_NCHANNELS  (REG_NCHANNELS + INJ_NCHANNELS)

#define DEV1_PORT       (1)
#define DEV1_NCHANNELS  ADC1_NCHANNELS
#define ADC_REF_VOLTAGE (3.3f)
#define ADC_VAL_MAX     (4095)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* High priority example private data */

struct highpri_s
{
  FAR struct stm32_adc_dev_s *adc1;
#ifdef HIGHPRI_HAVE_TIM1
  struct stm32_pwm_dev_s     *pwm;
#endif
  volatile uint32_t  cntr1;
  volatile uint32_t  cntr2;
  volatile uint8_t   current;
  uint16_t           r_val[REG_NCHANNELS];
  float              r_volt[REG_NCHANNELS];
#ifdef HIGHPRI_HAVE_INJECTED
  uint16_t           j_val[INJ_NCHANNELS];
  float              j_volt[INJ_NCHANNELS];
#endif
  bool               lock;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ADC channel list  */

static const uint8_t g_chanlist1[DEV1_NCHANNELS] =
{
  5,
#if INJ_NCHANNELS > 0
  13,
#endif
};

/* Configurations of pins used by ADC channel */

static const uint32_t g_pinlist1[DEV1_NCHANNELS] =
{
  GPIO_ADC1_IN5,                 /* PA5 */
#if INJ_NCHANNELS > 0
  GPIO_ADC1_IN13,                /* PC3 */
#endif
};

static struct highpri_s g_highpri;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: adc12_handler
 *
 * Description:
 *   This is the handler for the high speed ADC interrupt.
 *
 ****************************************************************************/

#if !defined(CONFIG_STM32_ADC1_DMA) || defined(HIGHPRI_HAVE_INJECTED)
void adc_handler(void)
{
  FAR struct stm32_adc_dev_s *adc = g_highpri.adc1;
  float ref = ADC_REF_VOLTAGE;
  float bit = ADC_VAL_MAX;
  uint32_t pending;
#ifdef HIGHPRI_HAVE_INJECTED
  int i = 0;
#endif

  /* Get pending ADC1 interrupts */

  pending = STM32_ADC_INT_GET(adc);

  if (g_highpri.lock == true)
    {
      goto irq_out;
    }

#ifndef CONFIG_STM32_ADC1_DMA
  /* Regular channel end of conversion */

  if (pending & ADC_ISR_EOC)
    {
      /* Increase regular sequence counter */

      g_highpri.cntr1 += 1;

      /* Get regular data */

      g_highpri.r_val[g_highpri.current] = STM32_ADC_REGDATA_GET(adc);

      /* Do some floating point operations */

      g_highpri.r_volt[g_highpri.current] =
        (float)g_highpri.r_val[g_highpri.current] * ref / bit;

      if (g_highpri.current >= REG_NCHANNELS - 1)
        {
          g_highpri.current = 0;
        }
      else
        {
          g_highpri.current += 1;
        }
    }
#endif

#ifdef HIGHPRI_HAVE_INJECTED
  /* Injected channel end of sequence */

  if (pending & ADC_ISR_JEOC)
    {
      /* Increase injected sequence counter */

      g_highpri.cntr2 += 1;

      /* Get injected channels */

      for (i = 0; i < INJ_NCHANNELS; i += 1)
        {
          g_highpri.j_val[i] = STM32_ADC_INJDATA_GET(adc, i);
        }

      /* Do some floating point operations */

      for (i = 0; i < INJ_NCHANNELS; i += 1)
        {
          g_highpri.j_volt[i] = (float)g_highpri.j_val[i] * ref / bit;
        }
    }
#endif

irq_out:

  /* Clear ADC pending interrupts */

  STM32_ADC_INT_ACK(adc, pending);
}
#endif

/****************************************************************************
 * Name: dma2s0_handler
 *
 * Description:
 *   This is the handler for the high speed ADC interrupt using DMA transfer.
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_ADC1_DMA
void dma2s0_handler(void)
{
  float ref = ADC_REF_VOLTAGE;
  float bit = ADC_VAL_MAX;
  uint8_t pending;
  int i;

  pending = stm32_dma_intget(DMA2, DMA_STREAM0);

  if (g_highpri.lock == true)
    {
      goto irq_out;
    }

  /* Increase regular sequence counter */

  g_highpri.cntr1 += 1;

  for (i = 0; i < REG_NCHANNELS; i += 1)
    {
      /* Do some floating point operations */

      g_highpri.r_volt[i] = (float)g_highpri.r_val[i] * ref / bit;
    }

irq_out:

  /* Clear DMA pending interrupts */

  stm32_dma_intack(DMA2, DMA_STREAM0, pending);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: highpri_main
 *
 * Description:
 *   Main entry point in into the high priority interrupt test.
 *
 ****************************************************************************/

int highpri_main(int argc, char *argv[])
{
#ifdef HIGHPRI_HAVE_TIM1
  struct stm32_pwm_dev_s *pwm1;
#endif
  FAR struct adc_dev_s *adc1;
  FAR struct highpri_s *highpri;
  int ret;
  int i;

  highpri = &g_highpri;

  /* Initialize highpri structure */

  memset(highpri, 0, sizeof(struct highpri_s));

  printf("\nhighpri_main: Started\n");

  /* Configure the pins as analog inputs for the selected channels */

  for (i = 0; i < DEV1_NCHANNELS; i++)
    {
      stm32_configgpio(g_pinlist1[i]);
    }

  /* Initialize ADC driver */

  adc1 = stm32_adcinitialize(DEV1_PORT, g_chanlist1, DEV1_NCHANNELS);
  if (adc1 == NULL)
    {
      aerr("ERROR: Failed to get ADC interface 1\n");
      ret = EXIT_FAILURE;
      goto errout;
    }

  highpri->adc1 = (struct stm32_adc_dev_s *)adc1->ad_priv;

#ifdef HIGHPRI_HAVE_TIM1
  /* Initialize TIM1 */

  pwm1 = (FAR struct stm32_pwm_dev_s *) stm32_pwminitialize(1);
  if (pwm1 == NULL)
    {
      printf("ERROR: Failed to get PWM1 interface\n");
      ret = EXIT_FAILURE;
      goto errout;
    }

  highpri->pwm = pwm1;

  /* Setup PWM device */

  PWM_SETUP(pwm1);

  /* Set timer frequency */

  PWM_FREQ_UPDATE(pwm1, 1000);

#if ADC1_EXTSEL_VALUE == ADC1_EXTSEL_T1CC1
  /* Set CCR1 */

  PWM_CCR_UPDATE(pwm1, 1, 0x0f00);

  /* Enable TIM1 OUT1 */

  PWM_OUTPUTS_ENABLE(pwm1, STM32_PWM_OUT1, true);
#else
#  error T1CC1 only supported for now
#endif

#ifdef CONFIG_DEBUG_PWM_INFO
  /* Print debug */

  PWM_DUMP_REGS(pwm1);
#endif

#endif /* HIGHPRI_HAVE_TIM1 */

#if !defined(CONFIG_STM32_ADC1_DMA) || defined(HIGHPRI_HAVE_INJECTED)
  /* Attach ADC ram vector if no DMA or injected channels support */

  ret = up_ramvec_attach(STM32_IRQ_ADC, adc_handler);
  if (ret < 0)
    {
      fprintf(stderr, "highpri_main: ERROR: up_ramvec_attach failed: %d\n",
              ret);
      ret = EXIT_FAILURE;
      goto errout;
    }

  /* Set the priority of the ADC interrupt vector */

  ret = up_prioritize_irq(STM32_IRQ_ADC, NVIC_SYSH_HIGH_PRIORITY);
  if (ret < 0)
    {
      fprintf(stderr, "highpri_main: ERROR: up_prioritize_irq failed: %d\n",
              ret);
      ret = EXIT_FAILURE;
      goto errout;
    }

  up_enable_irq(STM32_IRQ_ADC);
#endif

#ifdef CONFIG_STM32_ADC1_DMA
  /* Attach DMA2 STREAM0 ram vector if DMA */

  ret = up_ramvec_attach(STM32_IRQ_DMA2S0, dma2s0_handler);
  if (ret < 0)
    {
      fprintf(stderr, "highpri_main: ERROR: up_ramvec_attach failed: %d\n",
              ret);
      ret = EXIT_FAILURE;
      goto errout;
    }

  /* Set the priority of the DMA2 STREAM0 interrupt vector */

  ret = up_prioritize_irq(STM32_IRQ_DMA2S0, NVIC_SYSH_HIGH_PRIORITY);
  if (ret < 0)
    {
      fprintf(stderr, "highpri_main: ERROR: up_prioritize_irq failed: %d\n",
              ret);
      ret = EXIT_FAILURE;
      goto errout;
    }

  up_enable_irq(STM32_IRQ_DMA2S0);
#endif

  /* Setup ADC hardware */

  adc1->ad_ops->ao_setup(adc1);

#ifndef CONFIG_STM32_ADC1_DMA
  /* Enable ADC regular conversion interrupts if no DMA */

  STM32_ADC_INT_ENABLE(highpri->adc1, ADC_IER_EOC);
#else
  /* Note: ADC and DMA must be reset after overrun occurs.
   *       For this example we assume that overrun will not occur.
   *       This is true only if DMA and ADC trigger are properly configured.
   *       DMA configuration must be done before ADC trigger starts!
   */

  /* Register ADC buffer for DMA transfer */

  STM32_ADC_REGBUF_REGISTER(highpri->adc1, g_highpri.r_val, REG_NCHANNELS);
#endif

#ifdef HIGHPRI_HAVE_INJECTED
  /* Enable ADC injected channels end of conversion interrupts */

  STM32_ADC_INT_ENABLE(highpri->adc1, ADC_IER_JEOC);
#endif

#ifdef HIGHPRI_HAVE_TIM1
  /* Enable timer counter after ADC and DMA configuration */

  PWM_TIM_ENABLE(pwm1, true);
#endif

  while (1)
    {
#ifndef CONFIG_STM32_ADC1_DMA
      /* Software trigger for regular sequence */

      adc1->ad_ops->ao_ioctl(adc1, IO_TRIGGER_REG, 0);

      usleep(100);
#endif

#ifdef HIGHPRI_HAVE_INJECTED
      /* Software trigger for injected sequence */

      adc1->ad_ops->ao_ioctl(adc1, IO_TRIGGER_INJ, 0);

      usleep(100);
#endif
      /* Lock global data */

      g_highpri.lock = true;

#ifndef CONFIG_STM32_ADC1_DMA
      printf("%d [%d] %0.3fV\n", g_highpri.cntr1, g_highpri.current,
              g_highpri.r_volt[g_highpri.current]);
#else
      printf("%d ", g_highpri.cntr1);

      for (i = 0; i < REG_NCHANNELS; i += 1)
        {
          printf("r:[%d] %0.3fV, ", i, g_highpri.r_volt[i]);
        }

      printf("\n");
#endif

#ifdef HIGHPRI_HAVE_INJECTED
      /* Print data from injected channels */

      printf("%d ", g_highpri.cntr2);

      for (i = 0; i < INJ_NCHANNELS; i += 1)
        {
          printf("j:[%d] %0.3fV, ", i, g_highpri.j_volt[i]);
        }

      printf("\n");
#endif
      /* Unlock global data */

      g_highpri.lock = false;

      nxsig_sleep(1);
    }

errout:
  return ret;
}

#endif /* CONFIG_STM32F429I_DISCO_HIGHPRI */
