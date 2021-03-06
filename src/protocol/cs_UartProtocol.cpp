/**
 * Author: Crownstone Team
 * Copyright: Crownstone
 * Date: Jan 17, 2018
 * License: LGPLv3+, Apache, or MIT, your choice
 */

/**********************************************************************************************************************
 *
 * The Crownstone is a high-voltage (domestic) switch. It can be used for:
 *   - indoor localization
 *   - building automation
 *
 * It is one of the first, or the first(?), open-source Internet-of-Things devices entering the market.
 *
 * Read more on: https://crownstone.rocks
 *
 * Almost all configuration options should be set in CMakeBuild.config.
 *
 *********************************************************************************************************************/

#include "protocol/cs_UartProtocol.h"
#include "util/cs_Utils.h"
#include "protocol/cs_ErrorCodes.h"
#include "util/cs_BleError.h"
#include "ble/cs_Nordic.h"

#include "structs/cs_StreamBuffer.h"
#include "processing/cs_CommandHandler.h"


// Define both test pin to enable gpio debug.
//#define TEST_PIN   22
//#define TEST_PIN2  23

UartProtocol::UartProtocol():
readBuffer(NULL),
readBufferIdx(0),
startedReading(false),
escapeNextByte(false),
readPacketSize(0),
readBusy(false)
{

}

void handle_msg(void * data, uint16_t size) {
	UartProtocol::getInstance().handleMsg(data, size);
}

void UartProtocol::init() {
	readBuffer = new uint8_t[UART_RX_BUFFER_SIZE];
#ifdef TEST_PIN
    nrf_gpio_cfg_output(TEST_PIN);
#endif
#ifdef TEST_PIN2
    nrf_gpio_cfg_output(TEST_PIN2);
#endif
}

void UartProtocol::reset() {
//	write("r\r\n");
//	BLEutil::printArray(readBuffer, readBufferIdx);
#ifdef TEST_PIN2
	nrf_gpio_pin_toggle(TEST_PIN2);
#endif
	readBufferIdx = 0;
	startedReading = false;
	escapeNextByte = false;
	readPacketSize = 0;
}

void UartProtocol::escape(uint8_t& val) {
	val ^= UART_ESCAPE_FLIP_MASK;
}

void UartProtocol::unEscape(uint8_t& val) {
	val ^= UART_ESCAPE_FLIP_MASK;
}


uint16_t UartProtocol::crc16(const uint8_t * data, uint16_t size) {
	return crc16_compute(data, size, NULL);
}

void UartProtocol::crc16(const uint8_t * data, const uint16_t size, uint16_t& crc) {
	crc = crc16_compute(data, size, &crc);
}

void UartProtocol::writeMsg(UartOpcodeTx opCode, uint8_t * data, uint16_t size) {
	if (size > UART_RX_MAX_PAYLOAD_SIZE) {
		LOGw("msg too large");
		return;
	}

	uart_msg_header_t header;
	header.opCode = opCode;
	header.size = size;

	uint16_t crc = crc16((uint8_t*)(&header), sizeof(uart_msg_header_t));
	crc16(data, size, crc);
	uart_msg_tail_t tail;
	tail.crc = crc;

	LOGd("write opcode=%u len=%u crc=%u", opCode, size, crc);
	BLEutil::printArray(data, size);

	writeStartByte();
	writeBytes((uint8_t*)(&header), sizeof(uart_msg_header_t));
	writeBytes(data, size);
	writeBytes((uint8_t*)(&tail), sizeof(uart_msg_tail_t));
	write(SERIAL_CRLF); // Just so it still looks ok on minicom
}

