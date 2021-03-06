/**
 * Author: Dominik Egger
 * Copyright: Distributed Organisms B.V. (DoBots)
 * Date: May 19, 2016
 * License: LGPLv3+
 */

#include <processing/cs_PowerSampling.h>

#include <storage/cs_Settings.h>
#include <drivers/cs_Serial.h>
#include <drivers/cs_RTC.h>
#include <protocol/cs_StateTypes.h>
#include <events/cs_EventDispatcher.h>
#include <storage/cs_State.h>
#include <processing/cs_Switch.h>
#include <third/optmed.h>

#if BUILD_MESHING == 1
#include <mesh/cs_MeshControl.h>
#endif

#include <math.h>
#include "third/SortMedian.h"

// Define test pin to enable gpio debug.
//#define TEST_PIN 20

// Define to print power samples
#define PRINT_POWER_SAMPLES

#define VOLTAGE_CHANNEL_IDX 0
#define CURRENT_CHANNEL_IDX 1

PowerSampling::PowerSampling() :
		_isInitialized(false),
		_adc(NULL),
		_powerSamplingSentDoneTimerId(NULL),
		_powerSamplesBuffer(NULL),
		_consecutivePwmOvercurrent(0),
		_lastEnergyCalculationTicks(0),
		_energyUsedmicroJoule(0),
//		_lastSwitchState(0),
		_lastSwitchOffTicks(0),
		_lastSwitchOffTicksValid(false),
		_igbtFailureDetectionStarted(false)
{
	_powerSamplingReadTimerData = { {0} };
	_powerSamplingSentDoneTimerId = &_powerSamplingReadTimerData;
	_adc = &(ADC::getInstance());
	_powerMilliWattHist = new CircularBuffer<int32_t>(POWER_SAMPLING_RMS_WINDOW_SIZE);
	_currentRmsMilliAmpHist = new CircularBuffer<int32_t>(POWER_SAMPLING_RMS_WINDOW_SIZE);
	_voltageRmsMilliVoltHist = new CircularBuffer<int32_t>(POWER_SAMPLING_RMS_WINDOW_SIZE);
	_filteredCurrentRmsHistMA = new CircularBuffer<int32_t>(POWER_SAMPLING_RMS_WINDOW_SIZE);
	_logsEnabled.asInt = 0;
}

#if POWER_SAMPLING_RMS_WINDOW_SIZE == 7
#define opt_med(arr) opt_med7(arr)
#elif POWER_SAMPLING_RMS_WINDOW_SIZE == 9
#define opt_med(arr) opt_med9(arr)
#elif POWER_SAMPLING_RMS_WINDOW_SIZE == 25
#define opt_med(arr) opt_med25(arr)
#endif

#ifdef PRINT_POWER_SAMPLES
static int printPower = 0;
#endif

// adc done callback is already decoupled from adc interrupt
void adc_done_callback(nrf_saadc_value_t* buf, uint16_t size, uint8_t bufNum) {
	PowerSampling::getInstance().powerSampleAdcDone(buf, size, bufNum);
}

