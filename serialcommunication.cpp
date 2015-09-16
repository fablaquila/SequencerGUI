/******************************************************************************
 * SequencerGUI                                                               *
 * Copyright (C) 2015                                                         *
 * Tomassino Ferrauto <t_ferrauto@yahoo.it>                                   *
 *                                                                            *
 * This program is free software; you can redistribute it and/or modify       *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation; either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software                *
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA *
 ******************************************************************************/

#include "serialcommunication.h"
#include <QDebug>

SerialCommunication::SerialCommunication(QObject* parent)
	: QObject(parent)
	, m_serialPort()
	, m_sequence(nullptr)
	, m_isStreamMode(false)
	, m_isImmediateMode(false)
	, m_arduinoBoot()
	, m_incomingData()
	, m_paused(false)
	, m_hardwareQueueFull(false)
{
	// Connecting signals from the serial port
	connect(&m_serialPort, &QSerialPort::readyRead, this, &SerialCommunication::handleReadyRead);
	connect(&m_serialPort, static_cast<void (QSerialPort::*)(QSerialPort::SerialPortError)>(&QSerialPort::error), this, &SerialCommunication::handleError);

	// Connecting the signal for the Arduino boot timer. Also setting the timer to be singleShot
	m_arduinoBoot.setSingleShot(true);
	connect(&m_arduinoBoot, &QTimer::timeout, this, &SerialCommunication::arduinoBootFinished);
}

SerialCommunication::~SerialCommunication()
{
	// No exception must leave the destructor, so we catch everything here. We use two separated
	// try-catch statements because we must always call both stop() and closeSerial()
	try {
		stop();
	} catch (...) {
	}

	try {
		closeSerial();
	} catch (...) {
	}
}

bool SerialCommunication::openSerial(QString port, int baudRate)
{
	if (isStreaming()) {
		throw SerialCommunicationException("Cannot open port while a sequence is being streamed");
	}

	// Closing the old port
	if (m_serialPort.isOpen()) {
		m_serialPort.close();
		m_serialPort.clearError();
	}

	// Setting the name and baud rate of the port
	m_serialPort.setPortName(port);
	m_serialPort.setBaudRate(baudRate);

	// Trying to open the port
	if (!m_serialPort.open(QIODevice::ReadWrite)) {
		return false;
	}

	// This is necessary to give time to Arduino to "boot" (the board reboots every time the serial port
	// is opened, and then there are 0.5 seconds taken by the bootloader)
	m_arduinoBoot.start(1000);

	return true;
}

void SerialCommunication::closeSerial()
{
	if (isStreaming()) {
		throw SerialCommunicationException("Cannot close port while a sequence is being streamed");
	}

	// Closing the port
	if (m_serialPort.isOpen()) {
		m_serialPort.close();
		m_serialPort.clearError();
	}
}

void SerialCommunication::startStream(Sequence* sequence, bool startFromCurrent)
{
	if (!m_serialPort.isOpen()) {
		throw SerialCommunicationException("Cannot start streaming with a closed serial port");
	}
	if (isStreaming()) {
		throw SerialCommunicationException("Cannot start a new stream while a sequence is already being streamed");
	}

	m_incomingData.clear();

	// Resetting the pause flag and setting the m_is*Mode flags
	m_paused = false;
	m_isStreamMode = true;
	m_isImmediateMode = false;
	m_hardwareQueueFull = false;

	// Saving the sequence and resetting the current point if needed
	m_sequence = sequence;
	if (!startFromCurrent) {
		m_sequence->setCurPoint(0);
	}

	// If the m_arduinoBoot timer is running, we have to wait, otherwise we explicitly call
	// the arduinoBootFinished() function to start sending the sequence
	if (!m_arduinoBoot.isActive()) {
		arduinoBootFinished();
	}
}

void SerialCommunication::pauseStream()
{
	if (!isStreamMode()) {
		throw SerialCommunicationException("Cannot pause when no sequence is being streamed");
	}

	m_paused = true;
}

void SerialCommunication::resumeStream()
{
	if (!isStreamMode()) {
		throw SerialCommunicationException("Cannot pause when no sequence is being streamed");
	}

	if (!m_paused) {
		return;
	}

	// Resuming streaming
	m_paused = false;

	// Sending a sequence point if the hardware queue is not full
	if (!m_hardwareQueueFull) {
		sendData(createSequencePacketForPoint(m_sequence->point()));

		// Moving current point forward by one or terminating the sequence if the current point
		// was the last point
		incrementCurPoint();
	}
}

