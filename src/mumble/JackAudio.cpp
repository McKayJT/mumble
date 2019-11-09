// Copyright 2005-2019 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "JackAudio.h"

// We define a global macro called 'g'. This can lead to issues when included code uses 'g' as a type or parameter name (like protobuf 3.7 does). As such, for now, we have to make this our last include.
#include "Global.h"

#ifdef Q_CC_GNU
# define RESOLVE(var) {var = reinterpret_cast<__typeof__(var)>(qlJack.resolve(#var)); if (!var) return; }
#else
# define RESOLVE(var) { *reinterpret_cast<void **>(&var) = static_cast<void *>(qlJack.resolve(#var)); if (!var) return; }
#endif

static std::unique_ptr<JackAudioSystem> jas;

// jackStatusToStringList converts a jack_status_t (a flag type
// that can contain multiple Jack statuses) to a QStringList.
static QStringList jackStatusToStringList(const jack_status_t &status) {
	QStringList statusList;

	if (status & JackFailure) {
		statusList << QLatin1String("JackFailure - overall operation failed");
	}
	if (status & JackInvalidOption) {
		statusList << QLatin1String("JackInvalidOption - the operation contained an invalid or unsupported option");
	}
	if (status & JackNameNotUnique)  {
		statusList << QLatin1String("JackNameNotUnique - the desired client name is not unique");
	}
	if (status & JackServerStarted) {
		statusList << QLatin1String("JackServerStarted - the server was started as a result of this operation");
	}
	if (status & JackServerFailed) {
		statusList << QLatin1String("JackServerFailed - unable to connect to the JACK server");
	}
	if (status & JackServerError) {
		statusList << QLatin1String("JackServerError - communication error with the JACK server");
	}
	if (status & JackNoSuchClient) {
		statusList << QLatin1String("JackNoSuchClient - requested client does not exist");
	}
	if (status & JackLoadFailure) {
		statusList << QLatin1String("JackLoadFailure - unable to load initial client");
	}
	if (status & JackInitFailure) {
		statusList << QLatin1String("JackInitFailure - unable to initialize client");
	}
	if (status & JackShmFailure)  {
		statusList << QLatin1String("JackShmFailure - unable to access shared memory");
	}
	if (status & JackVersionError) {
		statusList << QLatin1String("JackVersionError - client's protocol version does not match");
	}
	if (status & JackBackendError) {
		statusList << QLatin1String("JackBackendError - a backend error occurred");
	}
	if (status & JackClientZombie) {
		statusList << QLatin1String("JackClientZombie - client zombified");
	}

	return statusList;
}

class JackAudioInputRegistrar : public AudioInputRegistrar {
	public:
		JackAudioInputRegistrar();
		virtual AudioInput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
		virtual bool canEcho(const QString &) const;
};

class JackAudioOutputRegistrar : public AudioOutputRegistrar {
	public:
		JackAudioOutputRegistrar();
		virtual AudioOutput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &, Settings &);
};

class JackAudioInit : public DeferInit {
	public:
		std::unique_ptr<JackAudioInputRegistrar> airJackAudio;
		std::unique_ptr<JackAudioOutputRegistrar> aorJackAudio;
		void initialize();
		void destroy();
};

JackAudioInputRegistrar::JackAudioInputRegistrar() : AudioInputRegistrar(QLatin1String("JACK"), 10) {}

AudioInput *JackAudioInputRegistrar::create() {
	return new JackAudioInput();
}

const QList<audioDevice> JackAudioInputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	auto qlInputDevs = jas->qhInput.keys();
	std::sort(qlInputDevs.begin(), qlInputDevs.end());

	for (const auto &dev : qlInputDevs) {
		qlReturn << audioDevice(jas->qhInput.value(dev), dev);
	}

	return qlReturn;
}

void JackAudioInputRegistrar::setDeviceChoice(const QVariant &, Settings &) {}

bool JackAudioInputRegistrar::canEcho(const QString &) const {
	return false;
}

JackAudioOutputRegistrar::JackAudioOutputRegistrar() : AudioOutputRegistrar(QLatin1String("JACK"), 10) {}

AudioOutput *JackAudioOutputRegistrar::create() {
	return new JackAudioOutput();
}