void PowerSampling::init(const boards_config_t& boardConfig) {
//	memcpy(&_config, &config, sizeof(power_sampling_config_t));

	Timer::getInstance().createSingleShot(_powerSamplingSentDoneTimerId, (app_timer_timeout_handler_t)PowerSampling::staticPowerSampleRead);

	Settings& settings = Settings::getInstance();
	settings.get(CONFIG_VOLTAGE_MULTIPLIER, &_voltageMultiplier);
	settings.get(CONFIG_CURRENT_MULTIPLIER, &_currentMultiplier);
	settings.get(CONFIG_VOLTAGE_ZERO, &_voltageZero);
	settings.get(CONFIG_CURRENT_ZERO, &_currentZero);
	settings.get(CONFIG_POWER_ZERO, &_powerZero);
	settings.get(CONFIG_SOFT_FUSE_CURRENT_THRESHOLD, &_currentMilliAmpThreshold);
	settings.get(CONFIG_SOFT_FUSE_CURRENT_THRESHOLD_PWM, &_currentMilliAmpThresholdPwm);

	initAverages();
	_recalibrateZeroVoltage = true;
	_recalibrateZeroCurrent = true;
	_avgZeroVoltageDiscount = VOLTAGE_ZERO_EXP_AVG_DISCOUNT;
	_avgZeroCurrentDiscount = CURRENT_ZERO_EXP_AVG_DISCOUNT;
	_avgPowerDiscount = POWER_EXP_AVG_DISCOUNT;
	_sendingSamples = false;

	LOGi(FMT_INIT, "buffers");
	uint16_t burstSize = _powerSamples.getMaxLength();

	size_t size = burstSize;
	_powerSamplesBuffer = (buffer_ptr_t) calloc(size, sizeof(uint8_t));
	LOGd("power sample buffer=%u size=%u", _powerSamplesBuffer, size);

	_powerSamples.assign(_powerSamplesBuffer, size);
	_powerMilliWattHist->init(); // Allocates buffer
	_currentRmsMilliAmpHist->init(); // Allocates buffer
	_voltageRmsMilliVoltHist->init(); // Allocates buffer
	_filteredCurrentRmsHistMA->init(); // Allocates buffer

	// Init moving median filter
	unsigned halfWindowSize = POWER_SAMPLING_CURVE_HALF_WINDOW_SIZE;
//	unsigned halfWindowSize = 5;  // Takes 0.74ms
//	unsigned halfWindowSize = 16; // Takes 0.93ms
	unsigned windowSize = halfWindowSize * 2 + 1;
	uint16_t bufSize = CS_ADC_BUF_SIZE / 2;
	unsigned blockCount = (bufSize + halfWindowSize*2) / windowSize; // Shouldn't have a remainder!
	_filterParams = new MedianFilter(halfWindowSize, blockCount);
	_inputSamples = new PowerVector(bufSize + halfWindowSize*2);
	_outputSamples = new PowerVector(bufSize);

	LOGd(FMT_INIT, "ADC");
	adc_config_t adcConfig;
	adcConfig.channelCount = 2;
	adcConfig.channels[VOLTAGE_CHANNEL_IDX].pin = boardConfig.pinAinVoltage;
	adcConfig.channels[VOLTAGE_CHANNEL_IDX].rangeMilliVolt = boardConfig.voltageRange;
	adcConfig.channels[VOLTAGE_CHANNEL_IDX].referencePin = boardConfig.flags.hasAdcZeroRef ? boardConfig.pinAinZeroRef : CS_ADC_REF_PIN_NOT_AVAILABLE;
	adcConfig.channels[CURRENT_CHANNEL_IDX].pin = boardConfig.pinAinCurrent;
	adcConfig.channels[CURRENT_CHANNEL_IDX].rangeMilliVolt = boardConfig.currentRange;
	adcConfig.channels[CURRENT_CHANNEL_IDX].referencePin = boardConfig.flags.hasAdcZeroRef ? boardConfig.pinAinZeroRef : CS_ADC_REF_PIN_NOT_AVAILABLE;
	adcConfig.samplingPeriodUs = CS_ADC_SAMPLE_INTERVAL_US;
	_adc->init(adcConfig);

	_adc->setDoneCallback(adc_done_callback);

	// init the adc config
	_adcConfig.rangeMilliVolt[VOLTAGE_CHANNEL_IDX] = boardConfig.voltageRange;
	_adcConfig.rangeMilliVolt[CURRENT_CHANNEL_IDX] = boardConfig.currentRange;
	_adcConfig.currentPinGainHigh = boardConfig.pinAinCurrent;
	_adcConfig.currentPinGainMed  = boardConfig.pinAinCurrentGainMed;
	_adcConfig.currentPinGainLow  = boardConfig.pinAinCurrentGainLow;
	_adcConfig.voltagePin = boardConfig.pinAinVoltage;
	_adcConfig.zeroReferencePin = adcConfig.channels[CURRENT_CHANNEL_IDX].referencePin;
	_adcConfig.voltageChannelPin = _adcConfig.voltagePin;
	_adcConfig.voltageChannelUsedAs = 0;
	_adcConfig.currentDifferential = true;
	_adcConfig.voltageDifferential = true;

	EventDispatcher::getInstance().addListener(this);

#ifdef TEST_PIN
	nrf_gpio_cfg_output(TEST_PIN);
#endif

	_isInitialized = true;
}

void PowerSampling::startSampling() {
	LOGi(FMT_START, "power sample");
	// Get operation mode
	State::getInstance().get(STATE_OPERATION_MODE, _operationMode);

	EventDispatcher::getInstance().dispatch(EVT_POWER_SAMPLES_START);
	_powerSamples.clear();

	_adc->start();
}

void PowerSampling::stopSampling() {
	// todo:
}

void PowerSampling::sentDone() {
	_sendingSamples = false;
}

void PowerSampling::enableZeroCrossingInterrupt(ps_zero_crossing_cb_t callback) {
	_adc->setZeroCrossingCallback(callback);
	// Simply use the zero from the board config, that should be accurate enough for this purpose.
	_adc->enableZeroCrossingInterrupt(VOLTAGE_CHANNEL_IDX, _voltageZero);
}