void SerialCommunication::startImmediate(Sequence* sequence)
{
	if (!m_serialPort.isOpen()) {
		throw SerialCommunicationException("Cannot start streaming with a closed serial port");
	}
	if (isStreaming()) {
		throw SerialCommunicationException("Cannot start a new stream while a sequence is already being streamed");
	}

	m_incomingData.clear();

	// Setting the m_is*Mode flags
	m_isStreamMode = false;
	m_isImmediateMode = true;

	// Saving the sequence
	m_sequence = sequence;

	// Connecting the signal of the sequence telling us when the current point changes
	connect(m_sequence, &Sequence::curPointChanged, this, &SerialCommunication::curPointChanged);

	// If the m_arduinoBoot timer is running, we have to wait, otherwise we explicitly call
	// the arduinoBootFinished() function to send the current point of the sequence
	if (!m_arduinoBoot.isActive()) {
		arduinoBootFinished();
	}
}

void SerialCommunication::stop()
{
	if (!isStreaming()) {
		throw SerialCommunicationException("No stream to stop");
	}

	// Sending packet to stop streaming
	sendData(QByteArray("H"));

	// Disconnecting all signals from the sequence to us
	m_sequence->disconnect(this);

	// Setting the sequence to nullptr
	m_sequence = nullptr;

	// If we were in stream mode, emitting signal
	if (isStreamMode()) {
		emit streamStopped();
	}

	// Resetting flags
	m_paused = false;
	m_isStreamMode = false;
	m_isImmediateMode = false;
	m_hardwareQueueFull = false;

	m_incomingData.clear();
}

void SerialCommunication::handleReadyRead()
{
	// Getting data and adding to the buffer
	m_incomingData.append(m_serialPort.readAll());

	// This flag is true if there is data in the m_incomingData buffer but the packet is
	// not complete and we must wait for more data
	bool partialPacket = false;
	while (!(m_incomingData.isEmpty() || partialPacket)) {
		// Taking the first characted, of data, it tells us the kind of packet we received
		if (isStreamMode() && (m_incomingData[0] == 'N')) {
			// Buffer not full, we can send the current point in the sequence and move the current point forward
			m_hardwareQueueFull = false;
			sendData(createSequencePacketForPoint(m_sequence->point()));
			incrementCurPoint();

			// Removing packet from our buffer
			m_incomingData.remove(0, 1);
		} else if (isStreamMode() && (m_incomingData[0] == 'F')) {
			// Buffer full
			m_hardwareQueueFull = true;

			// Removing packet from our buffer
			m_incomingData.remove(0, 1);
		} else if (m_incomingData[0] == 'D') {
			// Debug packet, printing and emitting signal if the message is complete
			if (m_incomingData.size() < 2) {
				partialPacket = true;
			} else {
				// Reading message length
				int msgLength = static_cast<unsigned char>(m_incomingData[1]);

				// Checking we have the whole message
				if (m_incomingData.size() < (2 + msgLength)) {
					partialPacket = true;
				} else {
					// We have the whole message, putting in a QString
					const QString msg(m_incomingData.mid(2, msgLength));

					// Emitting signal and printing
					emit debugMessage(msg);
					qDebug() << "Debug packet, content:" << msg;
				}
			}
		} else {
			const QString errorString = QString("Received unknown or invalid packet type %1 (ascii %2)").arg(static_cast<unsigned int>(m_incomingData[0])).arg(m_incomingData[0]);
			emit streamError(errorString);
			qDebug() << errorString;

			// Removing the unknown character
			m_incomingData.remove(0, 1);
		}
	}
}

