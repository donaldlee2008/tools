#include "SerialPort.h"
#include <Windows.h>
#include <assert.h>
#include <thread>
#include <vector>
#include <string>
#include <iostream>

#include "log.h"
#include "TimeCounter.h"

namespace {
	constexpr int COMNAME_LEN = sizeof("COM");

	class Event
	{
	public:
		Event() = default;
		~Event()
		{
			CloseHandle(handle_);
			handle_ = nullptr;
		}

		bool Create(const std::string &name)
		{
			CloseHandle(handle_);
			handle_ = CreateEvent(nullptr, FALSE, FALSE, name.c_str());
			if (handle_ == nullptr)
			{
				return false;
			}
			return true;
		}
		
		bool Set(bool signaled)
		{
			if (signaled)
			{
				return SetEvent(handle_);
			}

			return ResetEvent(handle_);
		}

		HANDLE handle_ = nullptr;
	};

	struct InternalOverlapped : public OVERLAPPED
	{
		InternalOverlapped()
		{
			Internal = 0;
			InternalHigh = 0;
			Offset = 0;
			OffsetHigh = 0;
			hEvent = nullptr;
		}

		bool Create(const std::string &name)
		{
			CloseHandle(hEvent);
			hEvent = CreateEvent(nullptr, TRUE, FALSE, name.c_str());
			if (hEvent == nullptr)
			{
				return false;
			}
			return true;
		}

		void NotSignaled()
		{
			ResetEvent(hEvent);
		}

		~InternalOverlapped()
		{
			CloseHandle(hEvent);
			hEvent = nullptr;
			Internal = 0;
			InternalHigh = 0;
			Offset = 0;
			OffsetHigh = 0;
		}
	};

}

struct SerialPort::SerialInfo
{
	SerialPortConfig conf_;
	HANDLE comHandle_ = INVALID_HANDLE_VALUE;
	std::function<void(const std::vector<uint8_t>&, void*)> callBack_;
	std::thread callBackThd_;
	InternalOverlapped overlaped_;
	Event ctrlEvent_;
	bool enableCallback_ = false;

	SerialInfo(SerialPortConfig info)
		: conf_(info)
	{}
};

SerialPort::SerialPort(SerialPortConfig conf)
	: data_(new SerialInfo(conf))
{
	InitLog();
}


SerialPort::~SerialPort()
{
	delete data_;
	data_ = nullptr;
	DestroyLog();
}

