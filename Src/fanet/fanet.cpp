/*
 * fanet.cpp
 *
 *  Created on: Jul 4, 2018
 *      Author: sid
 */

#include "cmsis_os.h"

#include "common.h"
#include "minmax.h"
#include "print.h"

#include "../hal/power.h"
#include "../hal/wind.h"
#include "../misc/rnd.h"
#include "../misc/sha1.h"
#include "../serial/serial_interface.h"
#include "sx1272.h"
#include "fanet.h"
#include "frame/fname.h"
#include "frame/fgndtracking.h"
#include "frame/fservice.h"
#include "frame/ftracking.h"

osMessageQId fanet_QueueID;

void fanet_rtos(void)
{
	/* Queue */
	osMessageQDef(Fanet_Queue, FANET_QUEUE_SIZE, uint16_t);
	fanet_QueueID = osMessageCreate (osMessageQ(Fanet_Queue), NULL);
}

void fanet_irq(void)
{
	if(osKernelRunning() == 0)
		return;

	osMessagePut(fanet_QueueID, FANET_SX1272_IRQ, osWaitForever);
}

void fanet_task(void const * argument)
{
	/* seed rnd */
	/* start random machine */
	rnd::seed(HAL_GetUIDw0() + HAL_GetUIDw1() + HAL_GetUIDw2());

	/* configure mac */
	while(fmac.init(fanet) == false)
	{
		serialInt.print_line(FN_REPLYE_RADIO_FAILED);
		osDelay(1000);
	}
	//fmac.writeAddr(FanetMacAddr(1, 2));

	osThreadYield();
	serialInt.print_line(FN_REPLYM_INITIALIZED);
	debug_printf("Addr %02X:%04X\n", fmac.addr.manufacturer, fmac.addr.id);

	/* turn on FANET */
	fmac.setPower(true);
//todo tx power

	/* fanet loop */
	while(1)
	{
		/* Get the message from the queue */
		osEvent event = osMessageGet(fanet_QueueID, MAC_SLOT_MS);
		if(event.status == osEventMessage)
		{
			if(event.value.v == FANET_SX1272_IRQ)
				sx1272_irq();
		}

		/* handle rx/tx stuff */
		fmac.handle();

		/* handle regular fanet stuff */
		fanet.handle();
	}
}

/*
 * C++
 */

/*
 * Memory management
 */
void *operator new(size_t size)
{
	void *mem = pvPortMalloc(size);
		return mem;
}

void operator delete(void *p)
{
	vPortFree(p);
}


void Fanet::handle(void)
{
	const uint32_t current = osKernelSysTick();

#if 0
	pwr management

	/* turn on rf chip */
	if(is_broadcast_ready(fmac.numNeighbors()))
	{
		/* enabling radio chip */
		if(!sx1272_isArmed())
		{
			last_framecount = framecount;
#if defined(DEBUG) || defined(DEBUG_SEMIHOSTING)
			printf("%u en (%d)\n", (unsigned int)HAL_GetTick(), pwr_suf);
#endif
		}
		sx1272_setArmed(true);
		last_ready_time = HAL_GetTick();
	}
	else if((last_ready_time + WSAPP_RADIO_UPTIME < HAL_GetTick() || !power_sufficiant()) &&
			fmac.tx_queue_depleted() && framecount != last_framecount)
	{
		/* disabling radio chip */
#if defined(DEBUG) || defined(DEBUG_SEMIHOSTING)
		if(sx1272_isArmed())
			printf("%u dis\n", (unsigned int)HAL_GetTick());
#endif
		sx1272_setArmed(false);
	}

	required???
	/* sleep for 10ms */
	//note: tick will wake us up every 1ms
	for(int i=0; i<10; i++)
		HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);

#endif


	//remote config request/reply... maybe 2 (pos) has to move to 3
	//next power settings! for low pwr + diasbale goe forwarding
	//vario com, pos, replay, geofence....


