#ifndef RC_PWM_H
#define RC_PWM_H

#include <unistd.h>

/**
 * Functions to generate pulse width modulation control signals for cheap
 * toy-grade non-proportional radio controlled cars.
 */


/**
 * Initializes the RC broadcasting. This must be called before calling
 * rcSetBroadcastPwm and after either program start or calling rcStop.
 */
int rcInitialize(void);

/**
 * Sets the control parameters.
 */
void rcSetBroadcastPwm(
	float frequency,
	float deadFrequency,
	int burstUs,
	int synchronizationBurstCount,
	int synchronizationMultiple,
	int burstCount
);

/**
 * Stops the broadcasting.
 */
void rcStop(void);

/**
 * Fills a buffer for broadcast.
 */
ssize_t rcFillBuffer(void *buffer, size_t count);

#endif  // RC_PWM_H