void PowerSampling::handleEvent(uint16_t evt, void* p_data, uint16_t length) {
	switch (evt) {
	case EVT_TOGGLE_LOG_POWER:
		_logsEnabled.flags.power = !_logsEnabled.flags.power;
		break;
	case EVT_TOGGLE_LOG_CURRENT:
		_logsEnabled.flags.current = !_logsEnabled.flags.current;
		break;
	case EVT_TOGGLE_LOG_VOLTAGE:
		_logsEnabled.flags.voltage = !_logsEnabled.flags.voltage;
		break;
	case EVT_TOGGLE_LOG_FILTERED_CURRENT:
		_logsEnabled.flags.filteredCurrent = !_logsEnabled.flags.filteredCurrent;
		break;
	case EVT_TOGGLE_ADC_VOLTAGE_VDD_REFERENCE_PIN:
		toggleVoltageChannelInput();
		break;
	case EVT_TOGGLE_ADC_DIFFERENTIAL_CURRENT:
		toggleDifferentialModeCurrent();
		break;
	case EVT_TOGGLE_ADC_DIFFERENTIAL_VOLTAGE:
		toggleDifferentialModeVoltage();
		break;
	case EVT_INC_VOLTAGE_RANGE:
		changeRange(VOLTAGE_CHANNEL_IDX, 600);
		break;
	case EVT_DEC_VOLTAGE_RANGE:
		changeRange(VOLTAGE_CHANNEL_IDX, -600);
		break;
	case EVT_INC_CURRENT_RANGE:
		changeRange(CURRENT_CHANNEL_IDX, 600);
		break;
	case EVT_DEC_CURRENT_RANGE:
		changeRange(CURRENT_CHANNEL_IDX, -600);
		break;
	}
}

/**
 * After ADC has finished, calculate power consumption, copy data if required, and release buffer.
 *
 * Only when in normal operation mode (e.g. not in setup mode) sent the information to a BLE characteristic.
 */
void PowerSampling::powerSampleAdcDone(nrf_saadc_value_t* buf, uint16_t size, uint8_t bufNum) {
#ifdef TEST_PIN
	nrf_gpio_pin_toggle(TEST_PIN);
#endif
	power_t power;
	power.buf = buf;
	power.bufSize = size;
	power.voltageIndex = VOLTAGE_CHANNEL_IDX;
	power.currentIndex = CURRENT_CHANNEL_IDX;
	power.numChannels = 2;
	power.sampleIntervalUs = CS_ADC_SAMPLE_INTERVAL_US;
	power.acPeriodUs = 20000;

	filter(power);
#ifdef TEST_PIN
	nrf_gpio_pin_toggle(TEST_PIN);
#endif

	if (_recalibrateZeroVoltage) {
		calculateVoltageZero(power);
//		_recalibrateZeroVoltage = false;
	}
	if (_recalibrateZeroCurrent) {
		calculateCurrentZero(power);
//		_recalibrateZeroCurrent = false;
	}

#ifdef TEST_PIN
	nrf_gpio_pin_toggle(TEST_PIN);
#endif

	calculatePower(power);
	calculateEnergy();

	if (_operationMode == OPERATION_MODE_NORMAL) {
		if (!_sendingSamples) {
			copyBufferToPowerSamples(power);
		}
		// TODO: use State.set() for this.
		EventDispatcher::getInstance().dispatch(STATE_POWER_USAGE, &_avgPowerMilliWatt, sizeof(_avgPowerMilliWatt));

		EventDispatcher::getInstance().dispatch(STATE_ACCUMULATED_ENERGY, &_energyUsedmicroJoule, sizeof(_energyUsedmicroJoule));
	}

#ifdef TEST_PIN
	nrf_gpio_pin_toggle(TEST_PIN);
#endif
	_adc->releaseBuffer(buf);
}

void PowerSampling::getBuffer(buffer_ptr_t& buffer, uint16_t& size) {
	_powerSamples.getBuffer(buffer, size);
}

/**
 * Fill the number of samples in the characteristic.
 */
void PowerSampling::copyBufferToPowerSamples(power_t power) {
	// First clear the old samples
	// Dispatch event that samples are will be cleared
	EventDispatcher::getInstance().dispatch(EVT_POWER_SAMPLES_START);
	_powerSamples.clear();
	// Use dummy timestamps for now, for backward compatibility
	uint32_t startTime = RTC::getCount(); // Not really the start time
	uint32_t dT = CS_ADC_SAMPLE_INTERVAL_US * RTC_CLOCK_FREQ / (NRF_RTC0->PRESCALER + 1) / 1000 / 1000;

	for (int i = 0; i < power.bufSize; i += power.numChannels) {
		if (_powerSamples.getCurrentSamplesBuffer()->full() || _powerSamples.getVoltageSamplesBuffer()->full()) {
			readyToSendPowerSamples();
			return;
		}
		_powerSamples.getCurrentSamplesBuffer()->push(power.buf[i+power.currentIndex]);
		_powerSamples.getVoltageSamplesBuffer()->push(power.buf[i+power.voltageIndex]);
		if ((!_powerSamples.getCurrentTimestampsBuffer()->push(startTime + (i/power.numChannels) * dT)) || 
				(!_powerSamples.getVoltageTimestampsBuffer()->push(startTime + (i/power.numChannels) * dT))) {
			_powerSamples.getCurrentSamplesBuffer()->clear();
			_powerSamples.getVoltageSamplesBuffer()->clear();
			return;
		}
	}
	//! TODO: are we actually ready here?
	readyToSendPowerSamples();
}