//todo disable forward, promiscous enabed if geo-based forward rule
fmac.doForward = false;
fmac.promiscuous = true;
//todo


	/* time for next replay broadcast? */
	if(nextRfBroadcast < current)
	{
		/* just in case, nothing is found delay next eval by 10sec */
		nextRfBroadcast = current + 10000;

		/* find next index */
		bool found = false;
		for(uint16_t i=0; i<NELEM(replayFeature); i++)
		{
			/* overflow -> pause */
			if(++nextRfIdx >= (int16_t)NELEM(replayFeature))
			{
				nextRfIdx = -1;
				nextRfBroadcast = current + FANET_TYPE6_PAUSE_MS;
				break;
			}

			if(nextRfIdx >= 0 && replayFeature[nextRfIdx].isVaild())
			{
				found = true;
				break;
			}
		}

		/* send frame */
		if(found == true)
		{
			/* in case of a busy channel, ensure that frames from the tx fifo gets also a change */
			nextRfBroadcast = current + FANET_TYPE6_TAU_MS * (!!power::isSufficiant() + 1);

			/* broadcast replay */
			FanetFrame *frm = replayFeature[nextRfIdx].toFrame();
			if(fmac.transmit(frm) != 0)
			{
				/* failed, try again latter */
				delete frm;
				nextRfIdx--;
				nextRfBroadcast = current + FANET_TYPE6_TAU_MS;
			}
		}
	}

	/* remove unavailable nodes */
	fanet.cleanNeighbors();
}

std::list<FanetNeighbor*> &Fanet::getNeighbors_locked(void)
{
	osMutexWait(neighborMutex, osWaitForever);
	return neighbors;
}

FanetNeighbor *Fanet::getNeighbor_locked(const FanetMacAddr &addr)
{
	osMutexWait(neighborMutex, osWaitForever);

	/* find neighbor of interest */
	FanetNeighbor *retNeighbor = nullptr;
	for(auto *n : neighbors)
	{
		if(n->addr == addr)
		{
			retNeighbor = n;
			break;
		}
	}

	/* does not exist -> release */
	if(retNeighbor == nullptr)
		osMutexRelease(neighborMutex);

	return retNeighbor;
}

void Fanet::releaseNeighbors(void)
{
	osMutexRelease(neighborMutex);
}

void Fanet::cleanNeighbors(void)
{
	osMutexWait(neighborMutex, osWaitForever);
	neighbors.remove_if([](FanetNeighbor *fn){ if(fn->isAround()) return false; delete fn; return true; });
	osMutexRelease(neighborMutex);
}

void Fanet::seenNeighbor(FanetMacAddr &addr)
{
	osMutexWait(neighborMutex, osWaitForever);

	/* update neighbors list */
	bool neighbor_known = false;
	for(auto *neighbor : neighbors)
	{
		if(neighbor->addr == addr)
		{
			neighbor->seen();
			neighbor_known = true;
			break;
		}
	}

	/* neighbor unknown until now, add to list */
	if (neighbor_known == false)
	{
		/* too many neighbors, delete oldest member (front) */
		if (neighbors.size() > FANET_NEIGHBOR_SIZE)
			neighbors.pop_front();

		neighbors.push_back(new FanetNeighbor(addr));
	}

	osMutexRelease(neighborMutex);
}

bool Fanet::isNeighbor(FanetMacAddr & addr)
{
	osMutexWait(neighborMutex, osWaitForever);
	bool found = false;

	for(auto *neighbor : neighbors)
	{
		if(neighbor->addr == addr)
		{
			found = true;
			break;
		}
	}

	osMutexRelease(neighborMutex);
	return found;
}

uint16_t Fanet::numNeighbors(void)
{
	osMutexWait(neighborMutex, osWaitForever);
	uint16_t nn = neighbors.size();
	osMutexRelease(neighborMutex);

	return nn;
}

