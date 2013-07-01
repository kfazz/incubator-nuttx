/****************************************************************************
 * configs/nuttx/spi/spi_bitbang.c
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
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

#include <nuttx/spi/spi_bitbang.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* Usage ********************************************************************/
/* To use this logic, you should provide a C file that does the following:
 *
 * - Defines SPI_SETSCK and SPI_CLRSCK to set and clear the SCK signal
 * - Defines SPI_SETMOSI and SPI_CLRMOSI to set and clear the MISO signal
 * - Defines SPI_GETMISO to sample the MISO state
 * - Defines SPI_PERBIT_NSEC which is the minimum time to transfer one bit.
 *   This determines the maximum frequency.
 * - Provide implementations of spi_select(), spi_status(), and spi_cmddata().
 * - Then include this file
 * - Provide an initialization function that initializes the GPIO pins used
 *   in the bit bang interface and calls spi_create_bitbang().
 */

/* Debug ********************************************************************/
/* Check if SPI debut is enabled (non-standard.. no support in
 * include/debug.h
 */

#ifndef CONFIG_DEBUG
#  undef CONFIG_DEBUG_VERBOSE
#  undef CONFIG_DEBUG_SPI
#endif

#ifdef CONFIG_DEBUG_SPI
#  define spidbg lldbg
#  ifdef CONFIG_DEBUG_VERBOSE
#    define spivdbg lldbg
#  else
#    define spivdbg(x...)
#  endif
#else
#  define spidbg(x...)
#  define spivdbg(x...)
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void     spi_select(FAR struct spi_bitbang_s *priv,
                  enum spi_dev_e devid, bool selected);
static uint32_t spi_setfrequency(FAR struct spi_bitbang_s *priv,
                  uint32_t frequency);
static void     spi_setmode(FAR struct spi_bitbang_s *priv,
                  enum spi_mode_e mode);
static uint16_t spi_bitexchange0(FAR struct spi_bitbang_s *priv,
                  uint16_t dataout);
static uint16_t spi_bitexchange1(FAR struct spi_bitbang_s *priv,
                  uint16_t dataout);
static uint16_t spi_bitexchange2(FAR struct spi_bitbang_s *priv,
                  uint16_t dataout);
static uint16_t spi_bitexchange3(FAR struct spi_bitbang_s *priv,
                  uint16_t dataout);
static uint16_t spi_exchange(FAR struct spi_bitbang_s *priv,
                  uint16_t dataout);
static uint8_t  spi_status(FAR struct spi_bitbang_s *priv,
                  enum spi_dev_e devid);
