/*
 * fanet.cpp
 *
 *  Created on: Jul 4, 2018
 *      Author: sid
 */

#include "cmsis_os.h"

#include "print.h"

#include "../misc/rnd.h"
#include "../serial/serial_interface.h"
#include "sx1272.h"
#include "fanet.h"
#include "frame/fname.h"
#include "frame/fgndtracking.h"
#include "frame/ftracking.h"

osMessageQId fanet_QueueID;

/*
 *
 * TODO new can return NULL!!!! check!
 *
 *
 */

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

	osThreadYield();
	serialInt.print_line(FN_REPLYM_INITIALIZED);

	/* turn on FANET */
	fmac.setPower(true);

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
//        if (mem)
            return mem;
//        else
//            throw std::bad_alloc();
}

//void *operator new(std::size_t size, const std::nothrow_t &)
//{
//	return pvPortMalloc(size);
//}

void operator delete(void *p)
{
	vPortFree(p);
}


void Fanet::handle(void)
{
	/* broadcast name */
//	if(settings.broadcastName && nextNameBroadcast < osKernelSysTick() && strlen(pilot.name))
//	{
//		FanetFrameName *ffn = new FanetFrameName();
//		if(fmac.transmit(ffn) != 0)
//			delete ffn;

//		nextNameBroadcast = osKernelSysTick() + FANET_TYPE2_TAU_MS;
//	}

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

/* Fapp */
bool Fanet::isBroadcastReady(void)
{
	/* is the state valid? */
//	if(flightCtl.gnssVaild == false || flightCtl.player.isActive())			//todo dev mode here
		return false;

	/* in case of a busy channel, ensure that frames from the fifo get also a change */
//	if(nextTrkBroadcast > osKernelSysTick())
//		return false;

	/* determine if its time to send something (again) */
//	const int tau_add = (numNeighbors()/10 + 1) * FANET_TYPE1OR7_TAU_MS;
//	if(lastTrkBroadcast + tau_add > osKernelSysTick())
//		return false;

//	return true;
}

//note: we must return a frame here as we already advertised that we are ready
FanetFrame *Fanet::getFrame(void)
{
	return nullptr;	//TODO!!!
	/* broadcast tracking information */
	//note: filled upon serialize
//	FanetFrame *frm;
//	if(fanet.state <= FanetDef::distressCallAuto || flightCtl.getMode() != FlightControl::Flight ||
//			flightCtl.recorder->state < Recorder::AutoOn)
//		frm = new FanetFrameGndTracking();
//	else
//		frm = new FanetFrameTracking();

	/* in case of a busy channel, ensure that frames from the fifo gets also a change */
//	nextTrkBroadcast = osKernelSysTick() + FANET_TYPE1OR7_MINTAU_MS;

//	return static_cast<FanetFrame *>(frm);
}

void Fanet::handleAcked(bool ack, FanetMacAddr &addr)
{
	if(promiscuous)
		serialInt.handle_acked(ack, addr);

	ackRes = ack ? ACK : NACK;
	ackAddr = addr;							//note: address has to be set as second to prevent race conditions!
}


void Fanet::handleFrame(FanetFrame *frm)
{
	if(promiscuous)
		serialInt.handle_frame(frm);

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

/*
 * remote configuration
 */

void Fanet::writeKey(char *newKey)
{
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
	HAL_FLASH_Unlock();
	HAL_FLASHEx_Erase(&eraseInit, &sectorError);
	HAL_FLASH_Lock();

	/* write */
	HAL_FLASH_Unlock();
	for(unsigned int i=0; i<(sizeof(_key)+7)/8; i++)
	{
		/* build config */
		uint64_t addr_container = _key[i*8] | _key[i*8+1]<<8 | _key[i*8+2]<<16  | _key[i*8+3]<<24 |
				((uint64_t)_key[i*8+4])<<32 | ((uint64_t)_key[i*8+5])<<40  |
				((uint64_t)_key[i*8+6])<<48 | ((uint64_t)_key[i*8+7])<<56;

		HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_KEYADDR_BASE + i*8, addr_container);
	}
	HAL_FLASH_Lock();
}

void Fanet::loadKey(void)
{
	if(*(__IO uint64_t*)FANET_KEYADDR_BASE == UINT64_MAX)
		return;

	snprintf(_key, sizeof(_key), "%s", (char *)(__IO uint64_t*)FANET_KEYADDR_BASE);
}

void Fanet::writePosition(Coordinate2D newPos, float newHeading)
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
	HAL_FLASH_Unlock();
	HAL_FLASHEx_Erase(&eraseInit, &sectorError);
	HAL_FLASH_Lock();

	/* write */
	HAL_FLASH_Unlock();

	uint64_t addr_container = ((uint64_t) ((uint8_t *)&position.latitude)[0]) << 0 |
				((uint64_t) ((uint8_t *)&position.latitude)[1]) << 8 |
				((uint64_t) ((uint8_t *)&position.latitude)[2]) << 16 |
				((uint64_t) ((uint8_t *)&position.latitude)[3]) << 24;
	HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 0, addr_container);

	addr_container = ((uint64_t) ((uint8_t *)&position.longitude)[0]) << 0 |
				((uint64_t) ((uint8_t *)&position.longitude)[1]) << 8 |
				((uint64_t) ((uint8_t *)&position.longitude)[2]) << 16 |
				((uint64_t) ((uint8_t *)&position.longitude)[3]) << 24;
	HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 8, addr_container);

	addr_container = ((uint64_t) ((uint8_t *)&heading)[0]) << 0 |
				((uint64_t) ((uint8_t *)&heading)[1]) << 8 |
				((uint64_t) ((uint8_t *)&heading)[2]) << 16 |
				((uint64_t) ((uint8_t *)&heading)[3]) << 24;
	HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FANET_POSADDR_BASE + 16, addr_container);

}

void Fanet::loadPosition(void)
{
	if(*(__IO uint64_t*)FANET_POSADDR_BASE == UINT64_MAX)
		return;

	memcpy(&_position.latitude, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+0), sizeof(float));
	memcpy(&_position.longitude, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+8), sizeof(float));
	memcpy(&_heading, (void *)(__IO uint64_t*) (FANET_POSADDR_BASE+16), sizeof(float));
}

Fanet::Fanet() : Fapp(), position(_position), heading(_heading)
{
	/* read configuration */
	loadKey();
	loadPosition();


}

Fanet fanet = Fanet();