void PowerSampling::readyToSendPowerSamples() {
	//! Mark that the power samples are being sent now
	_sendingSamples = true;
	//! Dispatch event that samples are now filled and ready to be sent
	EventDispatcher::getInstance().dispatch(EVT_POWER_SAMPLES_END, _powerSamplesBuffer, _powerSamples.getDataLength());
	//! Simply use an amount of time for sending, should be event based or polling based
	Timer::getInstance().start(_powerSamplingSentDoneTimerId, MS_TO_TICKS(3000), this);
}

void PowerSampling::initAverages() {
	_avgZeroVoltage = _voltageZero * 1000;
	_avgZeroCurrent = _currentZero * 1000;
	_avgPower = 0.0;
}

/**
 * This just returns the given currentIndex. 
 */
uint16_t PowerSampling::determineCurrentIndex(power_t power) {
	return power.currentIndex;
}

/**
 * The voltage curve is a distorted sinusoid. We calculate the zero(-crossing) by averaging over the buffer over 
 * exactly one cycle (positive and negative) of the sinusoid. The cycle does not start at a particular known phase.
 *
 * @param buf                                    Series of samples for voltage and current (we skip every numChannel).
 * @param bufSize                                Not used.
 * @param numChannels                            By default 2 channels, voltage and current.
 * @param voltageIndex                           Offset into the array.
 * @param currentIndex                           Offset into the array for current (irrelevant).
 * @param sampleIntervalUs                       CS_ADC_SAMPLE_INTERVAL_US (default 200).
 * @param acPeriodUs                             20000 (at 50Hz this is 20.000 microseconds, this means: 100 samples).
 *
 * We iterate over the buffer. The number of samples within a single buffer (either voltage or current) depends on the
 * period in microseconds and the sample interval also in microseconds.
 */
void PowerSampling::calculateVoltageZero(power_t power) {
	uint16_t numSamples = power.acPeriodUs / power.sampleIntervalUs; 

	int64_t sum = 0;
	for (int i = power.voltageIndex; i < numSamples * power.numChannels; i += power.numChannels) {
		sum += power.buf[i];
	}
	int32_t zeroVoltage = sum * 1000 / numSamples;
//	_avgZeroVoltage = zeroVoltage;
	
	// Exponential moving average
	int64_t avgZeroVoltageDiscount = _avgZeroVoltageDiscount; // Make sure calculations are in int64_t
	_avgZeroVoltage = ((1000 - avgZeroVoltageDiscount) * _avgZeroVoltage + avgZeroVoltageDiscount * zeroVoltage) / 1000;
}

/**
 * The same as for the voltage curve, but for the current.
 */
void PowerSampling::calculateCurrentZero(power_t power) {
	uint16_t numSamples = power.acPeriodUs / power.sampleIntervalUs; 

	int64_t sum = 0;
//	for (int i = power.currentIndex; i < numSamples * power.numChannels; i += power.numChannels) {
//		sum += power.buf[i];
//	}
	// Use filtered samples to calculate the zero.
	for (int i = 0; i < numSamples; ++i) {
		sum += _outputSamples->at(i);
	}
	int32_t zeroCurrent = sum * 1000 / numSamples;
//	_avgZeroCurrent = zeroCurrent;

	// Exponential moving average
	int64_t avgZeroCurrentDiscount = _avgZeroCurrentDiscount; // Make sure calculations are in int64_t
	_avgZeroCurrent = ((1000 - avgZeroCurrentDiscount) * _avgZeroCurrent + avgZeroCurrentDiscount * zeroCurrent) / 1000;
}

