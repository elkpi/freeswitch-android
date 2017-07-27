/*
 * SpanDSP - a series of DSP components for telephony
 *
 * async_tests.c - Tests for asynchronous serial processing.
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2004 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*! \file */

/*! \page async_tests_page Asynchronous bit stream tests
\section async_tests_page_sec_1 What does it do?
*/

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sndfile.h>

#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES

#include "spandsp.h"

async_rx_state_t rx_async;
async_tx_state_t tx_async;

int full_len;
uint8_t old_buf[1000];
uint8_t new_buf[1000];

volatile int tx_async_chars;
volatile int rx_async_chars;
volatile int rx_async_char_mask;

int v14_test_async_tx_get_bit(void *user_data);

int v14_test_async_tx_get_bit(void *user_data)
{
    async_tx_state_t *s;
    int bit;
    int parity_bit;
    static int destuff = 0;

    /* Special routine to test V.14 rate adaption, by randomly skipping
       stop bits. */
    s = (async_tx_state_t *) user_data;
    if (s->bitpos == 0)
    {
        s->byte_in_progress = s->get_byte(s->user_data);
        s->byte_in_progress &= (0xFFFF >> (16 - s->data_bits));
        if (s->parity)
        {
            parity_bit = parity8(s->byte_in_progress);
            if (s->parity == ASYNC_PARITY_ODD)
                parity_bit ^= 1;
            s->byte_in_progress |= (parity_bit << s->data_bits);
            s->byte_in_progress |= (0xFFFF << (s->data_bits + 1));
        }
        else
        {
            s->byte_in_progress |= (0xFFFF << s->data_bits);
        }
        /* Start bit */
        bit = 0;
        s->bitpos++;
    }
    else
    {
        bit = s->byte_in_progress & 1;
        s->byte_in_progress >>= 1;
        /* Drop the stop bit on every fourth character for V.14 simulation */
        if ((++destuff & 3) == 0)
        {
            if (++s->bitpos > s->total_bits - 1)
                s->bitpos = 0;
        }
        else
        {
            if (++s->bitpos > s->total_bits)
                s->bitpos = 0;
        }
    }
    return bit;
}
/*- End of function --------------------------------------------------------*/

static int test_get_async_byte(void *user_data)
{
    int byte;

    byte = tx_async_chars & 0xFF;
    tx_async_chars++;
    return byte;
}
/*- End of function --------------------------------------------------------*/

static void test_put_async_byte(void *user_data, int byte)
{
    if ((rx_async_chars & rx_async_char_mask) != byte)
        printf("Received byte is 0x%X (expected 0x%X)\n", byte, rx_async_chars);
    rx_async_chars++;
}
/*- End of function --------------------------------------------------------*/

int main(int argc, char *argv[])
{
    int bit;

    printf("Test with async 8N1\n");
    async_tx_init(&tx_async, 8, ASYNC_PARITY_NONE, 1, false, test_get_async_byte, NULL);
    async_rx_init(&rx_async, 8, ASYNC_PARITY_NONE, 1, false, test_put_async_byte, NULL);
    tx_async_chars = 0;
    rx_async_chars = 0;
    rx_async_char_mask = 0xFF;
    while (rx_async_chars < 1000)
    {
        bit = async_tx_get_bit(&tx_async);
        async_rx_put_bit(&rx_async, bit);
    }
    printf("Chars=%d/%d, PE=%d, FE=%d\n", tx_async_chars, rx_async_chars, rx_async.parity_errors, rx_async.framing_errors);
    if (tx_async_chars != rx_async_chars
        ||
        rx_async.parity_errors
        ||
        rx_async.framing_errors)
    {
        printf("Test failed.\n");
        exit(2);
    }

    printf("Test with async 7E1\n");
    async_tx_init(&tx_async, 7, ASYNC_PARITY_EVEN, 1, false, test_get_async_byte, NULL);
    async_rx_init(&rx_async, 7, ASYNC_PARITY_EVEN, 1, false, test_put_async_byte, NULL);
    tx_async_chars = 0;
    rx_async_chars = 0;
    rx_async_char_mask = 0x7F;
    while (rx_async_chars < 1000)
    {
        bit = async_tx_get_bit(&tx_async);
        async_rx_put_bit(&rx_async, bit);
    }
    printf("Chars=%d/%d, PE=%d, FE=%d\n", tx_async_chars, rx_async_chars, rx_async.parity_errors, rx_async.framing_errors);
    if (tx_async_chars != rx_async_chars
        ||
        rx_async.parity_errors
        ||
        rx_async.framing_errors)
    {
        printf("Test failed.\n");
        exit(2);
    }

    printf("Test with async 8O1\n");
    async_tx_init(&tx_async, 8, ASYNC_PARITY_ODD, 1, false, test_get_async_byte, NULL);
    async_rx_init(&rx_async, 8, ASYNC_PARITY_ODD, 1, false, test_put_async_byte, NULL);
    tx_async_chars = 0;
    rx_async_chars = 0;
    rx_async_char_mask = 0xFF;
    while (rx_async_chars < 1000)
    {
        bit = async_tx_get_bit(&tx_async);
        async_rx_put_bit(&rx_async, bit);
    }
    printf("Chars=%d/%d, PE=%d, FE=%d\n", tx_async_chars, rx_async_chars, rx_async.parity_errors, rx_async.framing_errors);
    if (tx_async_chars != rx_async_chars
        ||
        rx_async.parity_errors
        ||
        rx_async.framing_errors)
    {
        printf("Test failed.\n");
        exit(2);
    }

    printf("Test with async 8O1 and V.14\n");
    async_tx_init(&tx_async, 8, ASYNC_PARITY_ODD, 1, true, test_get_async_byte, NULL);
    async_rx_init(&rx_async, 8, ASYNC_PARITY_ODD, 1, true, test_put_async_byte, NULL);
    tx_async_chars = 0;
    rx_async_chars = 0;
    rx_async_char_mask = 0xFF;
    while (rx_async_chars < 1000)
    {
        bit = v14_test_async_tx_get_bit(&tx_async);
        async_rx_put_bit(&rx_async, bit);
    }
    printf("Chars=%d/%d, PE=%d, FE=%d\n", tx_async_chars, rx_async_chars, rx_async.parity_errors, rx_async.framing_errors);
    if (tx_async_chars != rx_async_chars
        ||
        rx_async.parity_errors
        ||
        rx_async.framing_errors)
    {
        printf("Test failed.\n");
        exit(2);
    }

    printf("Test with async 5N2\n");
    async_tx_init(&tx_async, 5, ASYNC_PARITY_NONE, 2, false, test_get_async_byte, NULL);
    async_rx_init(&rx_async, 5, ASYNC_PARITY_NONE, 2, false, test_put_async_byte, NULL);
    tx_async_chars = 0;
    rx_async_chars = 0;
    rx_async_char_mask = 0x1F;
    while (rx_async_chars < 1000)
    {
        bit = async_tx_get_bit(&tx_async);
        async_rx_put_bit(&rx_async, bit);
    }
    printf("Chars=%d/%d, PE=%d, FE=%d\n", tx_async_chars, rx_async_chars, rx_async.parity_errors, rx_async.framing_errors);
    if (tx_async_chars != rx_async_chars
        ||
        rx_async.parity_errors
        ||
        rx_async.framing_errors)
    {
        printf("Test failed.\n");
        exit(2);
    }

    printf("Tests passed.\n");
    return 0;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