void UartProtocol::onRead(uint8_t val) {
	// CRC error? Reset.
	// Start char? Reset.
	// Bad escaped value? Reset.
	// Bad length? Reset. Over-run length of buffer? Reset.
	// Haven't seen a start char in too long? Reset anyway.

	// Can't read anything while processing the previous msg.
	if (readBusy) {
#ifdef TEST_PIN2
		nrf_gpio_pin_toggle(TEST_PIN2);
#endif
		return;
	}

#ifdef TEST_PIN
	nrf_gpio_pin_toggle(TEST_PIN);
#endif

	// An escape shouldn't be followed by a special byte
	if (escapeNextByte) {
		switch (val) {
		case UART_START_BYTE:
		case UART_ESCAPE_BYTE:
			reset();
			return;
		}
	}

	if (val == UART_START_BYTE) {
		reset();
		startedReading = true;
		return;
	}

	if (!startedReading) {
		return;
	}


	if (val == UART_ESCAPE_BYTE) {
		escapeNextByte = true;
		return;
	}

	if (escapeNextByte) {
		unEscape(val);
		escapeNextByte = false;
	}

//#ifdef TEST_PIN
//	nrf_gpio_pin_toggle(TEST_PIN);
//#endif

	readBuffer[readBufferIdx++] = val;

	if (readBufferIdx == sizeof(uart_msg_header_t)) {
		// First check received size, then add header and tail size.
		// Otherwise an overflow would lead to passing the size check.
		readPacketSize = ((uart_msg_header_t*)readBuffer)->size;
		if (readPacketSize > UART_RX_MAX_PAYLOAD_SIZE) {
			reset();
			return;
		}
		readPacketSize += sizeof(uart_msg_header_t) + sizeof(uart_msg_tail_t);
	}

	if (readBufferIdx >= sizeof(uart_msg_header_t)) {
		// Header was read.
		if (readBufferIdx >= readPacketSize) {
			readBusy = true;
			// Decouple callback from interrupt handler, and put it on app scheduler instead
			uint32_t errorCode = app_sched_event_put(readBuffer, readBufferIdx, handle_msg);
			APP_ERROR_CHECK(errorCode);
		}
	}
}


void UartProtocol::handleMsg(void * data, uint16_t size) {
	LOGd("read:");
	BLEutil::printArray(data, size);

	// Check CRC
	uint16_t calculatedCrc = crc16((uint8_t*)data, size - sizeof(uart_msg_tail_t));
	uint16_t receivedCrc = *((uint16_t*)((uint8_t*)data + size - sizeof(uart_msg_tail_t)));
	if (calculatedCrc != receivedCrc) {
		LOGw("crc mismatch: %u vs %u", calculatedCrc, receivedCrc);
		readBusy = false;
		reset();
		return;
	}

	uart_msg_header_t* header = (uart_msg_header_t*)data;
	uint8_t* payload = (uint8_t*)data + sizeof(uart_msg_header_t);

	switch (header->opCode) {
	case UART_OPCODE_RX_CONTROL: {
		stream_header_t* streamHeader = (stream_header_t*)payload;
		if (header->size - sizeof(stream_header_t) < streamHeader->length) {
			LOGw(STR_ERR_BUFFER_NOT_LARGE_ENOUGH);
			break;
		}
		uint8_t* streamPayload = (uint8_t*)payload + sizeof(stream_header_t);
		CommandHandler::getInstance().handleCommand((CommandHandlerTypes)streamHeader->type, streamPayload, streamHeader->length);
		break;
	}
	}


	// When done, ALWAYS reset and set readBusy to false!
	// Reset invalidates the data, right?
	readBusy = false;
	reset();
	return;


//	uint16_t event = 0;
//	switch (readByte) {
//	case 42: // *
//		event = EVT_INC_VOLTAGE_RANGE;
//		break;
//	case 47: // /
//		event = EVT_DEC_VOLTAGE_RANGE;
//		break;
//	case 43: // +
//		event = EVT_INC_CURRENT_RANGE;
//		break;
//	case 45: // -
//		event = EVT_DEC_CURRENT_RANGE;
//		break;
//	case 97: // a
//		event = EVT_TOGGLE_ADVERTISEMENT;
//		break;
//	case 99: // c
//		event = EVT_TOGGLE_LOG_CURRENT;
//		break;
//	case 68: // D
//		event = EVT_TOGGLE_ADC_DIFFERENTIAL_VOLTAGE;
//		break;
//	case 100: // d
//		event = EVT_TOGGLE_ADC_DIFFERENTIAL_CURRENT;
//		break;
//	case 70: // F
//		write("Paid respect\r\n");
//		break;
//	case 102: // f
//		event = EVT_TOGGLE_LOG_FILTERED_CURRENT;
//		break;
//	case 109: // m
//		event = EVT_TOGGLE_MESH;
//		break;
//	case 112: // p
//		event = EVT_TOGGLE_LOG_POWER;
//		break;
//	case 82: // R
//		write("radio: %u\r\n", NRF_RADIO->POWER);
//		break;
//	case 114: // r
//		event = EVT_CMD_RESET;
//		break;
//	case 86: // V
//		event = EVT_TOGGLE_ADC_VOLTAGE_VDD_REFERENCE_PIN;
//		break;
//	case 118: // v
//		event = EVT_TOGGLE_LOG_VOLTAGE;
//		break;
//	}
//	if (event != 0) {
//		EventDispatcher::getInstance().dispatch(event);
//	}
//	readBusy = false;
}