const QList<audioDevice> JackAudioOutputRegistrar::getDeviceChoices() {
	QList<audioDevice> qlReturn;

	QStringList qlOutputDevs = jas->qhOutput.keys();
	std::sort(qlOutputDevs.begin(), qlOutputDevs.end());

	if (qlOutputDevs.contains(g.s.qsJackAudioOutput)) {
		qlOutputDevs.removeAll(g.s.qsJackAudioOutput);
		qlOutputDevs.prepend(g.s.qsJackAudioOutput);
	}

	foreach(const QString &dev, qlOutputDevs) {
		qlReturn << audioDevice(jas->qhOutput.value(dev), dev);
	}

	return qlReturn;
}

void JackAudioOutputRegistrar::setDeviceChoice(const QVariant &choice, Settings &s) {
	s.qsJackAudioOutput = choice.toString();
}

void JackAudioInit::initialize() {
	jas.reset(new JackAudioSystem());

	jas->qmWait.lock();
	jas->qwcWait.wait(&jas->qmWait, 1000);
	jas->qmWait.unlock();

	if (jas->bAvailable) {
		airJackAudio.reset(new JackAudioInputRegistrar());
		aorJackAudio.reset(new JackAudioOutputRegistrar());
	} else {
		jas.reset();
	}
}

void JackAudioInit::destroy() {
	airJackAudio.reset();
	aorJackAudio.reset();
	jas.reset();
}

// Instantiate JackAudioSystem, JackAudioInputRegistrar and JackAudioOutputRegistrar
static JackAudioInit jai;

JackAudioSystem::JackAudioSystem()
    : bAvailable(false)
    , users(0)
    , client(nullptr)
{
	QStringList alternatives;
#ifdef Q_OS_WIN
	alternatives << QLatin1String("libjack64.dll");
	alternatives << QLatin1String("libjack32.dll");
#elif defined(Q_OS_MAC)
	alternatives << QLatin1String("libjack.dylib");
	alternatives << QLatin1String("libjack.0.dylib");
#else
	alternatives << QLatin1String("libjack.so");
	alternatives << QLatin1String("libjack.so.0");
#endif
	for (const QString &lib : alternatives) {
		qlJack.setFileName(lib);
		if (qlJack.load()) {
			break;
		}
	}

	if (!qlJack.isLoaded()) {
		return;
	}

	RESOLVE(jack_get_version_string)
	RESOLVE(jack_free)
	RESOLVE(jack_get_client_name)
	RESOLVE(jack_client_open)
	RESOLVE(jack_client_close)
	RESOLVE(jack_activate)
	RESOLVE(jack_deactivate)
	RESOLVE(jack_get_sample_rate)
	RESOLVE(jack_get_buffer_size)
	RESOLVE(jack_get_client_name)
	RESOLVE(jack_get_ports)
	RESOLVE(jack_connect)
	RESOLVE(jack_port_disconnect)
	RESOLVE(jack_port_register)
	RESOLVE(jack_port_unregister)
	RESOLVE(jack_port_name)
	RESOLVE(jack_port_by_name)
	RESOLVE(jack_port_flags)
	RESOLVE(jack_port_get_buffer)
	RESOLVE(jack_set_process_callback)
	RESOLVE(jack_set_sample_rate_callback)
	RESOLVE(jack_set_buffer_size_callback)
	RESOLVE(jack_on_shutdown)
	RESOLVE(jack_ringbuffer_create)
	RESOLVE(jack_ringbuffer_free)
	RESOLVE(jack_ringbuffer_mlock)
	RESOLVE(jack_ringbuffer_read)
	RESOLVE(jack_ringbuffer_read_space)
	RESOLVE(jack_ringbuffer_write_space)
	RESOLVE(jack_ringbuffer_get_write_vector)
	RESOLVE(jack_ringbuffer_write_advance)

	qhInput.insert(QString(), tr("Hardware Ports"));
	qhOutput.insert(QString::number(1), tr("Mono"));
	qhOutput.insert(QString::number(2), tr("Stereo"));

	bAvailable = true;

	qDebug("JACK %s from %s", jack_get_version_string(), qPrintable(qlJack.fileName()));
}

