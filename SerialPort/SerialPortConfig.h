#pragma once
#include <cstdint>
#include <string>

// ���ں�����
struct SerialPortConfig
{
	std::string portName = "COM1";		// �˿ں�����
	uint32_t baudRate = 1200;			// ������
	uint8_t byteSize = 8;				// ����λ
	uint8_t parity = 0;					// ��żУ��λ
	uint8_t stopBits = 0;				// ֹͣλ
	uint32_t readTimeOut = 2000;		// ����ʱʱ�� ���� 0����δ����
	uint32_t writeTimeOut = 2000;		// д��ʱʱ�� ���� 0����δ����
};