void PowerSampling::filter(power_t power) {
	uint16_t bufSize = power.bufSize / power.numChannels;
//
//	// Parameters
//	unsigned halfWindowSize = 5;  // Takes 0.74ms
//	unsigned halfWindowSize = 16; // Takes 0.93ms
//	unsigned windowSize = halfWindowSize * 2 + 1;
//	unsigned blockCount = (bufSize + halfWindowSize*2) / windowSize; // Shouldn't have a remainder!
//	MedianFilter filterParams(halfWindowSize, blockCount);
//
//	Vector samples(bufSize + halfWindowSize*2);
//	Vector filtered(bufSize);


	// Pad the start of the input vector with the first sample in the buffer
	uint16_t j = 0;
	for (; j<_filterParams->half; ++j) {
		_inputSamples->at(j) = power.buf[power.currentIndex];
	}
	// Copy samples from buffer to input vector
	uint16_t i = power.currentIndex;
	for (; i<power.bufSize; i += power.numChannels) {
		_inputSamples->at(j) = power.buf[i];
		++j;
	}
	// Pad the end of the buffer with the last sample in the buffer
	for (; j<bufSize+_filterParams->half; ++j) {
		_inputSamples->at(j) = power.buf[i];
	}

	// Filter the data
	sort_median(*_filterParams, *_inputSamples, *_outputSamples);
}


/**
 * Calculate power.
 *
 * The int64_t sum is large enough: 2^63 / (2^12 * 1000 * 2^12 * 1000) = 5*10^5. Many more samples than the 100 we use.
 */
