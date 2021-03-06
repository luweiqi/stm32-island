/*
 * This file is part of the stm32-template project.
 *
 * Copyright (C) 2020 Johannes Huebner <dev@johanneshuebner.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rtc.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/stm32/iwdg.h>
#include "stm32_can.h"
#include "terminal.h"
#include "params.h"
#include "hwdefs.h"
#include "digio.h"
#include "hwinit.h"
#include "anain.h"
#include "param_save.h"
#include "my_math.h"
#include "errormessage.h"
#include "printf.h"
#include "stm32scheduler.h"
#include "pwmgeneration.h"

static Stm32Scheduler* scheduler;
static Can* can;
static PwmGeneration* pwm;

HWREV hwRev = HW_PRIUS;

//sample 100ms task
static void Ms100Task(void)
{
   //The following call toggles the LED output, so every 100ms
   //The LED changes from on to off and back.
   //Other calls:
   //DigIo::led_out.Set(); //turns LED on
   //DigIo::led_out.Clear(); //turns LED off
   //For every entry in digio_prj.h there is a member in DigIo
   DigIo::led_out.Toggle();
   //The boot loader enables the watchdog, we have to reset it
   //at least every 2s or otherwise the controller is hard reset.
   iwdg_reset();
   //Calculate CPU load. Don't be surprised if it is zero.
   s32fp cpuLoad = FP_FROMINT(scheduler->GetCpuLoad());
   //This sets a fixed point value WITHOUT calling the parm_Change() function
   Param::SetFlt(Param::cpuload, cpuLoad / 10);

   //If we chose to send CAN messages every 100 ms, do this here.
   if (Param::GetInt(Param::canperiod) == CAN_PERIOD_100MS)
      can->SendAll();
}

//sample 10 ms task
static void Ms10Task(void)
{
   if (Param::GetInt(Param::start) == MOD_RUN)
   {
      if (Param::GetInt(Param::opmode) != MOD_RUN)
      {
         uint32_t banks[2] = { GPIOA, GPIOB };
         uint16_t pins[2] = { GPIO8 | GPIO9 | GPIO10, GPIO13 | GPIO14 | GPIO15 };

         pwm->SetPolarity(false, banks, pins, 2);
         pwm->SetPwmDigits(MIN_PWM_DIGITS + Param::GetInt(Param::pwmfrq));
         pwm->Start();
         DigIo::dcsw_out.Set();
         Param::SetInt(Param::opmode, MOD_RUN);
      }
   }
   else if (Param::GetInt(Param::start) == MOD_OFF)
   {
      Param::SetInt(Param::opmode, MOD_OFF);
      DigIo::dcsw_out.Clear();
      pwm->Stop();
   }

   //If we chose to send CAN messages every 10 ms, do this here.
   if (Param::GetInt(Param::canperiod) == CAN_PERIOD_10MS)
      can->SendAll();
}

/** This function is called when the user changes a parameter */
extern void parm_Change(Param::PARAM_NUM paramNum)
{
   switch (paramNum)
   {
   default:
      pwm->SetUdc(Param::Get(Param::udcspnt));
      pwm->ConfigureUdcController(Param::GetInt(Param::udckp), Param::GetInt(Param::udcki));
      pwm->SetFrequency(Param::Get(Param::frq));
      pwm->SetCurrentDivider(Param::Get(Param::il1gain), Param::Get(Param::il2gain));
      //Handle general parameter changes here. Add paramNum labels for handling specific parameters
      break;
   }
}

//Whichever timer(s) you use for the scheduler, you have to
//implement their ISRs here and call into the respective scheduler
extern "C" void tim2_isr(void)
{
   scheduler->Run();
}

extern "C" void tim1_brk_isr(void)
{
   timer_disable_irq(TIM1, TIM_DIER_BIE);
   pwm->Stop();
   Param::SetInt(Param::opmode, MOD_OFF);
}

extern "C" void tim1_up_isr(void)
{
   s32fp il[2];
   s32fp udcgain = Param::Get(Param::udcgain);
   int udcofs = Param::GetInt(Param::udcofs);
   s32fp udc = FP_DIV(FP_FROMINT(AnaIn::udc.Get() - udcofs), udcgain);

   /* Clear interrupt pending flag */
   timer_clear_flag(TIM1, TIM_SR_UIF);
   int dc = pwm->Run(udc, il);

   Param::SetFlt(Param::udc, udc);
   Param::SetInt(Param::boosteramp, dc);
   Param::SetFlt(Param::il1, il[0]);
   Param::SetFlt(Param::il2, il[1]);
}

extern "C" int main(void)
{
   clock_setup(); //Must always come first
   rtc_setup();
   ANA_IN_CONFIGURE(ANA_IN_LIST);
   DIG_IO_CONFIGURE(DIG_IO_LIST);
   AnaIn::Start(); //Starts background ADC conversion via DMA
   write_bootloader_pininit(); //Instructs boot loader to initialize certain pins

   usart_setup(); //Initializes UART3 with DMA for use by the terminal (see below)
   tim_setup(); //Sample init of a timer
   nvic_setup(); //Set up some interrupts
   term_Init(); //Initialize terminal
   parm_load(); //Load stored parameters

   PwmGeneration p(TIM1);
   pwm = &p;

   parm_Change(Param::PARAM_LAST); //Call callback one for general parameter propagation

   Stm32Scheduler s(TIM2); //We never exit main so it's ok to put it on stack
   scheduler = &s;
   //Initialize CAN1, including interrupts. Clock must be enabled in clock_setup()
   Can c(CAN1, (Can::baudrates)Param::GetInt(Param::canspeed));
   //store a pointer for easier access
   can = &c;


   //Up to four tasks can be added to each timer scheduler
   //AddTask takes a function pointer and a calling interval in milliseconds.
   //The longest interval is 655ms due to hardware restrictions
   //You have to enable the interrupt (int this case for TIM2) in nvic_setup()
   //There you can also configure the priority of the scheduler over other interrupts
   s.AddTask(Ms10Task, 10);
   s.AddTask(Ms100Task, 100);

   //backward compatibility, version 4 was the first to support the "stream" command
   Param::SetInt(Param::version, 4);

   //Now all our main() does is running the terminal
   //All other processing takes place in the scheduler or other interrupt service routines
   //The terminal has lowest priority, so even loading it down heavily will not disturb
   //our more important processing routines.
   term_Run();

   return 0;
}