JackAudioSystem::~JackAudioSystem() {
	deinitialize();
}

bool JackAudioSystem::initialize() {
	QMutexLocker lock(&qmWait);

	if (client) {
		lock.unlock();
		deinitialize();
		lock.relock();
	}

	jack_status_t status;
	client = jack_client_open(g.s.qsJackClientName.toStdString().c_str(), g.s.bJackStartServer ? JackNullOption : JackNoStartServer, &status);
	if (!client) {
		const auto errors = jackStatusToStringList(status);
		qWarning("JackAudioSystem: unable to open client due to %i errors:", errors.count());
		for (auto i = 0; i < errors.count(); ++i) {
			qWarning("JackAudioSystem: %s", qPrintable(errors.at(i)));
		}

		return false;
	}

	qDebug("JackAudioSystem: client \"%s\" opened successfully", jack_get_client_name(client));

	auto ret = jack_set_process_callback(client, processCallback, nullptr);
	if (ret != 0) {
		qWarning("JackAudioSystem: unable to set process callback - jack_set_process_callback() returned %i", ret);
		jack_client_close(client);
		client = nullptr;
		return false;
	}

	ret = jack_set_sample_rate_callback(client, sampleRateCallback, nullptr);
	if (ret != 0) {
		qWarning("JackAudioSystem: unable to set sample rate callback - jack_set_sample_rate_callback() returned %i", ret);
		jack_client_close(client);
		client = nullptr;
		return false;
	}

	ret = jack_set_buffer_size_callback(client, bufferSizeCallback, nullptr);
	if (ret != 0) {
		qWarning("JackAudioSystem: unable to set buffer size callback - jack_set_buffer_size_callback() returned %i", ret);
		jack_client_close(client);
		client = nullptr;
		return false;
	}

	jack_on_shutdown(client, shutdownCallback, nullptr);

	return true;
}

void JackAudioSystem::deinitialize() {
	QMutexLocker lock(&qmWait);

	if (!client) {
		return;
	}

	const auto clientName = QString::fromLatin1(jack_get_client_name(client));

	const auto err = jack_client_close(client);
	if (err != 0) {
		qWarning("JackAudioSystem: unable to disconnect from the server - jack_client_close() returned %i", err);
		return;
	}

	client = nullptr;

	qDebug("JackAudioSystem: client \"%s\" closed successfully", clientName.toStdString().c_str());
}

bool JackAudioSystem::activate() {
	QMutexLocker lock(&qmWait);

	if (!client) {
		lock.unlock();

		if (!initialize()) {
			return false;
		}

		lock.relock();
	}

	if (users++ > 0) {
		// The client is already active, because there is at least a user
		return true;
	}

	const auto ret = jack_activate(client);
	if (ret != 0) {
		qWarning("JackAudioSystem: unable to activate client - jack_activate() returned %i", ret);
		return false;
	}

	qDebug("JackAudioSystem: client activated");

	return true;
}

void JackAudioSystem::deactivate() {
	QMutexLocker lock(&qmWait);

	if (!client) {
		return;
	}

	if (--users > 0) {
		// There is still at least a user, we only decrement the counter
		return;
	}

	const auto err = jack_deactivate(client);
	if (err != 0)  {
		qWarning("JackAudioSystem: unable to remove client from the process graph - jack_deactivate() returned %i", err);
		return;
	}

	qDebug("JackAudioSystem: client deactivated");

	lock.unlock();

	deinitialize();
}

bool JackAudioSystem::isOk() {
	QMutexLocker lock(&qmWait);
	return client != nullptr;
}

uint8_t JackAudioSystem::outPorts() {
	return static_cast<uint8_t>(qBound<unsigned>(1, g.s.qsJackAudioOutput.toUInt(), JACK_MAX_OUTPUT_PORTS));
}

jack_nframes_t JackAudioSystem::sampleRate() {
	QMutexLocker lock(&qmWait);

	if (!client) {
		return 0;
	}

	return jack_get_sample_rate(client);
}

jack_nframes_t JackAudioSystem::bufferSize() {
	QMutexLocker lock(&qmWait);

	if (!client) {
		return 0;
	}

	return jack_get_buffer_size(client);
}

