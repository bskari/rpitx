#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include "RcPwm.h"

// RC control signals look like this:
//  +-----+  +-----+  +-----+  +--+  +--+  +--+  +--+  +--+
//  |     |  |     |  |     |  |  |  |  |  |  |  |  |  |  |
// -+     +--+     +--+     +--+  +--+  +--+  +--+  +--+  +--
// Synchronization phase        Signal phase
//
// All signals and gaps between signals last some integer multiple of a base
// time (baseUs), such as 250 useconds. The first part, the synchronization
// phase, sends a number of bursts (synchronizationBurstCount) that are each a
// multiple of baseUs (synchronizationMultiple), with a baseUs gap between.
// This is followed by a number of bursts (burstCount), each baseUs long with a
// baseUs gap. The number of bursts indicates the command, e.g. 55 means drive
// forward and right.
// Because we're using DMA to broadcast consistently, we can't prevent the Pi
// from broadcasting for the gaps, so instead we broadcast at an unused
// frequency (deadFrequency).
struct rcBroadcastParameters {
	float frequency;
	float deadFrequency;
	int burstUs;
	int synchronizationBurstCount;
	int synchronizationMultiple;
	int burstCount;
};

static struct rcBroadcastParameters currentParameters;
static struct rcBroadcastParameters newParameters;
static enum broadcastState {
	SYNCHRONIZATION,
	SYNCHRONIZATION_GAP,
	SIGNAL,
	SIGNAL_GAP
} state = SYNCHRONIZATION;
static int stateCount;
static int updated;
static pthread_mutex_t mutex;
static int stop;

#ifndef NDEBUG
static int initialized = 0;
#endif


int rcInitialize(void) {
	if (pthread_mutex_init(&mutex, NULL) != 0) {
		return -1;
	}
#ifndef NDEBUG
	initialized = 1;
#endif
	// Default to the middle of the FM band to minimize potential interference
	currentParameters.frequency = 100.1;
	currentParameters.deadFrequency = 100.1;
	currentParameters.burstUs = 1000;
	currentParameters.synchronizationBurstCount = 4;
	currentParameters.burstCount = 50;
	stateCount = 0;
	updated = 0;
	stop = 0;
	return 0;
}


void rcSetBroadcastPwm(
	const float frequency,
	const float deadFrequency,
	const int burstUs,
	const int synchronizationBurstCount,
	const int synchronizationMultiple,
	const int burstCount
) {
	assert(initialized && "rcInitialize not called");
	pthread_mutex_lock(&mutex);
	newParameters.frequency = frequency;
	newParameters.deadFrequency = deadFrequency;
	newParameters.burstUs = burstUs;
	newParameters.synchronizationBurstCount = synchronizationBurstCount;
	newParameters.synchronizationMultiple = synchronizationMultiple;
	newParameters.burstCount = burstCount;
	updated = 1;
	pthread_mutex_unlock(&mutex);
}


ssize_t rcFillBuffer(void *buffer, size_t count) {
	static size_t offset;

	assert(mutex != NULL && "rcInitialize not called");

	if (stop) {
		return 0;
	}

	size_t bytesWritten = 0;

	while (bytesWritten < count) {
		switch (state) {
			case SYNCHRONIZATION:
				*((double*)buffer) = currentParameters.frequency;
				buffer = (double*)buffer + 1;
				*((uint32_t*)buffer) = currentParameters.burstUs *
					currentParameters.synchronizationMultiple;
				buffer = (uint32_t*)buffer + 1;
				bytesWritten += sizeof(double) + sizeof(uint32_t);
				state = SYNCHRONIZATION_GAP;
				break;
			case SYNCHRONIZATION_GAP:
				*((double*)buffer) = currentParameters.deadFrequency;
				buffer = (double*)buffer + 1;
				*((uint32_t*)buffer) = currentParameters.burstUs;
				buffer = (uint32_t*)buffer + 1;
				bytesWritten += sizeof(double) + sizeof(uint32_t);
				++stateCount;
				if (stateCount == currentParameters.synchronizationBurstCount) {
					stateCount = 0;
					state = SIGNAL;
				} else {
					state = SYNCHRONIZATION;
				}
				break;
			case SIGNAL:
				*((double*)buffer) = currentParameters.frequency;
				buffer = (double*)buffer + 1;
				*((uint32_t*)buffer) = currentParameters.burstUs;
				buffer = (uint32_t*)buffer + 1;
				bytesWritten += sizeof(double) + sizeof(uint32_t);
				state = SIGNAL_GAP;
				break;
			case SIGNAL_GAP:
				*((double*)buffer) = currentParameters.deadFrequency;
				buffer = (double*)buffer + 1;
				*((uint32_t*)buffer) = currentParameters.burstUs;
				buffer = (uint32_t*)buffer + 1;
				bytesWritten += sizeof(double) + sizeof(uint32_t);
				++stateCount;
				if (stateCount == currentParameters.burstCount) {
					// TODO(2016-03-05) Do I need to lock the mutex before
					// checking updated?
					if (updated) {
						pthread_mutex_lock(&mutex);
						currentParameters = newParameters;
						updated = 0;
						pthread_mutex_unlock(&mutex);
					}
					stateCount = 0;
					state = SYNCHRONIZATION;
				} else {
					state = SIGNAL;
				}
				break;
			default:
				assert(!"Invalid state");
		}
	}

	return bytesWritten;
}


void rcStop(void) {
	stop = 1;
	pthread_mutex_destroy(&mutex);
#ifndef NDEBUG
	initialized = false;
#endif
}
