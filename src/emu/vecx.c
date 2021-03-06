#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "vecx.h"

#include "e6809.h"
#include "e6522.h"
#include "e8910.h"
#include "edac.h"

void(*vecx_render) (void);

uint8_t rom[8192];
uint8_t cart[32768];
uint8_t ram[1024];

/* the sound chip registers */

uint8_t snd_select;

enum
{
	VECTREX_PDECAY = 30,      /* phosphor decay rate */

	/* number of 6809 cycles before a frame redraw */

	FCYCLES_INIT = VECTREX_MHZ / VECTREX_PDECAY,

	/* max number of possible vectors that maybe on the screen at one time.
	 * one only needs VECTREX_MHZ / VECTREX_PDECAY but we need to also store
	 * deleted vectors in a single table
	 */

	VECTOR_MAX_CNT = VECTREX_MHZ / VECTREX_PDECAY,
};

size_t vector_draw_cnt;
vector_t vectors[VECTOR_MAX_CNT];

int32_t fcycles;

/* update the snd chips internal registers when VIA.ora/VIA.orb changes */

static void snd_update(void)
{
	switch (VIA.orb & 0x18)
	{
	case 0x00:
		/* the sound chip is disabled */
		break;
	case 0x08:
		/* the sound chip is sending data */
		break;
	case 0x10:
		/* the sound chip is recieving data */
		if (snd_select != 14) e8910_write(snd_select, VIA.ora);
		break;
	case 0x18:
		/* the sound chip is latching an address */
		if ((VIA.ora & 0xf0) == 0x00) snd_select = VIA.ora & 0x0f;
		break;
	}
}

static uint8_t read8_port_a()
{
	if ((VIA.orb & 0x18) == 0x08)
	{
		/* the snd chip is driving port a */
		return e8910_read(snd_select);
	}
	else
	{
		return VIA.ora;
	}
}

static uint8_t read8_port_b()
{
	/* compare signal is an input so the value does not come from
	* VIA.orb.
	*/
	if (VIA.acr & 0x80)
	{
		/* timer 1 has control of bit 7 */
		return (uint8_t)((VIA.orb & 0x5f) | VIA.t1pb7 | DAC.compare);
	}
	else
	{
		/* bit 7 is being driven by VIA.orb */
		return (uint8_t)((VIA.orb & 0xdf) | DAC.compare);
	}
}

static void write8_port_a(uint8_t data)
{
	snd_update();

	/* output of port a feeds directly into the dac which then
	* feeds the x axis sample and hold.
	*/
	DAC.xsh = data ^ 0x80;
	dac_update();
}

static void write8_port_b(uint8_t data)
{
	(void)data;
	snd_update();
	dac_update();
}

static uint8_t read8(uint16_t address)
{
	uint8_t data = 0xff;

	if ((address & 0xe000) == 0xe000)
	{
		/* rom */
		data = rom[address & 0x1fff];
	}
	else if ((address & 0xe000) == 0xc000)
	{
		if (address & 0x800)
		{
			/* ram */
			data = ram[address & 0x3ff];
		}
		else if (address & 0x1000)
		{
			/* io */
			data = via_read(address);
		}
	}
	else if (address < 0x8000)
	{
		/* cartridge */
		data = cart[address];
	}

	return data;
}

static void write8(uint16_t address, uint8_t data)
{
	if ((address & 0xe000) == 0xe000)
	{
		/* rom */
	}
	else if ((address & 0xe000) == 0xc000)
	{
		/* it is possible for both ram and io to be written at the same! */

		if (address & 0x800)
		{
			ram[address & 0x3ff] = data;
		}

		if (address & 0x1000)
		{
			via_write(address, data);
		}
	}
	else if (address < 0x8000)
	{
		/* cartridge */
	}
}

static void addline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color)
{
	vectors[vector_draw_cnt].x0 = x0;
	vectors[vector_draw_cnt].y0 = y0;
	vectors[vector_draw_cnt].x1 = x1;
	vectors[vector_draw_cnt].y1 = y1;
	vectors[vector_draw_cnt].color = color;
	vector_draw_cnt++;
}

void vecx_input(uint8_t key, uint8_t value)
{
	uint8_t psg_io = e8910_read(14);
	switch (key)
	{
	case VECTREX_PAD1_BUTTON1: e8910_write(14, value ? psg_io & ~0x01 : psg_io | 0x01); break;
	case VECTREX_PAD1_BUTTON2: e8910_write(14, value ? psg_io & ~0x02 : psg_io | 0x02); break;
	case VECTREX_PAD1_BUTTON3: e8910_write(14, value ? psg_io & ~0x04 : psg_io | 0x04); break;
	case VECTREX_PAD1_BUTTON4: e8910_write(14, value ? psg_io & ~0x08 : psg_io | 0x08); break;

	case VECTREX_PAD2_BUTTON1: e8910_write(14, value ? psg_io & ~0x10 : psg_io | 0x10); break;
	case VECTREX_PAD2_BUTTON2: e8910_write(14, value ? psg_io & ~0x20 : psg_io | 0x20); break;
	case VECTREX_PAD2_BUTTON3: e8910_write(14, value ? psg_io & ~0x40 : psg_io | 0x40); break;
	case VECTREX_PAD2_BUTTON4: e8910_write(14, value ? psg_io & ~0x80 : psg_io | 0x80); break;

	case VECTREX_PAD1_X: DAC.jch0 = value; break;
	case VECTREX_PAD1_Y: DAC.jch1 = value; break;
	case VECTREX_PAD2_X: DAC.jch2 = value; break;
	case VECTREX_PAD2_Y: DAC.jch3 = value; break;
	}
}

void vecx_reset(void)
{
	/* ram */

	for (int r = 0; r < 1024; r++)
		ram[r] = (uint8_t)rand();

	e8910_reset();

	snd_select = 0;

	dac_add_line = addline;

	dac_reset();

	vector_draw_cnt = 0;
	fcycles = FCYCLES_INIT;

	via_read8_port_a = read8_port_a;
	via_read8_port_b = read8_port_b;
	via_write8_port_a = write8_port_a;
	via_write8_port_b = write8_port_b;

	via_reset();

	e6809_read8 = read8;
	e6809_write8 = write8;

	e6809_reset();
}

void vecx_emu(int32_t cycles)
{
	while (cycles > 0)
	{
		uint16_t icycles = e6809_sstep(VIA.ifr & 0x80, 0);

		for (uint16_t c = 0; c < icycles; c++)
		{
			via_sstep0();
			dac_sstep();
			via_sstep1();
		}

		cycles -= (int32_t)icycles;

		fcycles -= (int32_t)icycles;

		if (fcycles < 0)
		{

			fcycles += FCYCLES_INIT;
			vecx_render();

			/* everything that was drawn during this pass
			 * now is being removed.
			 */
			vector_draw_cnt = 0;
		}
	}
}