JackPorts JackAudioSystem::getPhysicalPorts(const uint8_t &flags) {
	QMutexLocker lock(&qmWait);

	if (!client) {
		return JackPorts();
	}

	const auto ports = jack_get_ports(client, nullptr, nullptr, JackPortIsPhysical | JackPortIsTerminal);
	if (!ports) {
		return JackPorts();
	}

	JackPorts ret;

	for (auto i = 0; ports[i]; ++i) {
		if (!ports[i]) {
			// End of the array
			break;
		}

		auto port = jack_port_by_name(client, ports[i]);
		if (!port)  {
			qWarning("JackAudioSystem: jack_port_by_name() returned an invalid port - skipping it");
			continue;
		}

		if (jack_port_flags(port) & flags) {
			ret.append(port);
		}
	}

	jack_free(ports);

	return ret;
}

void *JackAudioSystem::getPortBuffer(jack_port_t *port, const jack_nframes_t &frames) {
	if (!port) {
		return nullptr;
	}

	return jack_port_get_buffer(port, frames);
}

jack_port_t *JackAudioSystem::registerPort(const char *name, const uint8_t &flags) {
	QMutexLocker lock(&qmWait);

	if (!client || !name) {
		return nullptr;
	}

	return jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, flags, 0);
}


bool JackAudioSystem::unregisterPort(jack_port_t *port) {
	QMutexLocker lock(&qmWait);

	if (!client || !port) {
		return false;
	}

	const auto ret = jack_port_unregister(client, port);
	if (ret != 0)  {
		qWarning("JackAudioSystem: unable to unregister port - jack_port_unregister() returned %i", ret);
		return false;
	}

	return true;
}

bool JackAudioSystem::connectPort(jack_port_t *sourcePort, jack_port_t *destinationPort) {
	QMutexLocker lock(&qmWait);

	if (!client || !sourcePort || !destinationPort) {
		return false;
	}

	const auto sourcePortName = jack_port_name(sourcePort);
	const auto destinationPortName = jack_port_name(destinationPort);

	const auto ret = jack_connect(client, sourcePortName, destinationPortName);
	if (ret != 0)  {
		qWarning("JackAudioSystem: unable to connect port '%s' to '%s' - jack_connect() returned %i", sourcePortName, destinationPortName, ret);
		return false;
	}

	return true;
}

bool JackAudioSystem::disconnectPort(jack_port_t *port) {
	QMutexLocker lock(&qmWait);

	if (!client || !port) {
		return false;
	}

	const auto ret = jack_port_disconnect(client, port);
	if (ret != 0)  {
		qWarning("JackAudioSystem: unable to disconnect port - jack_port_disconnect() returned %i", ret);
		return false;
	}

	return true;
}

int JackAudioSystem::processCallback(jack_nframes_t frames, void *) {
	auto const jai = dynamic_cast<JackAudioInput *>(g.ai.get());
	auto const jao = dynamic_cast<JackAudioOutput *>(g.ao.get());

	const bool input = (jai && jai->isReady());
	const bool output = (jao && jao->isReady());

	if (input && !jai->process(frames)) {
		return 1;
	}

	if (output && !jao->process(frames)) {
		return 1;
	}

	return 0;
}

int JackAudioSystem::sampleRateCallback(jack_nframes_t, void *) {
	auto const jai = dynamic_cast<JackAudioInput *>(g.ai.get());
	auto const jao = dynamic_cast<JackAudioOutput *>(g.ao.get());

	if (jai) {
		jai->activate();
	}

	if (jao) {
		jao->activate();
	}

	return 0;
}

int JackAudioSystem::bufferSizeCallback(jack_nframes_t frames, void *) {
	auto const jao = dynamic_cast<JackAudioOutput *>(g.ao.get());
	if (jao) {
		if (!jao->allocBuffer(frames)) {
			return 1;
		}
	}

	return 0;
}

/*
 * ringbuffer functions do not have locks.
 * They are single consumer single producer lockless
 */

jack_ringbuffer_t *JackAudioSystem::ringbufferCreate(size_t size) {
	if (size == 0) {
		return nullptr;
	}

	return jack_ringbuffer_create(size);
}