FanetFrame *Fanet::broadcastIntended(void)
{
	const uint32_t current = osKernelSysTick();

	/* time for next service broadcast? */
	if(nextServiceBroadcast > current)
		return nullptr;

	FanetFrameService *sfrm = new FanetFrameService(false, strlen(fanet.key)>0);		//no Inet but remoteCfg if key present
	debug_printf("Service Tx\n");
	if(wind.sensorPresent)
		sfrm->setWind(wind.getDir_2min_avg(), wind.getSpeed_2min_avg(), wind.getSpeed_max());
	sfrm->setSoc(power::isSufficiant() ? 100.0f : 30.0f);

	/* in case of a busy channel, ensure that frames from the tx fifo gets also a change */
	nextServiceBroadcast = current + 1000;
	return sfrm;
}

void Fanet::broadcastSuccessful(FanetFrame::FrameType_t type)
{
	const uint32_t current = osKernelSysTick();

	/* service frame */
	nextServiceBroadcast = current + (numNeighbors()/20+1) * FANET_TYPE4_TAU_MS * (!!power::isSufficiant() + 1);
}

void Fanet::handleAcked(bool ack, FanetMacAddr &addr)
{
	if(frameToConsole)
		serialInt.handle_acked(ack, addr);

	ackRes = ack ? ACK : NACK;
	ackAddr = addr;							//note: address has to be set as second to prevent race conditions!
}


void Fanet::handleFrame(FanetFrame *frm)
{
	if(frameToConsole)
		serialInt.handleFrame(frm);

	/* decode */
	if(frm->dest == fmac.addr || frm->dest == FanetMacAddr())
	{
		osMutexWait(neighborMutex, osWaitForever);

		/* find neighbor */
		//note: prev called seenNeighbor() which ensures neighbor is already known
		FanetNeighbor *fn = nullptr;
		for(auto *neighbor : neighbors)
		{
			if(neighbor->addr == frm->src)
			{
				fn = neighbor;
				break;
			}
		}

		/* decode information */
		frm->decodePayload(fn);

		osMutexRelease(neighborMutex);
	}
	else
	{
		/* not directly frame specifically for us. geoforward? */

		//todo, inside areas required....
		//todo dynamic replay features....

	}
}

/*
 * remote configuration
 */

bool Fanet::writeKey(char *newKey)
{
	if(newKey == nullptr || strlen(newKey) > sizeof(_key))
		return false;

	/* copy key */
	memset(_key, 0, sizeof(_key));
	snprintf(_key, sizeof(_key), "%s", newKey);

	/* determine page */
	FLASH_EraseInitTypeDef eraseInit = {0};
	eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
	eraseInit.Banks = FLASH_BANK_1;
	eraseInit.Page = FANET_KEYADDR_PAGE;
	eraseInit.NbPages = 1;

	/* erase */
	uint32_t sectorError = 0;
	if(HAL_FLASH_Unlock() != HAL_OK)
		return false;
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
	if(HAL_FLASHEx_Erase(&eraseInit, &sectorError) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	/* write */
	for(unsigned int i=0; i<(sizeof(_key)+7)/8; i++)
	{
		/* build config */
		uint64_t addr_container = _key[i*8] | _key[i*8+1]<<8 | _key[i*8+2]<<16  | _key[i*8+3]<<24 |
				((uint64_t)_key[i*8+4])<<32 | ((uint64_t)_key[i*8+5])<<40  |
				((uint64_t)_key[i*8+6])<<48 | ((uint64_t)_key[i*8+7])<<56;

		if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_KEYADDR_BASE + i*8, addr_container) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return false;
		}
	}

	HAL_FLASH_Lock();
	return true;
}

void Fanet::loadKey(void)
{
	if(*(__IO uint64_t*)FANET_KEYADDR_BASE == UINT64_MAX)
		return;

	snprintf(_key, sizeof(_key), "%s", (char *)(__IO uint64_t*)FANET_KEYADDR_BASE);

	debug_printf("Key: '%s'\n", key);
}