void PowerSampling::calculatePower(power_t power) {

	uint16_t numSamples = power.acPeriodUs / power.sampleIntervalUs; 

	if ((int)power.bufSize < numSamples * power.numChannels) {
		LOGe("Should have at least a whole period in a buffer!");
		return;
	}

	//////////////////////////////////////////////////
	// Calculatate power, Irms, and Vrms
	//////////////////////////////////////////////////

	int64_t pSum = 0;
	int64_t cSquareSum = 0;
	int64_t vSquareSum = 0;
	int64_t current;
	int64_t voltage;
	for (uint16_t i = 0; i < numSamples * power.numChannels; i += power.numChannels) {
		current = (int64_t)power.buf[i+power.currentIndex]*1000 - _avgZeroCurrent;
		voltage = (int64_t)power.buf[i+power.voltageIndex]*1000 - _avgZeroVoltage;
		cSquareSum += (current * current) / (1000*1000);
		vSquareSum += (voltage * voltage) / (1000*1000);
		pSum +=       (current * voltage) / (1000*1000);
	}
	int32_t powerMilliWatt = pSum * _currentMultiplier * _voltageMultiplier * 1000 / numSamples - _powerZero;
	int32_t currentRmsMA =  sqrt((double)cSquareSum * _currentMultiplier * _currentMultiplier / numSamples) * 1000;
	int32_t voltageRmsMilliVolt = sqrt((double)vSquareSum * _voltageMultiplier * _voltageMultiplier / numSamples) * 1000;



	////////////////////////////////////////////////////////////////////////////////
	// Calculate Irms of median filtered samples, and filter over multiple periods
	////////////////////////////////////////////////////////////////////////////////

	// Calculate Irms again, but now with the filtered current samples
	cSquareSum = 0;
	for (uint16_t i=0; i<numSamples; ++i) {
		current = (int64_t)_outputSamples->at(i)*1000 - _avgZeroCurrent;
		cSquareSum += (current * current) / (1000*1000);
	}
	int32_t filteredCurrentRmsMA =  sqrt((double)cSquareSum * _currentMultiplier * _currentMultiplier / numSamples) * 1000;

	// Calculate median when there are enough values in history, else calculate the average.
	_filteredCurrentRmsHistMA->push(filteredCurrentRmsMA);
	int32_t filteredCurrentRmsMedianMA;
	if (_filteredCurrentRmsHistMA->full()) {
		memcpy(_histCopy, _filteredCurrentRmsHistMA->getBuffer(), _filteredCurrentRmsHistMA->getMaxByteSize());
		filteredCurrentRmsMedianMA = opt_med(_histCopy);
	}
	else {
		int64_t currentRmsSumMA = 0;
		for (uint16_t i=0; i<_filteredCurrentRmsHistMA->size(); ++i) {
			currentRmsSumMA += _filteredCurrentRmsHistMA->operator [](i);
		}
		filteredCurrentRmsMedianMA = currentRmsSumMA / _filteredCurrentRmsHistMA->size();
	}

	// Now that Irms is known: first check the soft fuse.
	checkSoftfuse(filteredCurrentRmsMedianMA, filteredCurrentRmsMedianMA);



	/////////////////////////////////////////////////////////
	// Filter Irms, Vrms, and Power (over multiple periods)
	/////////////////////////////////////////////////////////

	// Calculate median when there are enough values in history, else calculate the average.
	_currentRmsMilliAmpHist->push(currentRmsMA);
	int32_t currentRmsMedianMA;
	if (_currentRmsMilliAmpHist->full()) {
		memcpy(_histCopy, _currentRmsMilliAmpHist->getBuffer(), _currentRmsMilliAmpHist->getMaxByteSize());
		currentRmsMedianMA = opt_med(_histCopy);
	}
	else {
		int64_t currentRmsMilliAmpSum = 0;
		for (uint16_t i=0; i<_currentRmsMilliAmpHist->size(); ++i) {
			currentRmsMilliAmpSum += _currentRmsMilliAmpHist->operator [](i);
		}
		currentRmsMedianMA = currentRmsMilliAmpSum / _currentRmsMilliAmpHist->size();
	}

//	// Exponential moving average of the median
//	int64_t discountCurrent = 200;
//	_avgCurrentRmsMilliAmp = ((1000-discountCurrent) * _avgCurrentRmsMilliAmp + discountCurrent * medianCurrentRmsMilliAmp) / 1000;
	// Use median as average
	_avgCurrentRmsMilliAmp = currentRmsMedianMA;

	// Calculate median when there are enough values in history, else calculate the average.
	_voltageRmsMilliVoltHist->push(voltageRmsMilliVolt);
	if (_voltageRmsMilliVoltHist->full()) {
		memcpy(_histCopy, _voltageRmsMilliVoltHist->getBuffer(), _voltageRmsMilliVoltHist->getMaxByteSize());
		_avgVoltageRmsMilliVolt = opt_med(_histCopy);
	}
	else {
		int64_t voltageRmsMilliVoltSum = 0;
		for (uint16_t i=0; i<_voltageRmsMilliVoltHist->size(); ++i) {
			voltageRmsMilliVoltSum += _voltageRmsMilliVoltHist->operator [](i);
		}
		_avgVoltageRmsMilliVolt = voltageRmsMilliVoltSum / _voltageRmsMilliVoltHist->size();
	}

	// Calculate apparent power: current_rms * voltage_rms
	__attribute__((unused)) uint32_t powerMilliWattApparent = (int64_t)_avgCurrentRmsMilliAmp * _avgVoltageRmsMilliVolt / 1000;

//	// Calculate median when there are enough values in history, else calculate the average.
//	_powerMilliWattHist->push(powerMilliWatt);
//	if (_powerMilliWattHist->full()) {
//		memcpy(_histCopy, _powerMilliWattHist->getBuffer(), _powerMilliWattHist->getMaxByteSize());
//		_avgPowerMilliWatt = opt_med(_histCopy);
//	}
//	else {
//		int64_t powerMilliWattSum = 0;
//		for (uint16_t i=0; i<_powerMilliWattHist->size(); ++i) {
//			powerMilliWattSum += _powerMilliWattHist->operator [](i);
//		}
//		_avgPowerMilliWatt = powerMilliWattSum / _powerMilliWattHist->size();
//	}

	// Exponential moving average
	int64_t avgPowerDiscount = _avgPowerDiscount;
	_avgPowerMilliWatt = ((1000-avgPowerDiscount) * _avgPowerMilliWatt + avgPowerDiscount * powerMilliWatt) / 1000;
//	_avgPowerMilliWatt = powerMilliWatt;



	/////////////////////////////////////////////////////////
	// Debug prints
	/////////////////////////////////////////////////////////

#ifdef PRINT_POWER_SAMPLES
	if (printPower % 500 == 0) {
//	if (printPower % 500 == 0 || currentRmsMedianMA > _currentMilliAmpThresholdPwm || currentRmsMA > _currentMilliAmpThresholdPwm) {

		if (_logsEnabled.flags.power) {
			// Calculated values
			write("Calc: ");
//			write("%i %i ", currentRmsMilliAmp, _avgCurrentRmsMilliAmp);
//			write("%i %i ", voltageRmsMilliVolt, _avgVoltageRmsMilliVolt);
//			write("%i %i ", powerMilliWatt, _avgPowerMilliWatt);
			write("I=%i I_med=%i filt_I=%i filt_I_med=%i ", currentRmsMA, currentRmsMedianMA, filteredCurrentRmsMA, filteredCurrentRmsMedianMA);
			write("vZero=%i cZero=%i ", _avgZeroVoltage, _avgZeroCurrent);
//			write("pSum=%lld ", pSum);
			write("apparent=%u ", powerMilliWattApparent);
			write("power=%d avg=%d ",powerMilliWatt, _avgPowerMilliWatt);
			write("\r\n");
		}

		if (_logsEnabled.flags.current) {
			// Current wave
			write("Current: ");
//			write("\r\n");
			for (int i = power.currentIndex; i < numSamples * power.numChannels; i += power.numChannels) {
				write("%d ", power.buf[i]);
				if (i % 40 == 40 - 1) {
//					write("\r\n");
				}
			}
			write("\r\n");
		}

		if (_logsEnabled.flags.filteredCurrent) {
			// Filtered current wave
			write("Filtered: ");
//			write("\r\n");
			for (int i = 0; i < numSamples; ++i) {
				write("%d ", _outputSamples->at(i));
				if (i % 20 == 20 - 1) {
//					write("\r\n");
				}
			}
			write("\r\n");
		}

		if (_logsEnabled.flags.voltage) {
			// Voltage wave
			write("Voltage: ");
//			write("\r\n");
			for (int i = power.voltageIndex; i < numSamples * power.numChannels; i += power.numChannels) {
				write("%d ", power.buf[i]);
				if (i % 40 == 40 - 2) {
//					write("\r\n");
				}
			}
			write("\r\n");
		}
	}
	++printPower;
#endif
}

