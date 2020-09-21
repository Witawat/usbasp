/*
 * isp.c - part of USBasp
 *
 * Autor..........: Thomas Fischl <tfischl@gmx.de>
 *                  Ralph Doncaster at gmail dot com
 * Description....: Provides functions for communication/programming
 *                  over ISP interface
 * Licence........: GNU GPL v2 (see Readme.txt)
 * Creation Date..: 2005-02-23
 * Last change....: 2020-09-14
 */

#include <avr/io.h>
#include "isp.h"
#include "clock.h"
#include "usbasp.h"

uchar sck_sw_delay;
uchar isp_hiaddr;

static inline void spiHWenable() {
    /* enable SPI, master */
    SPCR |= (1 << SPE) | (1 << MSTR);
}

static inline void spiHWdisable() {
    SPCR = 0;
}

void ispSetSCKOption(uchar option) {

    if (option == USBASP_ISP_SCK_AUTO)
        option = USBASP_ISP_SCK_1500;

    if (option >= USBASP_ISP_SCK_93_75) {
        ispTransmit = ispTransmit_hw;
        SPSR = 0;
        sck_sw_delay = 1;    /* force RST#/SCK pulse for 320us */

        switch (option) {

        case USBASP_ISP_SCK_3000:
            /* 3MHz, XTAL/4 */
            SPCR = 0;
            break;
        case USBASP_ISP_SCK_1500:
        default:
            /* 1.5MHz, XTAL/8 */
            SPSR = (1 << SPI2X);
        case USBASP_ISP_SCK_750:
            /* 750kHz, XTAL/16 */
            SPCR = (1 << SPR0);
            break;
        case USBASP_ISP_SCK_375:
            /* 375kHz, XTAL/32 (default) */
            SPSR = (1 << SPI2X);
        case USBASP_ISP_SCK_187_5:
            /* 187.5kHz XTAL/64 */
            SPCR = (1 << SPR1);
            break;
        case USBASP_ISP_SCK_93_75:
            /* 93.75kHz XTAL/128 */
            SPCR = (1 << SPR1) | (1 << SPR0);
            break;
        }

    } else {
        ispTransmit = ispTransmit_sw;
#if 0
        switch (option) {

        case USBASP_ISP_SCK_32:
            sck_sw_delay = 3; break;
        case USBASP_ISP_SCK_16:
            sck_sw_delay = 6; break;
        case USBASP_ISP_SCK_8:
            sck_sw_delay = 12; break;
        case USBASP_ISP_SCK_4:
            sck_sw_delay = 24; break;
        case USBASP_ISP_SCK_2:
            sck_sw_delay = 48; break;
        case USBASP_ISP_SCK_1:
            sck_sw_delay = 96; break;
        case USBASP_ISP_SCK_0_5:
            sck_sw_delay = 192; break;
        }
#endif
        /* more efficient than switch */
        sck_sw_delay = 3 << (USBASP_ISP_SCK_32 - option);
    }
}

void ispDelay() {

    uint8_t starttime = TIMERVALUE;
    while ((uint8_t) (TIMERVALUE - starttime) < sck_sw_delay) {
    }
}

void ispConnect() {

    /* all ISP pins were inputs before, now set output pins
     * V-USB modifies DDR, so set only one at a time for atomic sbi */
    ISP_DDR |= (1 << ISP_SCK);
    ISP_DDR |= (1 << ISP_MOSI);
    ISP_DDR |= (1 << ISP_RST);

    /* enable pullup on MISO for improved noise immunity */
    ISP_OUT |= (1 << ISP_MISO);

    /* positive pulse on RST for at least 2 target clock cycles */
    ISP_OUT |= (1 << ISP_RST);
    clockWait(1);                       /* 320us */
    ISP_OUT &= ~(1 << ISP_RST);

    /* Initial extended address value */
    isp_hiaddr = 0xff;  /* ensure that even 0x00000 causes a write of the extended address byte */
}

void ispDisconnect() {

    /* set all ISP pins inputs */
    ISP_DDR &= ~((1 << ISP_RST) | (1 << ISP_SCK) | (1 << ISP_MOSI));
    /* switch pullups off */
    ISP_OUT &= ~((1 << ISP_RST) | (1 << ISP_SCK) | (1 << ISP_MOSI));

    /* disable hardware SPI */
    spiHWdisable();
}

// todo: make ispTransmit function that checks mode and branches
uchar ispTransmit_sw(uchar send_byte) {

    uchar rec_byte = 0;
    uchar i;
    for (i = 0; i < 8; i++) {

        /* set MSB to MOSI-pin */
        if ((send_byte & 0x80) != 0) {
            ISP_OUT |= (1 << ISP_MOSI); /* MOSI high */
        } else {
            ISP_OUT &= ~(1 << ISP_MOSI); /* MOSI low */
        }
        /* shift to next bit */
        send_byte = send_byte << 1;

        /* receive data */
        rec_byte = rec_byte << 1;
        if ((ISP_IN & (1 << ISP_MISO)) != 0) {
            rec_byte++;
        }

        /* pulse SCK */
        ISP_OUT |= (1 << ISP_SCK); /* SCK high */
        ispDelay();
        ISP_OUT &= ~(1 << ISP_SCK); /* SCK low */
        ispDelay();
    }

    return rec_byte;
}