bool Fanet::writePosition(Coordinate3D newPos, float newHeading)
{
	/* copy position */
	_position = newPos;
	_heading = newHeading;

	/* determine page */
	FLASH_EraseInitTypeDef eraseInit = {0};
	eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
	eraseInit.Banks = FLASH_BANK_1;
	eraseInit.Page = FANET_POSADDR_PAGE;
	eraseInit.NbPages = 1;

	/* erase */
	uint32_t sectorError = 0;
	if(HAL_FLASH_Unlock() != HAL_OK)
		return false;
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
	if(HAL_FLASHEx_Erase(&eraseInit, &sectorError) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	/* write */

	uint64_t addr_container = ((uint64_t) ((uint8_t *)&position.latitude)[0]) << 0 | ((uint64_t) ((uint8_t *)&position.latitude)[1]) << 8 |
				((uint64_t) ((uint8_t *)&position.latitude)[2]) << 16 |	((uint64_t) ((uint8_t *)&position.latitude)[3]) << 24;
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 0, addr_container) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	addr_container = ((uint64_t) ((uint8_t *)&position.longitude)[0]) << 0 | ((uint64_t) ((uint8_t *)&position.longitude)[1]) << 8 |
				((uint64_t) ((uint8_t *)&position.longitude)[2]) << 16 | ((uint64_t) ((uint8_t *)&position.longitude)[3]) << 24;
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 8, addr_container) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	addr_container = ((uint64_t) ((uint8_t *)&position.altitude)[0]) << 0 |	((uint64_t) ((uint8_t *)&position.altitude)[1]) << 8 |
				((uint64_t) ((uint8_t *)&position.altitude)[2]) << 16 |	((uint64_t) ((uint8_t *)&position.altitude)[3]) << 24;
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 16, addr_container) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	addr_container = ((uint64_t) ((uint8_t *)&heading)[0]) << 0 | ((uint64_t) ((uint8_t *)&heading)[1]) << 8 |
				((uint64_t) ((uint8_t *)&heading)[2]) << 16 | ((uint64_t) ((uint8_t *)&heading)[3]) << 24;
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 24, addr_container) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	HAL_FLASH_Lock();
	return true;
}

void Fanet::loadPosition(void)
{
	if(*(__IO uint64_t*)FANET_POSADDR_BASE == UINT64_MAX)
		return;

	memcpy(&_position.latitude, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+0), sizeof(float));
	memcpy(&_position.longitude, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+8), sizeof(float));
	memcpy(&_position.altitude, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+16), sizeof(float));
	memcpy(&_heading, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+24), sizeof(float));

	debug_printf("Loc %.4f,%.4f,%.fm,%.fdeg\n", position.latitude, position.longitude, position.altitude, heading);
}

bool Fanet::writeReplayFeatures(void)
{
	/* determine page */
	FLASH_EraseInitTypeDef eraseInit = {0};
	eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
	eraseInit.Banks = FLASH_BANK_1;
	eraseInit.Page = FANET_RPADDR_PAGE;
	eraseInit.NbPages = 1;

	/* erase */
	uint32_t sectorError = 0;
	if(HAL_FLASH_Unlock() != HAL_OK)
		return false;
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
	if(HAL_FLASHEx_Erase(&eraseInit, &sectorError) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}

	/* write */
	bool error = false;
	for(uint16_t i=0; i<NELEM(replayFeature); i++)
		error |= !replayFeature[i].write(FANET_RPADDR_BASE + i*FLASH_PAGESIZE/NELEM(replayFeature)/8*8);

	HAL_FLASH_Lock();
	return !error;
}

void Fanet::loadReplayFeatures(void)
{
	for(uint16_t i=0; i<NELEM(replayFeature); i++)
		replayFeature[i].load(FANET_RPADDR_BASE + i*FLASH_PAGESIZE/NELEM(replayFeature)/8*8);
}

void Fanet::init(FanetMac *fmac)
{
	/* read configuration */
	loadKey();
	loadPosition();
	loadReplayFeatures();
}

Fanet fanet = Fanet();

