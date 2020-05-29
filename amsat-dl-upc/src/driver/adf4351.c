/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>
#include <string.h>
#include <stdio.h>
#include <mtstd/debug.h>

#include "adf4351.h"

#include <pinmap.h>


//
//prototypes
//
static void adf4351_default(adf4351_ctx* ctx);
static void adf4351_setup(adf4351_ctx* ctx);
static void adf4351_write_latch(u32 value);
static int  adf4351_calc_counters(adf4351_ctx* ctx);
static int  adf4351_calc_vco_core_freq(adf4351_ctx* ctx, u32 rf_out_freq);
static int  adf4351_calc_rcnt_block(adf4351_ctx* ctx);

//dump all registers
static void adf4351_regdump(adf4351_ctx* ctx);

static void adf4351_write_reg0(adf4351_ctx* ctx);
static void adf4351_write_reg1(adf4351_ctx* ctx);
static void adf4351_write_reg2(adf4351_ctx* ctx);
static void adf4351_write_reg3(adf4351_ctx* ctx);
static void adf4351_write_reg4(adf4351_ctx* ctx);
static void adf4351_write_reg5(adf4351_ctx* ctx);
static inline u32 gcd(u32 a, u32 b);

//
//main control
//
void adf4351_init(adf4351_ctx* ctx, u32 ref, u32 pfd)
{
	memset(ctx, 0, sizeof(*ctx));

	//copy settings
	ctx->init = TRUE;
	ctx->ref  = ref;
	ctx->pfd  = pfd;

	//init latch enable and chipselect gpios
    gpio_set_lvl_low(PIN_ADF_LE);
    gpio_set_lvl_high(PIN_ADF_CE);

	//apply default settings
	adf4351_default(ctx);
    
    __delay_us(100);
}

//set frequency
int adf4351_set_freq(adf4351_ctx* ctx, u32 freq)
{
    //check status
	if(!ctx->init) return ERROR_NODEV;

	adf4351_calc_vco_core_freq(ctx, freq);
	adf4351_calc_rcnt_block(ctx);
	adf4351_calc_counters(ctx);
    
	if(ctx->reg4.vcopd == 0) {
		adf4351_setup(ctx);
    }

	return 0;
}

//get PLL-lock
BOOL adf4351_get_lock(adf4351_ctx* ctx)
{
	u8 lock = (gpio_get_input(PIN_ADF_LOCK)==0)?FALSE:TRUE;
	static u8 lastlock = 255;
	
	if(lock != lastlock)
	{
		printf("UPC 00 09 ADF4351 %s\n",lock?"LOCKED":"UNLOCKED");
		lastlock = lock;
	}

	return lock;
}

void adf4351_set_rfout_enable(adf4351_ctx* ctx, BOOL enable)
{
    if(enable) {
        ctx->reg4.rfoutena = 1;		//RF out enable
    } else {
        ctx->reg4.rfoutena = 0;		//RF out disable
    }
    //write control register
    adf4351_write_reg4(ctx);
    adf4351_write_reg2(ctx);
}

//set mode of operation
void adf4351_set_mode(adf4351_ctx* ctx, BOOL enable)
{
	//check status
	if(!ctx->init) return;

	//update register
	if(enable) {
		ctx->reg4.vcopd = 0;		//no VCO power down
		ctx->reg2.pd = 0;           //Power-Down disable

		//do complete power-up sequence just to be sure
		adf4351_setup(ctx);
	}  else {
		ctx->reg4.vcopd = 1;		//VCO power down
		ctx->reg2.pd = 1;           //Power-Down enable

		//write control register
		adf4351_write_reg4(ctx);
		adf4351_write_reg2(ctx);
	}
}

//set tuning parameters
void adf4351_set_tune(adf4351_ctx* ctx, u8 icp, u8 pout, BOOL lownoise)
{
	printd("adf4351_set_tune(%p, %u, %u, %i)\n", ctx, icp, pout, lownoise);

    //update registers
    ctx->reg2.lownoise = lownoise?0:3;
    ctx->reg2.cpc      = icp;
    ctx->reg4.rfoutpwr = pout;

    if(ctx->reg4.vcopd == 0) {
        adf4351_setup(ctx);
    }
}