#ifdef CONFIG_SPI_CMDDATA
static int      spi_cmddata(FAR struct spi_bitbang_s *priv,
                  enum spi_dev_e devid, bool cmd);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_bitbang_ops_s g_spiops =
{
  spi_select,        /* select */
  spi_setfrequency,  /* setfrequency */
  spi_setmode,       /* setmode */
  spi_exchange,      /* exchange */
  spi_status,        /* status */
#ifdef CONFIG_SPI_CMDDATA
  spi_cmddata,       /* cmddata */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/
 /****************************************************************************
 * Name: spi_setfrequency
 *
 * Description:
 *   Set the SPI frequency.
 *
 * Input Parameters:
 *   dev -       Device-specific state data
 *   frequency - The SPI frequency requested
 *
 * Returned Value:
 *   Returns the actual frequency selected
 *
 ****************************************************************************/

static uint32_t spi_setfrequency(FAR struct spi_bitbang_s *priv, uint32_t frequency)
{
  uint32_t pnsec;

  /* SPI frequency cannot be precisely controlled with a bit-bang interface.
   * Freqency corresponds to delay in toggle the SPI clock line:  Set high,
   * wait, set low, wait, set high, wait, etc.
   *
   * Here we calcalute the half period of the frequency in nanoseconds (i.e.,
   * the amount of time that the clock should remain in the high or low state).
   *
   *   frequency = psec / 1 sec                       psec = full period in seconds
   *   psec      = 1 sec / frequency
   *   pnsec     = 1000000000 nsec / (2 * frequency)  pnsec = full period in nsec
   *
   * As examples:
   * 1) frequency = 400KHz; SPI_PERBIT_NSEC = 100
   *    pnsec     = 2500 - 100 = 2400
   *    holdtime  = ((2401) >> 1) + 500) / 1000 = 1
   * 2) frequency = 20MHz;  SPI_PERBIT_NSEC = 100
   *    pnsec     = 50 - 100 -> 0
   *    holdtime  = ((0) >> 1) + 500) / 1000 = 0
   */

  pnsec = (1000000000ul + (frequency >> 1)) / frequency;

  /* Minus the bit transfer overhead */

  if (pnsec > SPI_PERBIT_NSEC)
    {
      pnsec -= SPI_PERBIT_NSEC;
    }
  else
    {
      pnsec = 0;
    }

  /* The hold time in microseconds is half of this (in microseconds) */

  priv->holdtime =  (((pnsec + 1) >> 1) + 500) / 1000;

  /* Let's do our best to calculate the actual frequency
   *
   * As examples:
   * 1) frequency = 400KHz; SPI_PERBIT_NSEC = 100; holdtime = 1
   *    pnsec     = 2000 * 1 + 100 = 2100
   *    frequency = 476KHz
   * 2) frequency = 20MHz;  SPI_PERBIT_NSEC = 100; holdtime = 0
   *    pnsec     = 2000 * 0 + 100 = 100
   *    frequency = 10MHz
   */

  pnsec = 2000 * priv->holdtime + SPI_PERBIT_NSEC;
  frequency = 1000000000ul / pnsec;
  return frequency;
}

/****************************************************************************
 * Name: spi_setmode
 *
 * Description:
 *   Select the current SPI mode
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   mode - the new SPI mode
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void spi_setmode(FAR struct spi_bitbang_s *priv,
                        enum spi_mode_e mode)
{
  spivdbg("mode=%d\n", mode);

  switch (mode)
    {
    case SPIDEV_MODE0: /* CPOL=0; CPHA=0 */
#ifndef CONFIG_SPI_BITBANG_DISABLEMODE0
      SPI_CLRSCK;      /* Resting level of the clock is low */
      priv->exchange = spi_bitexchange0;
#else
      DEBUGPANIC();
#endif
      break;

    case SPIDEV_MODE1: /* CPOL=0; CPHA=1 */
#ifndef CONFIG_SPI_BITBANG_DISABLEMODE1
      SPI_CLRSCK;      /* Resting level of the clock is low */
      priv->exchange = spi_bitexchange1;
#else
      DEBUGPANIC();
#endif
      break;

    case SPIDEV_MODE2: /* CPOL=1; CPHA=0 */
#ifndef CONFIG_SPI_BITBANG_DISABLEMODE2
      SPI_SETSCK;      /* Resting level of the clock is high */
      priv->exchange = spi_bitexchange2;
#else
      DEBUGPANIC();
#endif
      break;

    case SPIDEV_MODE3: /* CPOL=1; CPHA=1 */
#ifndef CONFIG_SPI_BITBANG_DISABLEMODE3
      SPI_SETSCK;      /* Resting level of the clock is high */
      priv->exchange = spi_bitexchange3;
#else
      DEBUGPANIC();
#endif
      break;

    default:
      DEBUGPANIC();
      break;
    }
}

/****************************************************************************
 * Name: spi_bitexchange0
 *
 * Description:
 *   Exchange one bit in mode 0
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   lock - true: Lock spi bus, false: unlock SPI bus
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifndef CONFIG_SPI_BITBANG_DISABLEMODE0
static uint16_t spi_bitexchange0(FAR struct spi_bitbang_s *priv,
                                 uint16_t dataout)
{
  uint16_t datain;
                                   /* No clock transition before setting MOSI */
  if (dataout != 0)
    {
      SPI_SETMOSI;                /* Set MISO if the bit is set */
    }
  else
    {
      SPI_CLRMOSI;                /* Clear MISO if the bit is not set */
    }

  SPI_SETSCK;                     /* Clock transition before getting MISO */
  datain = (uint16_t)SPI_GETMISO; /* Get bit 0 = MOSI value */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  SPI_CLRSCK;                     /* Return clock to the resting state after getting MISO */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  return datain;
}
#endif

