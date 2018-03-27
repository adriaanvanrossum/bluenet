/*
 * Author: Crownstone Team
 * Copyright: Crownstone (https://crownstone.rocks)
 * Date: Aug 4, 2016
 * License: LGPLv3+, Apache License 2.0, and/or MIT (triple-licensed)
 */

#pragma once

#include <cstdint>
#include "drivers/cs_Timer.h"

class FactoryReset {
public:
	//! Gets a static singleton (no dynamic memory allocation)
	static FactoryReset& getInstance() {
		static FactoryReset instance;
		return instance;
	}

	void init();

	/* Function to recover: no need to be admin to call this */
	bool recover(uint32_t resetCode);

	/* Function to go into factory reset mode */
	bool factoryReset(uint32_t resetCode);

	/* Function to actually wipe the memory */
	bool finishFactoryReset();

	/* Enable/disable ability to recover */
	void enableRecovery(bool enable);

	/* Static function for the timeout */
	static void staticTimeout(FactoryReset *ptr) {
		ptr->timeout();
	}

	/* Static function for the timeout */
	static void staticProcess(FactoryReset *ptr) {
		ptr->process();
	}

private:
	FactoryReset();

	inline bool validateResetCode(uint32_t resetCode);
	bool performFactoryReset();
	void resetTimeout();
	void timeout();
	void process();

	bool _recoveryEnabled;
	uint32_t _rtcStartTime;

#if (NORDIC_SDK_VERSION >= 11)
	app_timer_t              _recoveryDisableTimerData;
	app_timer_id_t           _recoveryDisableTimerId;

	app_timer_t              _recoveryProcessTimerData;
	app_timer_id_t           _recoveryProcessTimerId;
#else
	uint32_t                 _recoveryDisableTimerId;
#endif
};