void JackAudioSystem::ringbufferFree(jack_ringbuffer_t *buf) {
	jack_ringbuffer_free(buf);
}

int JackAudioSystem::ringbufferMlock(jack_ringbuffer_t *buf) {
	if (buf == nullptr) {
		return -2;
	}
	return jack_ringbuffer_mlock(buf);
}

size_t JackAudioSystem::ringbufferRead(jack_ringbuffer_t *buf, char *dest, size_t cnt) {
	if(buf == nullptr || dest == nullptr || cnt == 0) {
		return 0;
	}
	return jack_ringbuffer_read(buf, dest, cnt);
}

size_t JackAudioSystem::ringbufferReadSpace(const jack_ringbuffer_t *buf) {
	if (buf == nullptr) {
		return 0;
	}
}

void JackAudioSystem::ringbufferGetWriteVector(const jack_ringbuffer_t *buf, jack_ringbuffer_data_t *vec) {
	if (buf == nullptr || vec == nullptr) {
		return;
	}
	return jack_ringbuffer_get_write_vector(buf, vec);
}

size_t JackAudioSystem::ringbufferWriteSpace(const jack_ringbuffer_t *buf) {
	if (buf == nullptr) {
		return 0;
	}
	return jack_ringbuffer_get_write_space(buf);
}

void JackAudioSystem::ringbufferWriteAdvance(jack_ringbuffer_t *buf, size_t cnt) {
	if (buff == nullptr)
		return;
	return jack_ringbuffer_write_advance(buf, cnt);
}

/*
 * end imported jack functions or ringbuffers 
 */

void JackAudioSystem::shutdownCallback(void *) {
	qWarning("JackAudioSystem: server shutdown");
	jas->client = nullptr;
	jas->users = 0;
}


JackAudioInput::JackAudioInput()
    : port(nullptr)
{
	bReady = activate();
}

JackAudioInput::~JackAudioInput() {
	// Request interruption
	qmWait.lock();
	bReady = false;
	qwcSleep.wakeAll();
	qmWait.unlock();

	// Wait for thread to exit
	wait();

	// Cleanup
	deactivate();
}

bool JackAudioInput::isReady() {
	return bReady;
}

bool JackAudioInput::activate() {
	QMutexLocker lock(&qmWait);

	if (!jas->activate()) {
		return false;
	}

	eMicFormat = SampleFloat;
	iMicChannels = 1;
	iMicFreq = jas->sampleRate();

	initializeMixer();

	lock.unlock();

	return registerPorts();
}

void JackAudioInput::deactivate() {
	unregisterPorts();
	jas->deactivate();
}

bool JackAudioInput::registerPorts() {
	unregisterPorts();

	QMutexLocker lock(&qmWait);

	port = jas->registerPort("input", JackPortIsInput);
	if (!port) {
		qWarning("JackAudioInput: unable to register port");
		return false;
	}

	return true;
}

bool JackAudioInput::unregisterPorts() {
	QMutexLocker lock(&qmWait);

	if (!port) {
		return false;
	}

	if (!jas->unregisterPort(port))  {
		qWarning("JackAudioInput: unable to unregister port");
		return false;
	}

	port = nullptr;

	return true;
}

void JackAudioInput::connectPorts() {
	disconnectPorts();

	QMutexLocker lock(&qmWait);

	if (!port) {
		return;
	}

	const JackPorts outputPorts = jas->getPhysicalPorts(JackPortIsOutput);
	for (auto outputPort : outputPorts) {
		if (jas->connectPort(outputPort, port)) {
			break;
		}
	}
}

bool JackAudioInput::disconnectPorts() {
	QMutexLocker lock(&qmWait);

	if (!port) {
		return true;
	}

	if (!jas->disconnectPort(port)) {
		qWarning("JackAudioInput: unable to disconnect port");
		return false;
	}

	return true;
}

bool JackAudioInput::process(const jack_nframes_t &frames) {
	QMutexLocker lock(&qmWait);

	auto inputBuffer = jas->getPortBuffer(port, frames);
	if (!inputBuffer) {
		return false;
	}

	addMic(inputBuffer, frames);

	return true;
}