bool SerialPort::Open(bool enableCallback /*= false*/)
{
	data_->enableCallback_ = enableCallback;
	assert(data_->comHandle_ == INVALID_HANDLE_VALUE);
	std::string portName = data_->conf_.portName;
	
	// ���COM�Ŵ���9��ʹ��\\.\COMxxx����ʽ
	if (data_->conf_.portName.size() > COMNAME_LEN)
	{
		portName = R"(\\.\)" + data_->conf_.portName;
	}

	DWORD attr = 0;
	if (data_->enableCallback_)
	{
		attr = FILE_FLAG_OVERLAPPED;
	}

	// ��õ�ǰ���ھ��
	data_->comHandle_  = CreateFile(portName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
		NULL, OPEN_EXISTING, attr, NULL);

	if (data_->comHandle_ == INVALID_HANDLE_VALUE)
	{
		ERROLOG("�򿪴���CreateFile" + portName+ "ʧ��,GetLastError=" + GetLastError());
		return false;
	}

	auto closeCom = [&handle = data_->comHandle_]
	{
		CloseHandle(handle);
		handle = NULL;
	};

	//���ö���ʱ
	COMMTIMEOUTS timeouts;
	GetCommTimeouts(data_->comHandle_, &timeouts);
	timeouts.ReadIntervalTimeout = 0;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.ReadTotalTimeoutConstant = data_->conf_.readTimeOut;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = data_->conf_.writeTimeOut;
	SetCommTimeouts(data_->comHandle_, &timeouts);

	// ���ô���������Ϣ
	DCB dcb;
	if (!GetCommState(data_->comHandle_, &dcb))
	{
		ERROLOG("��ȡ������ϢGetCommStateʧ�� GetLastError=" + GetLastError());
		closeCom();
		return false;
	}

	dcb.BaudRate = data_->conf_.baudRate; // ���ݴ�������
	dcb.ByteSize = data_->conf_.byteSize; // ÿ�ֽ�λ��
	dcb.Parity = data_->conf_.parity;     // У�鷽ʽ
	dcb.StopBits = data_->conf_.stopBits;
	if (dcb.Parity != NOPARITY)
	{
		dcb.fParity = TRUE;
	}

	if (!SetCommState(data_->comHandle_, &dcb))
	{
		ERROLOG("���ô�����ϢSetCommStateʧ��,GetLastError=" + GetLastError() + ",portName=" + portName + ",������=" + data_->conf_.baudRate +
			",����λ=" + data_->conf_.byteSize + ",У��λ=" + data_->conf_.parity + ",ֹͣλ=" + data_->conf_.stopBits);
		closeCom();
		return false;
	}

	// ��ջ���
	PurgeComm(data_->comHandle_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

	if (data_->enableCallback_)
	{
		auto name = R"(Global\)" + data_->conf_.portName + ".ctrl";
		if (!data_->ctrlEvent_.Create(name))
		{
			ERROLOG("���������¼�����ʧ��, GetLastError=" + GetLastError() + ",name=" + name);
			CloseHandle(data_->comHandle_);
			data_->comHandle_ = INVALID_HANDLE_VALUE;
			return false;
		}

		name = R"(Global\)" + data_->conf_.portName + ".serial";
		if (!data_->overlaped_.Create(name))
		{
			ERROLOG("���¼�����" + name + ",ʧ��, GetLastError=" + GetLastError());
			return false;
		}
	}
	
	return true;
}

bool SerialPort::Close()
{
	assert(data_->comHandle_ != INVALID_HANDLE_VALUE);
	UnregisterCallBack();
	PurgeComm(data_->comHandle_, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR);
	auto ret = CloseHandle(data_->comHandle_);
	data_->comHandle_ = INVALID_HANDLE_VALUE;
	return ret;
}

bool SerialPort::RegisterCallBack(std::function<void(const std::vector<uint8_t>&, void *)> callBack, void *param)
{
	if (!data_->enableCallback_)
	{
		ERROLOG("��δ�����ص�����");
		return false;
	}

	if (data_->callBack_)
	{
		ERROLOG("�Ѿ���һ���ص���������");
		return false;
	}

	if (data_->comHandle_ == INVALID_HANDLE_VALUE)
	{
		ERROLOG("��Ч�Ĵ��ھ��");
		return false;
	}

	if (!SetCommMask(data_->comHandle_, EV_RXCHAR))
	{
		ERROLOG("���ô����¼�����SetCommMask����, GetLastError=" + GetLastError());
		return false;
	}

	data_->callBack_ = callBack;
	
	auto readThd = [&]
	{
		DWORD mask = 0, dwErrorFlags = 0, realLen = 0;
		COMSTAT comStat;
		std::vector<uint8_t> buffer;
		HANDLE handles[] = { data_->overlaped_.hEvent, data_->ctrlEvent_.handle_ };

		while (true)
		{
			data_->overlaped_.NotSignaled();
			WaitCommEvent(data_->comHandle_, &mask, &data_->overlaped_);
			if (GetLastError() != ERROR_IO_PENDING)
			{
				ERROLOG("�ύ�첽����WaitCommEvent����, GetLastError=" + GetLastError());
				return;
			}

			auto ret = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
			auto eventno = ret - WAIT_OBJECT_0;
			if (eventno == 1)
			{
				return;
			}
			else if (eventno != 0)
			{
				WARNLOG("WaitForMultipleObjects ret = " + ret);
				continue;
			}

			if (!(mask & EV_RXCHAR))
			{
				continue;
			}

			if (!ClearCommError(data_->comHandle_, &dwErrorFlags, &comStat))
			{
				ERROLOG("��մ���" + data_->conf_.portName + "����ClearCommErrorʧ��,GetLastError=" + GetLastError());
				continue;
			}


			if (comStat.cbInQue == 0)
			{
				continue;
			}

			buffer.resize(comStat.cbInQue);
			realLen = 0;

			if (!ReadFile(data_->comHandle_, buffer.data(), comStat.cbInQue, &realLen, &data_->overlaped_))
			{
				ERROLOG("��ȡ����" + data_->conf_.portName + "����ReadFileʧ��,GetLastError=" + GetLastError());
				continue;
			}

			data_->callBack_(buffer, param);
		}
	};

	data_->callBackThd_ = std::thread(readThd);
	return true;
}

bool SerialPort::UnregisterCallBack()
{
	if (data_->comHandle_ == INVALID_HANDLE_VALUE)
	{
		ERROLOG("���ھ��Ϊ��");
		return false;
	}

	if (!data_->ctrlEvent_.Set(true))
	{
		ERROLOG("���ÿ����¼�����Ϊ���ź�״̬ʧ��, GetLastError=" + GetLastError());
		return false;
	}

	if (data_->callBackThd_.joinable())
	{
		data_->callBackThd_.join();
	}

	data_->callBack_ = std::function<void(const std::vector<uint8_t> &, void*)>();
	return true;
}

std::string SerialPort::Read(uint32_t dataLen /*= 12*/)
{
	assert(data_->comHandle_ != INVALID_HANDLE_VALUE);
	COMSTAT comStat;
	DWORD dwErrorFlags;
	// ��ջ���
	if (!PurgeComm(data_->comHandle_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT))
	{
		ERROLOG("��մ���" + data_->conf_.portName + "����PurgeCommʧ��,GetLastError=" + GetLastError());
		return "";
	}
	// �����ȴ����뻺����������������
	// �����ʼ�������ó�ʱʧ�����ô˴���ʱ�ж�
	TimeCounter timer;
	timer.Start();
	while (true)
	{
		if (!ClearCommError(data_->comHandle_, &dwErrorFlags, &comStat))
		{
			ERROLOG("��մ���" + data_->conf_.portName + "����ClearCommErrorʧ��,GetLastError=" + GetLastError());
			return "";
		}

		if (comStat.cbInQue >= dataLen)
		{
			break;
		}

		if (timer.End() > data_->conf_.readTimeOut)
		{
			ERROLOG("������" + data_->conf_.portName + "���ݳ�ʱ,�ȴ�ʱ��Ϊ" + timer.End() + ",���ó�ʱʱ��Ϊ" + data_->conf_.readTimeOut);
			return "";
		}

		std::this_thread::yield();
	}

	auto bufferLen = dataLen == 0 ? 1024 : dataLen;
	std::vector<char> buffer(bufferLen);
	DWORD realLen = 0;
	
	if (!ReadFile(data_->comHandle_, buffer.data(), bufferLen, &realLen, nullptr))
	{
		ERROLOG("��ȡ����" + data_->conf_.portName + "����ReadFileʧ��,GetLastError=" + GetLastError());
		return "";
	}

	// ���������ݲ��ܱ����õ�С
	if (realLen < dataLen)
	{
		ERROLOG("��ȡ����" + data_->conf_.portName + "���ݳ���" + realLen + "���ڴ�����" + dataLen + "�����");
		return "";
	}

	return std::string(buffer.data(), realLen);
}

bool SerialPort::Write(const std::string &contents)
{
	assert(data_->comHandle_ != INVALID_HANDLE_VALUE);
	COMSTAT ComStat;
	DWORD dwErrorFlags = 0;
	if (!ClearCommError(data_->comHandle_, &dwErrorFlags, &ComStat))
	{
		ERROLOG("��մ���" + data_->conf_.portName + "����ClearCommErrorʧ��,GetLastError=" + GetLastError());
		return false;
	}

	DWORD length = 0;
	if (!WriteFile(data_->comHandle_, contents.data(), contents.length(), &length, nullptr))
	{
		ERROLOG("д����" + data_->conf_.portName + "����WriteFileʧ��,GetLastError=" + GetLastError() + ",���ݳ���Ϊ" + contents.length());
		return false;
	}

	// �ж���Ҫд��Ĵ�С��ʵ��д��Ĵ�С�Ƿ���ͬ
	if (length != contents.length())
	{
		ERROLOG("д����" + data_->conf_.portName + "���ݳ���" + length + "����������" + contents.length() + "�����");
		return false;
	}

	return true;
}

