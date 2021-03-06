/*
 * power.cpp
 *
 *  Created on: 11 Jul 2019
 *      Author: sid
 */

#include "cmsis_os.h"
#include "stm32l4xx.h"
#include "comp.h"

#include "power.h"

bool power::psu = false;

/* Cap > 2.225V */
bool power::isSufficiant(void)
{
#ifdef DEBUG
	return true;
#endif

	/* power always sufficiant on PSU  */
	if(psu)
		return true;

	static uint32_t nextPwr = 0;
	static bool pwrSuf = false;
	uint32_t current = osKernelSysTick();

	/* use cache */
	if(current < nextPwr)
		return pwrSuf;
	nextPwr = current + 5000;

	/* generate new value */
	HAL_COMP_Start(&hcomp2);
	pwrSuf = !!HAL_COMP_GetOutputLevel(&hcomp2);
	HAL_COMP_Stop(&hcomp2);

	return pwrSuf;
}