//set ADF4351 to default state
static void adf4351_default(adf4351_ctx* ctx)
{
	ctx->reg1.phase     = 1; //recommeded value
	ctx->reg2.muxout    = 6; //digital lock detect
	ctx->reg2.cpc       = 7;
	ctx->reg2.lownoise  = 3; //low spurs mode
	ctx->reg2.pdpol     = 1;
	ctx->reg4.fbsel     = 1; //feedback directly from vco core
	ctx->reg4.rfoutena  = 0; //RF output disabled
	ctx->reg4.rfoutpwr  = 3;
    ctx->reg4.vcopd     = 1; //VCO power down
    ctx->reg5.ldpinmod  = 1; //digital lock detect
}

static void adf4351_setup(adf4351_ctx* ctx)
{
    adf4351_write_reg5(ctx);
    adf4351_write_reg4(ctx);
    adf4351_write_reg3(ctx);
    adf4351_write_reg2(ctx);
    adf4351_write_reg1(ctx);
    adf4351_write_reg0(ctx);
}

//calc all counters for given frequency
static int adf4351_calc_counters(adf4351_ctx* ctx)
{
	u32 integer, frac, mod, remainder;

	// we always compare with the vco core frequency for now
	integer = ctx->vco_freq / ctx->pfd;
	remainder = ctx->vco_freq % ctx->pfd;
    
    printd("ADF4351: vco_freq=%li => integer=%li remainder=%li pfd=%li\r\n", ctx->vco_freq, integer, remainder, ctx->pfd);

	// we always use the 4/5 prescaler for now
	if ((integer < 23) || (integer > 65535))
		return ERROR_RANGE;

	frac = remainder;
	mod  = ctx->pfd;

    printd("ADF4351 start: frac=%li mod=%li\r\n", frac, mod);

	while (mod >= 4096) {
		frac /= 2;
		mod /= 2;
	}
    
    printd("ADF4351 pre: frac=%li mod=%li\r\n", frac, mod);

	if(frac) {
		//frac-n mode
		u32 div = gcd(mod, frac);
		frac /= div;
		mod  /= div;
	} else {
		//int-n mode
		frac = 0;
		mod  = 2;
	}

    printd("ADF4351 post: frac=%li mod=%li\r\n", frac, mod);

	ctx->reg0.integer = integer;
	ctx->reg0.frac = frac;
    ctx->reg1.mod = mod;
    ctx->reg2.ldf = frac?0:1;
	
	return 0;
}

static int adf4351_calc_rcnt_block(adf4351_ctx* ctx)
{
	int div;
    u32 bscdiv;

	// 2x doubler and 2x divider are always disabled for now

	div = ctx->ref / ctx->pfd;

	if (ctx->ref % ctx->pfd) {
		printf("ADF4351: Cannot reach PFD of %i kHz with REF of %i kHz (not divideable).\n", ctx->pfd, ctx->ref);
		return ERROR_INVALID;
	}

	if (div >= 1024) {
		printf("ADF4351: Cannot reach PFD of %i kHz with REF of %i kHz (div too high).\n", ctx->pfd, ctx->ref);
		return ERROR_RANGE;
	}

    //special optimizations
	if((div%2)==0) {
		ctx->reg2.rcnt   = div/2;
		ctx->reg2.refdiv = 1;
	} else {
		ctx->reg2.rcnt   = div;
		ctx->reg2.refdiv = 0;
	}
	ctx->reg3.bscm = 0;
	bscdiv = ((ctx->pfd-1) / 125) + 1;

	if (bscdiv > 255) {
		ctx->reg3.bscm = 1;
		bscdiv = ((ctx->pfd-1) / 250) + 1;
	}

	if (bscdiv > 255) {
		printf("ADF4351: Cannot use pfd of %lu kHz (bscdiv too high).\n", ctx->pfd);
		return ERROR_INVALID;
	}

    ctx->reg4.bsclkdiv = bscdiv;
    
	return 0;
}