void SerialCommunication::handleError(QSerialPort::SerialPortError error)
{
	if (error != QSerialPort::NoError) {
		const QString errorString = "Error streaming: " + m_serialPort.errorString();
		emit streamError(errorString);
		qDebug() << errorString;

//		QString errorString = "Error streaming, error code: ";

//		switch (error) {
//			case QSerialPort::DeviceNotFoundError:
//				errorString += "DeviceNotFoundError (an error occurred while attempting to open an non-existing device)";
//				break;
//			case QSerialPort::PermissionError:
//				errorString += "PermissionError (an error occurred while attempting to open an already opened device by another process or a user not having enough permission and credentials to open)";
//				break;
//			case QSerialPort::OpenError:
//				errorString += "OpenError (an error occurred while attempting to open an already opened device in this object)";
//				break;
//			case QSerialPort::NotOpenError:
//				errorString += "NotOpenError (this error occurs when an operation is executed that can only be successfully performed if the device is open)";
//				break;
//			case QSerialPort::ParityError:
//				errorString += "ParityError (parity error detected by the hardware while reading data)";
//				break;
//			case QSerialPort::FramingError:
//				errorString += "FramingError (framing error detected by the hardware while reading data)";
//				break;
//			case QSerialPort::BreakConditionError:
//				errorString += "BreakConditionError (break condition detected by the hardware on the input line)";
//				break;
//			case QSerialPort::WriteError:
//				errorString += "WriteError (an I/O error occurred while writing the data)";
//				break;
//			case QSerialPort::ReadError:
//				errorString += "ReadError (an I/O error occurred while reading the data)";
//				break;
//			case QSerialPort::ResourceError:
//				errorString += "ResourceError (an I/O error occurred when a resource becomes unavailable, e.g. when the device is unexpectedly removed from the system)";
//				break;
//			case QSerialPort::UnsupportedOperationError:
//				errorString += "UnsupportedOperationError (the requested device operation is not supported or prohibited by the running operating system)";
//				break;
//			case QSerialPort::TimeoutError:
//				errorString += "TimeoutError (a timeout error occurred)";
//				break;
//			case QSerialPort::UnknownError:
//			default:
//				errorString += "UnknownError (an unidentified error occurred)";
//				break;
//		}

//		emit streamError(errorString);
//		qDebug() << "Serial error." << errorString;
	}
}

void SerialCommunication::arduinoBootFinished()
{
	// If we are streaming, sending data, otherwise doing nothing
	if (isStreaming()) {
		// First sending the start packet
		QByteArray startPacket;
		if (isStreamMode()) {
			startPacket.append('S');
		} else if (isImmediateMode()) {
			startPacket.append('I');
		} else {
			qFatal("Unknown mode, we should never get here");
		}
		// Adding the number of dimension of point to the start packet
		startPacket.append(m_sequence->pointDim() && 0xFF);
		sendData(startPacket);

		// Now sending the first sequence packet
		sendData(createSequencePacketForPoint(m_sequence->point()));

		// If we are in stream mode, we have to emit the signal and move the current point forward
		if (isStreamMode()) {
			emit streamStarted();

			incrementCurPoint();
		}
	}
}

void SerialCommunication::curPointChanged()
{
	// Safety check that we are in immediate mode (this slot is only connected in immediate mode)
	if (Q_UNLIKELY(!isImmediateMode())) {
		qDebug() << "Received curPointChanged() signal but not in immediate mode";

		return;
	}

	// Sending the current point
	sendData(createSequencePacketForPoint(m_sequence->point()));
}

QByteArray SerialCommunication::createSequencePacketForPoint(const SequencePoint& p) const
{
	QByteArray pkt(5 + p.point.size(), 0);

	// Packet type
	pkt[0] = 'P';

	// Point duration
	pkt[1] = (p.duration >> 8) && 0xFF;
	pkt[2] = p.duration & 0xFF;

	// Point time to target
	pkt[3] = (p.timeToTarget >> 8) && 0xFF;
	pkt[4] = p.timeToTarget & 0xFF;

	// Values
	for (int c = 0; c < p.point.size(); ++c) {
		pkt[5 + c] = static_cast<unsigned int>(p.point[c]) & 0xFF;
	}

	return pkt;
}

void SerialCommunication::incrementCurPoint()
{
	const int curPoint = m_sequence->curPoint();

	if (curPoint == (m_sequence->numPoints() - 1)) {
		// We are at the last point, stopping
		stop();
	} else {
		m_sequence->setCurPoint(curPoint + 1);
	}
}

void SerialCommunication::sendData(const QByteArray& dataToSend)
{
	// Writing data
	qint64 bytesWritten = m_serialPort.write(dataToSend);

	if (bytesWritten == -1) {
		qDebug() << "Error writing data";
	} else if (bytesWritten != dataToSend.size()) {
		qDebug() << "Cannot write all data";
	}
}