/****************************************************************************
 * Name: spi_bitexchange1
 *
 * Description:
 *   Exchange one bit in mode 1
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   lock - true: Lock spi bus, false: unlock SPI bus
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifndef CONFIG_SPI_BITBANG_DISABLEMODE1
static uint16_t spi_bitexchange1(FAR struct spi_bitbang_s *priv,
                                uint16_t dataout)
{
  uint16_t datain;

  SPI_SETSCK;                     /* Clock transition before setting MOSI */
  if (dataout != 0)
    {
      SPI_SETMOSI;                /* Set MISO if the bit is set */
    }
  else
    {
      SPI_CLRMOSI;                /* Clear MISO if the bit is not set */
    }

  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  SPI_CLRSCK;                     /* Clock transition before getting MISO */
  datain = (uint16_t)SPI_GETMISO; /* Get bit 0 = MOSI value */
                                  /* Clock is in resting state after getting MISO */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  return datain;
}
#endif

/****************************************************************************
 * Name: spi_bitexchange2
 *
 * Description:
 *   Exchange one bit in mode 2
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   lock - true: Lock spi bus, false: unlock SPI bus
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifndef CONFIG_SPI_BITBANG_DISABLEMODE2
static uint16_t spi_bitexchange2(FAR struct spi_bitbang_s *priv,
                                 uint16_t dataout)
{
  uint16_t datain;
                                  /* No clock transition before setting MOSI */
  if (dataout != 0)
    {
      SPI_SETMOSI;                /* Set MISO if the bit is set */
    }
  else
    {
      SPI_CLRMOSI;                /* Clear MISO if the bit is not set */
    }

  SPI_CLRSCK;                     /* Clock transition before getting MISO */
  datain = (uint16_t)SPI_GETMISO; /* Get bit 0 = MOSI value */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  SPI_SETSCK;                     /* Return clock to the resting state after getting MISO */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  return datain;
}
#endif

/****************************************************************************
 * Name: spi_bitexchange3
 *
 * Description:
 *   Exchange one bit in mode 3
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   lock - true: Lock spi bus, false: unlock SPI bus
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifndef CONFIG_SPI_BITBANG_DISABLEMODE3
static uint16_t spi_bitexchange3(FAR struct spi_bitbang_s *priv,
                                 uint16_t dataout)
{
  uint16_t datain;

  SPI_CLRSCK;                     /* Clock transition before setting MOSI */
  if (dataout != 0)
    {
      SPI_SETMOSI;                /* Set MISO if the bit is set */
    }
  else
    {
      SPI_CLRMOSI;                /* Clear MISO if the bit is not set */
    }

  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  SPI_SETSCK;                     /* Clock transition before getting MISO */
  datain = (uint16_t)SPI_GETMISO; /* Get bit 0 = MOSI value */
                                  /* Clock is in resting state after getting MISO */
  if (priv->holdtime)
    {
      up_udelay(priv->holdtime);
    }

  return datain;
}
#endif

/****************************************************************************
 * Name: spi_exchange
 *
 * Description:
 *   Exahange on word of data on SPI
 *
 * Input Parameters:
 *   priv     - Device-specific state data
 *   data     - The TX data to be exchanged with the slave
 *
 * Returned Value:
 *   The RX data received from the slave
 *
 ****************************************************************************/

#ifdef CONFIG_SPI_BITBANG_VARWIDTH
static uint16_t spi_exchange(FAR struct spi_bitbang_s *priv, uint16_t dataout)
{
  uint16_t datain;
  uint16_t bit;
  int shift;

  /* Transfer each bit.  This might be better done with straight-line
   * logic because the loop overhead will limit our maximum transfer
   * rate.
   */

  shift =  priv->nbits - 1
  for (bit = 1 << shift; bit != 0; bit >>= 1)
    {
      /* Shift to make space for the next, less significant bit.
       * Then exchange bits with the slave an OR in the new, returned
       * bit.
       */

      datain <<= 1;
      datain |= priv->exchange(priv, dataout & bit);
    }

  return datain;
}

#else
static uint16_t spi_exchange(FAR struct spi_bitbang_s *priv, uint16_t dataout)
{
  uint8_t datain;

  /* Transfer each bit.  This is better done with straight-line logic
   * when possible because the loop overhead will limit our maximum transfer
   * rate.
   */

  /* Exchange bit 7 with the slave */

  datain = priv->exchange(priv, dataout & (1 << 7));

  /* Exchange bit 6 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 6));

  /* Exchange bit 5 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 5));

  /* Exchange bit 4 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 4));

  /* Exchange bit 3 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 3));

  /* Exchange bit 2 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 2));

  /* Exchange bit 1 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 1));

  /* Exchange bit 0 with the slave */

  datain <<= 1;
  datain |= priv->exchange(priv, dataout & (1 << 0));

  return datain;
}
#endif