void PowerSampling::calculateEnergy() {
	uint32_t rtcCount = RTC::getCount();
	uint32_t diffTicks = RTC::difference(rtcCount, _lastEnergyCalculationTicks);
	// TODO: using ms introduces more error (due to rounding to ms), maybe use ticks directly?
//	_energyUsedmicroJoule += (int64_t)_avgPowerMilliWatt * RTC::ticksToMs(diffTicks);
//	_energyUsedmicroJoule += (int64_t)_avgPowerMilliWatt * diffTicks * (NRF_RTC0->PRESCALER + 1) * 1000 / RTC_CLOCK_FREQ;

	// In order to keep more precision: multiply ticks by some number, then divide the result by the same number.
	_energyUsedmicroJoule += (int64_t)_avgPowerMilliWatt * RTC::ticksToMs(1024*diffTicks) / 1024;
	_lastEnergyCalculationTicks = rtcCount;
}

void PowerSampling::checkSoftfuse(int32_t currentRmsMA, int32_t currentRmsFilteredMA) {
	//! Get the current state errors
	state_errors_t stateErrors;
	State::getInstance().get(STATE_ERRORS, stateErrors.asInt);

	// Get the current switch state before we dispatch any event (as that may change the switch).
	switch_state_t switchState;

	// TODO: implement this differently
	if (RTC::getCount() > RTC::msToTicks(2000)) {
		startIgbtFailureDetection();
	}


	// ---------- TODO: this should be kept up in the state ---------
	switch_state_t prevSwitchState = _lastSwitchState;
	State::getInstance().get(STATE_SWITCH_STATE, &switchState, sizeof(switch_state_t));
	_lastSwitchState = switchState;

	if (switchState.relay_state == 0 && switchState.pwm_state == 0 && (prevSwitchState.relay_state || prevSwitchState.pwm_state)) {
		// switch has been turned off
		_lastSwitchOffTicksValid = true;
		_lastSwitchOffTicks = RTC::getCount();
	}

	bool justSwitchedOff = false;
	if (_lastSwitchOffTicksValid) {
		uint32_t tickDiff = RTC::difference(RTC::getCount(), _lastSwitchOffTicks);
//		write("%u\r\n", tickDiff);
		if (tickDiff < RTC::msToTicks(1000)) {
			justSwitchedOff = true;
		}
		else {
			// Timed out
			_lastSwitchOffTicksValid = false;
		}
	}
	// ---------------------- end of to do --------------------------


	// Check if the filtered Irms is above threshold.
	if ((currentRmsFilteredMA > _currentMilliAmpThreshold) && (!stateErrors.errors.overCurrent)) {
		LOGw("current above threshold");
		EventDispatcher::getInstance().dispatch(EVT_CURRENT_USAGE_ABOVE_THRESHOLD);
		State::getInstance().set(STATE_ERROR_OVER_CURRENT, (uint8_t)1);
		return;
	}

	// When the dimmer is on: check if the filtered Irms is above threshold, or if the unfiltered Irms is way above threshold.
//	if ((currentRmsFilteredMA > _currentMilliAmpThresholdPwm || currentRmsMA > (int32_t)_currentMilliAmpThresholdPwm*5) && (!stateErrors.errors.overCurrentPwm)) {
//	if ((currentRmsFilteredMA > _currentMilliAmpThresholdPwm) && (!stateErrors.errors.overCurrentPwm)) {
	if (currentRmsMA > _currentMilliAmpThresholdPwm) {
		++_consecutivePwmOvercurrent;
	}
	else {
		_consecutivePwmOvercurrent = 0;
	}
	if ((_consecutivePwmOvercurrent > 20) && (!stateErrors.errors.overCurrentPwm)) {
		// Get the current pwm state before we dispatch the event (as that may change the pwm).
		switch_state_t switchState;
		State::getInstance().get(STATE_SWITCH_STATE, &switchState, sizeof(switch_state_t));
		if (switchState.pwm_state != 0) {
			// If the pwm was on:
			LOGw("current above pwm threshold");
			// Dispatch the event that will turn off the pwm
			EventDispatcher::getInstance().dispatch(EVT_CURRENT_USAGE_ABOVE_THRESHOLD_PWM);
			// Set overcurrent error.
			State::getInstance().set(STATE_ERROR_OVER_CURRENT_PWM, (uint8_t)1);
		}
		else if (switchState.relay_state == 0 && !justSwitchedOff && _igbtFailureDetectionStarted) {
			// If there is current flowing, but relay and dimmer are both off, then the dimmer is probably broken.
			LOGe("IGBT failure detected");
			EventDispatcher::getInstance().dispatch(EVT_DIMMER_ON_FAILURE_DETECTED);
			State::getInstance().set(STATE_ERROR_DIMMER_ON_FAILURE, (uint8_t)1);
		}
	}
}