void JackAudioInput::run() {
	if (!bReady) {
		return;
	}

	// Initialization
	if (g.s.bJackAutoConnect) {
		connectPorts();
	}

	// Pause thread until interruption is requested by the destructor
	qmWait.lock();
	qwcSleep.wait(&qmWait);
	qmWait.unlock();

	// Cleanup
	disconnectPorts();
}

JackAudioOutput::JackAudioOutput()
    : buffer(nullptr)
{
	bReady = activate();
}

JackAudioOutput::~JackAudioOutput() {
	// Request interruption
	qmWait.lock();
	bReady = false;
	qsSleep.release(1);
	qmWait.unlock();

	// Wait for thread to exit
	wait();
	// Cleanup
	deactivate();
}

bool JackAudioOutput::isReady() {
	return bReady;
}

bool JackAudioOutput::allocBuffer(const jack_nframes_t &frames) {
	QMutexLocker lock(&qmWait);

    iFrameSize = frames;
	if (buffer != nullptr) {
		jas->ringbufferFree(buffer);
	}

	buffer = jas->ringbufferCreate(iFrameSize * iSampleSize * JACK_BUFFER_PERIODS);

	if (buffer == nullptr) {
		return false;
	}

	jas->ringbufferMlock(buffer);

	return true;
}

bool JackAudioOutput::activate() {
	QMutexLocker lock(&qmWait);

	if (!jas->isOk()) {
		jas->initialize();
	}

	eSampleFormat = SampleFloat;
	iChannels = jas->outPorts();
	iMixerFreq = jas->sampleRate();
	uint32_t channelsMask[32];
	channelsMask[0] = SPEAKER_FRONT_LEFT;
	channelsMask[1] = SPEAKER_FRONT_RIGHT;
	initializeMixer(channelsMask);

	lock.unlock();

	if (!allocBuffer(jas->bufferSize())) {
		return false;
	}

	if (!registerPorts()) {
		return false;
	}

	if (!jas->activate()) {
		return false;
	}

	return true;
}

void JackAudioOutput::deactivate() {
	unregisterPorts();
	jas->deactivate();
	jas->ringbufferFree(buffer);
}

bool JackAudioOutput::registerPorts() {
	unregisterPorts();

	QMutexLocker lock(&qmWait);

	for (decltype(iChannels) i = 0; i < iChannels; ++i) {
		char name[10];
		snprintf(name, sizeof(name), "output_%d", i + 1);

		const auto port = jas->registerPort(name, JackPortIsOutput);
		if (port == nullptr) {
			qWarning("JackAudioOutput: unable to register port #%u", i);
			return false;
		}

		ports.append(port);
		outputBuffers.append(nullptr);
	}

	return true;
}

bool JackAudioOutput::unregisterPorts() {
	QMutexLocker lock(&qmWait);

	bool ret = true;

	for (auto i = 0; i < ports.size(); ++i) {
		if (!ports[i]) {
			continue;
		}

		if (!jas->unregisterPort(ports[i]))  {
			qWarning("JackAudioOutput: unable to unregister port #%u", i);
			ret = false;
		}
	}

	outputBuffers.clear();
	ports.clear();

	return ret;
}

void JackAudioOutput::connectPorts() {
	disconnectPorts();

	QMutexLocker lock(&qmWait);

	const auto inputPorts = jas->getPhysicalPorts(JackPortIsInput);
	uint8_t i = 0;

	for (auto inputPort : inputPorts) {
		if (i == ports.size()) {
			break;
		}

		if (ports[i]) {
			if (!jas->connectPort(ports[i], inputPort)) {
				continue;
			}
		}

		++i;
	}
}

bool JackAudioOutput::disconnectPorts() {
	QMutexLocker lock(&qmWait);

	bool ret = true;

	for (auto i = 0; i < ports.size(); ++i) {
		if (ports[i] && !jas->disconnectPort(ports[i])) {
			qWarning("JackAudioOutput: unable to disconnect port #%u", i);
			ret = false;
		}
	}

	return ret;
}