static int adf4351_calc_vco_core_freq(adf4351_ctx* ctx, u32 rf_out_freq)
{
	int div = 0;

	if (rf_out_freq > 4400000)
		return ERROR_RANGE;

	while ((rf_out_freq <= 2200000) && (div <= 4)) {
		rf_out_freq *= 2;
		div += 1;
	}

	if ((rf_out_freq <= 2200000) || (div > 4)) {
		return ERROR_RANGE;
    }

	ctx->vco_freq = rf_out_freq;
    ctx->reg1.prescaler = (ctx->vco_freq>3600000)?1:0;
	ctx->reg4.divsel = div;

	return 0;
}

/*
//dump all registers
void adf4351_regdump(adf4351_ctx* ctx)
{
	if (!ctx->init) {
		printf("ADF4351 not initialized\n");
		return;
	}

	//dump control latch
	printf("\n=== ADF4351 registers ===\n");
    printf("not implemented yet!\n");
}
*/
static void adf4351_write_reg0(adf4351_ctx* ctx)
{
	if(ctx->init) {
        u32 data = 0;

        data |= (((u32)ctx->reg0.frac&0x0FFF)<<3);        
        data |= (((u32)ctx->reg0.integer&0xFFFF)<<15);

        printd("ADF4351 WRITE REG0: 0x%08lX (int=%i frac=%i)\r\n", data, ctx->reg0.integer, ctx->reg0.frac);
        adf4351_write_latch(data);
    }
}

static void adf4351_write_reg1(adf4351_ctx* ctx)
{
    if(ctx->init) {
        u32 data = 1;
        
        data |= (((u32)ctx->reg1.mod&0x0FFF)<<3);
        data |= (((u32)ctx->reg1.phase&0x0FFF)<<15);
        data |= (((u32)ctx->reg1.prescaler&0x0001)<<27);
        
        printd("ADF4351 WRITE REG1: 0x%08lX (prescaler=%i phase=%i mod=%i)\r\n", data, ctx->reg1.prescaler, ctx->reg1.phase, ctx->reg1.mod);
        adf4351_write_latch(data);
    }
}

static void adf4351_write_reg2(adf4351_ctx* ctx)
{
    if(ctx->init) {
        u32 data = 2;

        data |= (((u32)ctx->reg2.cntreset&0x0001)<<3);
        data |= (((u32)ctx->reg2.cpts&0x0001)<<4);
        data |= (((u32)ctx->reg2.pd&0x0001)<<5);
        data |= (((u32)ctx->reg2.pdpol&0x0001)<<6);
        data |= (((u32)ctx->reg2.ldp&0x0001)<<7);
        data |= (((u32)ctx->reg2.ldf&0x0001)<<8);
        data |= (((u32)ctx->reg2.cpc&0x000F)<<9);
        data |= (((u32)ctx->reg2.dblbuff&0x0001)<<13);
        data |= (((u32)ctx->reg2.rcnt&0x03FF)<<14);
        data |= (((u32)ctx->reg2.refdiv&0x0001)<<24);
        data |= (((u32)ctx->reg2.refdbl&0x0001)<<25);
        data |= (((u32)ctx->reg2.muxout&0x0007)<<26);
        data |= (((u32)ctx->reg2.lownoise&0x0003)<<29);

        printd("ADF4351 WRITE REG2: 0x%08lX (lownoise=%i muxout=%i refdbl=%i refdiv=%i rcnt=%i dblbuff=%i cpc=%i ldf=%i ldp=%i pdpol=%i pd=%i cpts=%i cntreset=%i\r\n", data,
                ctx->reg2.lownoise, ctx->reg2.muxout, ctx->reg2.refdbl, ctx->reg2.refdiv, ctx->reg2.rcnt, ctx->reg2.dblbuff, 
                ctx->reg2.cpc, ctx->reg2.ldf, ctx->reg2.ldp, ctx->reg2.pdpol, ctx->reg2.pd, ctx->reg2.cpts, ctx->reg2.cntreset);
        
        adf4351_write_latch(data);
    }  
}