void PowerSampling::startIgbtFailureDetection() {
	_igbtFailureDetectionStarted = true;
}

void PowerSampling::toggleVoltageChannelInput() {
	_adcConfig.voltageChannelUsedAs = (_adcConfig.voltageChannelUsedAs + 1) % 5;

	switch (_adcConfig.voltageChannelUsedAs) {
	case 0: // voltage pin
		_adcConfig.voltageChannelPin = _adcConfig.voltagePin;
		break;
	case 1: // zero reference pin
		if (_adcConfig.zeroReferencePin == CS_ADC_REF_PIN_NOT_AVAILABLE) {
			// Skip this pin
			_adcConfig.voltageChannelUsedAs = (_adcConfig.voltageChannelUsedAs + 1) % 5;
			// No break here.
		}
		else {
			_adcConfig.voltageChannelPin = _adcConfig.zeroReferencePin;
			break;
		}
	case 2: // VDD
		_adcConfig.voltageChannelPin = CS_ADC_PIN_VDD;
		break;
	case 3: // Current med gain
		_adcConfig.voltageChannelPin  = _adcConfig.currentPinGainMed;
		break;
	case 4: // Current low gain
		_adcConfig.voltageChannelPin  = _adcConfig.currentPinGainLow;
		break;
	}

	adc_channel_config_t channelConfig;
	channelConfig.pin = _adcConfig.voltageChannelPin;
	channelConfig.rangeMilliVolt = _adcConfig.rangeMilliVolt[VOLTAGE_CHANNEL_IDX];
	channelConfig.referencePin = _adcConfig.voltageDifferential ? _adcConfig.zeroReferencePin : CS_ADC_REF_PIN_NOT_AVAILABLE;
	_adc->changeChannel(VOLTAGE_CHANNEL_IDX, channelConfig);
}

void PowerSampling::toggleDifferentialModeCurrent() {
	_adcConfig.currentDifferential = !_adcConfig.currentDifferential;
	adc_channel_config_t channelConfig;
	channelConfig.pin = _adcConfig.currentPinGainHigh;
	channelConfig.rangeMilliVolt = _adcConfig.rangeMilliVolt[CURRENT_CHANNEL_IDX];
	channelConfig.referencePin = _adcConfig.currentDifferential ? _adcConfig.zeroReferencePin : CS_ADC_REF_PIN_NOT_AVAILABLE;
	_adc->changeChannel(CURRENT_CHANNEL_IDX, channelConfig);
}

void PowerSampling::toggleDifferentialModeVoltage() {
	_adcConfig.voltageDifferential = !_adcConfig.voltageDifferential;
	adc_channel_config_t channelConfig;
	channelConfig.pin = _adcConfig.voltageChannelPin;
	channelConfig.rangeMilliVolt = _adcConfig.rangeMilliVolt[VOLTAGE_CHANNEL_IDX];
	channelConfig.referencePin = _adcConfig.voltageDifferential ? _adcConfig.zeroReferencePin : CS_ADC_REF_PIN_NOT_AVAILABLE;
	_adc->changeChannel(VOLTAGE_CHANNEL_IDX, channelConfig);
}

void PowerSampling::changeRange(uint8_t channel, int32_t amount) {
	_adcConfig.rangeMilliVolt[channel] += amount;
	if (_adcConfig.rangeMilliVolt[channel] < 150 || _adcConfig.rangeMilliVolt[channel] > 3600) {
		_adcConfig.rangeMilliVolt[channel] -= amount;
		return;
	}

	adc_channel_config_t channelConfig;
	if (channel == VOLTAGE_CHANNEL_IDX) {
		channelConfig.pin = _adcConfig.voltageChannelPin;
		channelConfig.referencePin = _adcConfig.voltageDifferential ? _adcConfig.zeroReferencePin : CS_ADC_REF_PIN_NOT_AVAILABLE;
	}
	else {
		channelConfig.pin = _adcConfig.currentPinGainHigh;
		channelConfig.referencePin = _adcConfig.currentDifferential ? _adcConfig.zeroReferencePin : CS_ADC_REF_PIN_NOT_AVAILABLE;
	}
	channelConfig.rangeMilliVolt = _adcConfig.rangeMilliVolt[channel];
	_adc->changeChannel(channel, channelConfig);
}