bool JackAudioOutput::process(const jack_nframes_t &frames) {

	if (!bReady) {
		return true;
	}

	/*
	 * FIXME: This is good on Linux, and maybe Windows.
	 * however, on certain platforms QSemaphore actually uses
	 * QWaitCondition, which uses a Mutex internally for locking
	 *
	 * This is in spite of the fact that on most POSIX systems, QMutex
	 * is actually implemented using semephores.
	 */
	qsSleep.release(1);

	for (decltype(iChannels) currentChannel = 0; currentChannel < iChannels; ++currentChannel) {

		auto outputBuffer = jas->getPortBuffer(ports[currentChannel], frames);
		if (!outputBuffer) {
			return false;
		}

		outputBuffers.replace(currentChannel, reinterpret_cast<jack_default_audio_sample_t*>(outputBuffer));
	}

	const size_t needed = frames * iSampleSize;
	size_t avail = jas->ringbufferReadSpace(buffer);

	if (avail == 0) {
		for (decltype(iChannels) currentChannel = 0; currentChannel < iChannels; ++currentChannel) {
			memset(outputBuffers[currentChannel], 0, frames * sizeof(jack_default_audio_sample_t));
		}
		return true;
	}

	if (iChannels == 1) {
		jas->ringbufferRead(buffer, reinterpret_cast<char *>(outputBuffers[0]), avail);
		if (avail < needed) {
			memset(reinterpret_cast<char *>(&(outputBuffers[avail])), 0, needed - avail);
		}
		return true;
	}

	auto samples = qMin(jas->ringbufferReadSpace(buffer), needed ) / sizeof(jack_default_audio_sample_t);
	for (auto currentSample = decltype(samples){0}; currentSample < samples; ++currentSample) {
		jas->ringbufferRead(buffer, reinterpret_cast<char *>(&outputBuffers[currentSample % iChannels][currentSample/iChannels]), sizeof(jack_default_audio_sample_t));
	}
	if ((samples / iChannels) < frames) {
		for (decltype(iChannels) currentChannel = 0; currentChannel < iChannels; ++currentChannel) {
			memset(&outputBuffers[currentChannel][avail/samples], 0, (needed - avail) / iChannels);
		}
	}

	return true;
}

void JackAudioOutput::run() {
	if (!bReady) {
		return;
	}

	// Initialization
	if (g.s.bJackAutoConnect) {
		connectPorts();
	}

    STACKVAR(unsigned char, spareSample, iSampleSize);
	jack_ringbuffer_data_t _writeVector[2];
	jack_ringbuffer_data_t *writeVector = _writeVector;

	// Keep feeding more data into the buffer
	do {
		qmWait.lock();
		auto iFrameBytes = iFrameSize * iSampleSize;
		auto iWrittenFrames = 0;
		if (jas->ringbufferWriteSpace(buffer) < iFrameBytes ) {
			qmWait.unlock();
			qsSleep.acquire(1);
			continue;
		}

		auto bOk = true;
		auto wanted = qMin(writeVector->len / iSampleSize, static_cast<size_t>(iFrameSize));

		if (wanted > 0) {
			bOk = mix(writeVector->buf, wanted);
			iWrittenFrames += bOk ? wanted : 0;
		}

		if (!bOk || wanted == iFrameSize) {
			goto next;
		}
		
		// corner case where one sample wraps around the buffer
		if (auto gap = writeVector->len - (wanted * iSampleSize); gap != 0) {
			writeVector->buf += wanted * iSampleSize;
			bOk = mix(spareSample, 1);
			if (bOk) {
				memcpy(writeVector->buf, spareSample, gap);
				writeVector++;
				memcpy(writeVector->buf, spareSample + gap, iSampleSize - gap);
				wanted++;
				iWrittenFrames++;
			} else {
				goto next;
			}
			writeVector->buf += iSampleSize - gap;
		} else {
			writeVector++;
		}

		if (wanted == iFrameSize) {
			goto next;
		}

		bOk = mix(writeVector->buf, iFrameSize - wanted);
		iWrittenFrames += bOk ? (iFrameSize - wanted) : 0;
next:
		jas->ringbufferWriteSpace(buffer, iWrittenFrames * iSampleSize);
		qmWait.unlock();
		qsSleep.acquire(1);
	} while (bReady);
}