uchar ispTransmit_hw(uchar send_byte) {
    SPDR = send_byte;

    while (!(SPSR & (1 << SPIF)));
    return SPDR;
}

uchar ispEnterProgrammingMode() {
    uchar check;

    if (prog_sck == 0) prog_sck = USBASP_ISP_SCK_1500;

    while (prog_sck >= USBASP_ISP_SCK_0_5) {
        uchar (*spiTx)(uchar) = ispTransmit;

        if (ispTransmit == ispTransmit_hw) spiHWenable();

        uchar tries = 3;
        do {
            /* pulse RST */
            ISP_OUT |= (1 << ISP_RST);      /* RST high */
            clockWait(1);                   /* 320us */
            ISP_OUT &= ~(1 << ISP_RST);     /* RST low */

            /* datasheet says wait 20ms, even though less seems fine */
            clockWait(20 / 0.320);          /* wait before PE */

            spiTx(0xAC);
            spiTx(0x53);
            check = spiTx(0);
            spiTx(0);

            if (check == 0x53) {
                /* bump up speed now that programming mode is enabled */
#               if DANGEROUS_MODE
                spiHWdisable();
                ispSetSCKOption(prog_sck + 1);
                if (ispTransmit == ispTransmit_hw) spiHWenable();
#               endif
                return 0;
            }
        } while (--tries);

        spiHWdisable();

        ispSetSCKOption(--prog_sck);    /* try lower speed */
    }

    return 1; /* error: device dosn't answer */
}

static void ispUpdateExtended(unsigned long address)
{
    uchar curr_hiaddr;

    curr_hiaddr = (address >> 17);

    /* check if extended address byte is changed */
    if(isp_hiaddr != curr_hiaddr)
    {
        isp_hiaddr = curr_hiaddr;
        /* Load Extended Address byte */
        ispTransmit(0x4D);
        ispTransmit(0x00);
        ispTransmit(isp_hiaddr);
        ispTransmit(0x00);
    }
}

uchar ispReadFlash(unsigned long address) {

    ispUpdateExtended(address);

    ispTransmit(0x20 | ((address & 1) << 3));
    ispTransmit(address >> 9);
    ispTransmit(address >> 1);
    return ispTransmit(0);
}

uchar ispWriteFlash(unsigned long address, uchar data, uchar pollmode) {

    /* 0xFF is value after chip erase, so skip programming
     if (data == 0xFF) {
     return 0;
     }
     */

    ispUpdateExtended(address);

    ispTransmit(0x40 | ((address & 1) << 3));
    ispTransmit(address >> 9);
    ispTransmit(address >> 1);
    ispTransmit(data);

    if (pollmode == 0)
        return 0;

    if (data == 0x7F) {
        clockWait(15); /* wait 4,8 ms */
        return 0;
    } else {

        /* polling flash */
        uchar retries = 30;
        uint8_t starttime = TIMERVALUE;
        while (retries != 0) {
            if (ispReadFlash(address) != 0x7F) {
                return 0;
            };

            if ((uint8_t) (TIMERVALUE - starttime) > CLOCK_T_320us) {
                starttime = TIMERVALUE;
                retries--;
            }

        }
        return 1; /* error */
    }

}

uchar ispFlushPage(unsigned long address, uchar pollvalue) {

    ispUpdateExtended(address);
    
    ispTransmit(0x4C);                  // write page
    ispTransmit(address >> 9);
    ispTransmit(address >> 1);
    ispTransmit(0);

    if (pollvalue == 0xFF) {
        clockWait(15);
        return 0;
    } else {

        /* polling flash */
        uchar retries = 30;
        uint8_t starttime = TIMERVALUE;

        while (retries != 0) {
            if (ispReadFlash(address) != 0xFF) {
                return 0;
            };

            if ((uint8_t) (TIMERVALUE - starttime) > CLOCK_T_320us) {
                starttime = TIMERVALUE;
                retries--;
            }

        }

        return 1; /* error */
    }

}

uchar ispReadEEPROM(unsigned int address) {
    ispTransmit(0xA0);
    ispTransmit(address >> 8);
    ispTransmit(address);
    return ispTransmit(0);
}

uchar ispWriteEEPROM(unsigned int address, uchar data) {

    ispTransmit(0xC0);
    ispTransmit(address >> 8);
    ispTransmit(address);
    ispTransmit(data);

    clockWait(30); // wait 9,6 ms

    return 0;
}