static void adf4351_write_reg3(adf4351_ctx* ctx)
{
    if(ctx->init) {
        u32 data = 3;

        data |= (((u32)ctx->reg3.clkdiv&0x0FFF)<<3);
        data |= (((u32)ctx->reg3.clkdivmode&0x0003)<<15);
        data |= (((u32)ctx->reg3.csr&0x0001)<<18);
        data |= (((u32)ctx->reg3.chcancel&0x0001)<<21);
        data |= (((u32)ctx->reg3.abp&0x0001)<<22);
        data |= (((u32)ctx->reg3.bscm&0x0001)<<23);

        printd("ADF4351 WRITE REG3: 0x%08lX (bscm=%i, abp=%i, chcancel=%i csr=%i clkdivmode=%i clkdiv=%i\r\n", data,
                ctx->reg3.bscm, ctx->reg3.abp, ctx->reg3.chcancel, ctx->reg3.csr, ctx->reg3.clkdivmode, ctx->reg3.clkdiv);
        adf4351_write_latch(data);
    }
}

static void adf4351_write_reg4(adf4351_ctx* ctx)
{
    if(ctx->init) {
        u32 data = 4;

        data |= (((u32)ctx->reg4.rfoutpwr&0x0003)<<3);
        data |= (((u32)ctx->reg4.rfoutena&0x0001)<<5);
        data |= (((u32)ctx->reg4.auxpwrout&0x0003)<<6);
        data |= (((u32)ctx->reg4.auxoutena&0x0001)<<8);
        data |= (((u32)ctx->reg4.auxoutsel&0x0001)<<9);
        data |= (((u32)ctx->reg4.mtld&0x0001)<<10);
        data |= (((u32)ctx->reg4.vcopd&0x0001)<<11);
        data |= (((u32)ctx->reg4.bsclkdiv&0x00FF)<<12);
        data |= (((u32)ctx->reg4.divsel&0x0007)<<20);
        data |= (((u32)ctx->reg4.fbsel&0x0001)<<23);
        
        printd("ADF4351 WRITE REG4: 0x%08lX (fbsel=%i, divsel=%i, bsclkdiv=%i, vcopd=%i, mtld=%i, auxoutsel=%i, auxoutena=%i, auxpwrout=%i, rfoutena=%i rfoutpwr=%i \r\n", data,
                ctx->reg4.fbsel, ctx->reg4.divsel, ctx->reg4.bsclkdiv, ctx->reg4.vcopd, ctx->reg4.mtld, ctx->reg4.auxoutsel, ctx->reg4.auxoutena, ctx->reg4.auxpwrout, ctx->reg4.rfoutena, ctx->reg4.rfoutpwr);
        adf4351_write_latch(data);
    }
}

static void adf4351_write_reg5(adf4351_ctx* ctx)
{
    if(ctx->init) {
        u32 data = 5;

        data |= (((u32)ctx->reg5.ldpinmod&0x0003)<<22);

        printd("ADF4351 WRITE REG5: 0x%08lX (ldpinmod=%i)\r\n", data, ctx->reg5.ldpinmod);
        adf4351_write_latch(data);
    }
}

static inline u32 gcd(u32 a, u32 b)
{
    while((a%b) != 0) {
        u32 c = a;

        if(a < b) {
            a = b;
            b = c;
        } else {
            a = b;
            b = c%b;
        }
    }
    
    return b;
}

//write value into latch
static void adf4351_write_latch(u32 value)
{
    //printd("0x%08lX\n", value);
    u32 mask = 0x80000000ULL;

    gpio_set_lvl_low(PIN_SIO_CLK);
    __delay_us(1);
	//write 32bit
    while(mask) {
        if(value&mask) {
            gpio_set_lvl_high(PIN_SIO_DATA);
        } else {
            gpio_set_lvl_low(PIN_SIO_DATA);
        }
        __delay_us(1);
        gpio_set_lvl_high(PIN_SIO_CLK);
        __delay_us(1);
        gpio_set_lvl_low(PIN_SIO_CLK);
        __delay_us(1);
        //next bit
        mask >>= 1;
    }

	//pulse latch-enable
	gpio_set_lvl_high(PIN_ADF_LE);
	__delay_us(1);
	gpio_set_lvl_low(PIN_ADF_LE);
	__delay_us(50);
}
