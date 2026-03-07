#include "spectrum.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <qbytearray.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qscopeguard.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format-utils.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/pod.h>

#include "../../core/logcat.hpp"
#include "connection.hpp"
#include "core.hpp"
#include "node.hpp"
#include "qml.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace qs::service::pipewire {

namespace {

QS_LOGGING_CATEGORY(logSpectrum, "quickshell.service.pipewire.spectrum", QtWarningMsg);

// In-place radix-2 Cooley-Tukey FFT for complex data of length n (must be power of 2).
void fft(std::complex<float>* data, int n) {
	// Bit-reversal permutation
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) std::swap(data[i], data[j]);
	}

	// Butterfly stages
	for (int len = 2; len <= n; len <<= 1) {
		auto angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
		std::complex<float> wn(std::cos(angle), std::sin(angle));
		for (int i = 0; i < n; i += len) {
			std::complex<float> w(1.0f, 0.0f);
			int half = len / 2;
			for (int j = 0; j < half; j++) {
				auto u = data[i + j];
				auto v = data[i + j + half] * w;
				data[i + j] = u + v;
				data[i + j + half] = u - v;
				w *= wn;
			}
		}
	}
}

} // anonymous namespace

// --- PwSpectrumStream: manages the Pipewire capture stream ---

class PwSpectrumStream {
public:
	PwSpectrumStream(PwAudioSpectrum* spectrum, PwNode* node)
	    : spectrum(spectrum), node(node) {}

	~PwSpectrumStream() { this->destroy(); }
	Q_DISABLE_COPY_MOVE(PwSpectrumStream);

	bool start();
	void destroy();

private:
	static const pw_stream_events EVENTS;
	static void onProcess(void* data);
	static void onParamChanged(void* data, uint32_t id, const spa_pod* param);
	static void
	onStateChanged(void* data, pw_stream_state oldState, pw_stream_state state, const char* error);
	static void onDestroy(void* data);

	void handleProcess();
	void handleParamChanged(uint32_t id, const spa_pod* param);

	PwAudioSpectrum* spectrum = nullptr;
	PwNode* node = nullptr;
	pw_stream* stream = nullptr;
	SpaHook listener;
	spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_UNKNOWN);
	bool formatReady = false;
};

const pw_stream_events PwSpectrumStream::EVENTS = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = &PwSpectrumStream::onDestroy,
    .state_changed = &PwSpectrumStream::onStateChanged,
    .param_changed = &PwSpectrumStream::onParamChanged,
    .process = &PwSpectrumStream::onProcess,
};

bool PwSpectrumStream::start() {
	auto* core = PwConnection::instance()->registry.core;
	if (core == nullptr || !core->isValid()) {
		qCWarning(logSpectrum) << "Cannot start spectrum stream: pipewire core not ready.";
		return false;
	}

	auto target =
	    QByteArray::number(this->node->objectSerial ? this->node->objectSerial : this->node->id);

	// clang-format off
	auto* props = pw_properties_new(
	    PW_KEY_MEDIA_TYPE, "Audio",
	    PW_KEY_MEDIA_CATEGORY, "Monitor",
	    PW_KEY_MEDIA_NAME, "Spectrum analyzer",
	    PW_KEY_APP_NAME, "Quickshell Spectrum",
	    PW_KEY_STREAM_MONITOR, "true",
	    PW_KEY_STREAM_CAPTURE_SINK, this->node->type.testFlags(PwNodeType::Sink) ? "true" : "false",
	    PW_KEY_TARGET_OBJECT, target.constData(),
	    nullptr
	);
	// clang-format on

	if (props == nullptr) {
		qCWarning(logSpectrum) << "Failed to create properties for spectrum stream.";
		return false;
	}

	this->stream = pw_stream_new(core->core, "quickshell-spectrum", props);
	if (this->stream == nullptr) {
		qCWarning(logSpectrum) << "Failed to create spectrum stream.";
		return false;
	}

	pw_stream_add_listener(this->stream, &this->listener.hook, &PwSpectrumStream::EVENTS, this);

	auto buffer = std::array<quint8, 512> {};
	auto builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size()); // NOLINT

	auto params = std::array<const spa_pod*, 1> {};
	auto raw = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32);
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);

	auto flags =
	    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
	auto res =
	    pw_stream_connect(this->stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params.data(), 1);

	if (res < 0) {
		qCWarning(logSpectrum) << "Failed to connect spectrum stream:" << res;
		this->destroy();
		return false;
	}

	return true;
}

void PwSpectrumStream::destroy() {
	if (this->stream == nullptr) return;
	this->listener.remove();
	pw_stream_destroy(this->stream);
	this->stream = nullptr;
	this->formatReady = false;
}

void PwSpectrumStream::onProcess(void* data) {
	static_cast<PwSpectrumStream*>(data)->handleProcess(); // NOLINT
}

void PwSpectrumStream::onParamChanged(void* data, uint32_t id, const spa_pod* param) {
	static_cast<PwSpectrumStream*>(data)->handleParamChanged(id, param); // NOLINT
}

void PwSpectrumStream::onStateChanged(
    void* data,
    pw_stream_state oldState,
    pw_stream_state state,
    const char* error
) {
	Q_UNUSED(data);
	Q_UNUSED(oldState);
	if (state == PW_STREAM_STATE_ERROR) {
		if (error != nullptr) {
			qCWarning(logSpectrum) << "Spectrum stream error:" << error;
		} else {
			qCWarning(logSpectrum) << "Spectrum stream error.";
		}
	}
}

void PwSpectrumStream::onDestroy(void* data) {
	auto* self = static_cast<PwSpectrumStream*>(data); // NOLINT
	self->stream = nullptr;
	self->listener.remove();
	self->formatReady = false;
}

void PwSpectrumStream::handleParamChanged(uint32_t id, const spa_pod* param) {
	if (param == nullptr || id != SPA_PARAM_Format) return;

	auto info = spa_audio_info {};
	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
	if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	auto raw = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_UNKNOWN); // NOLINT
	if (spa_format_audio_raw_parse(param, &raw) < 0) return;

	if (raw.format != SPA_AUDIO_FORMAT_F32) {
		qCWarning(logSpectrum) << "Unsupported spectrum format:" << raw.format;
		this->formatReady = false;
		return;
	}

	this->format = raw;
	this->formatReady = raw.channels > 0;

	if (this->formatReady) {
		this->spectrum->mSampleRate = static_cast<int>(raw.rate);
		this->spectrum->computeBarBins();
		qCDebug(logSpectrum) << "Spectrum format:" << raw.rate << "Hz," << raw.channels << "ch";
	}
}

void PwSpectrumStream::handleProcess() {
	if (!this->formatReady || this->stream == nullptr) return;

	auto* buffer = pw_stream_dequeue_buffer(this->stream);
	if (buffer == nullptr) return;
	auto requeue = qScopeGuard([&, this] { pw_stream_queue_buffer(this->stream, buffer); });

	auto* spaBuffer = buffer->buffer;
	if (spaBuffer == nullptr || spaBuffer->n_datas < 1) return;

	auto* data = &spaBuffer->datas[0]; // NOLINT
	if (data->data == nullptr || data->chunk == nullptr) return;

	auto channelCount = static_cast<int>(this->format.channels);
	if (channelCount <= 0) return;

	const auto* base = static_cast<const quint8*>(data->data) + data->chunk->offset; // NOLINT
	const auto* samples = reinterpret_cast<const float*>(base);                       // NOLINT
	auto totalSamples = static_cast<int>(data->chunk->size / sizeof(float));
	auto frameCount = totalSamples / channelCount;

	if (frameCount <= 0) return;

	// Mix to mono using a static buffer to avoid per-callback allocation
	static thread_local std::vector<float> mono; // NOLINT
	mono.resize(frameCount);

	if (channelCount == 1) {
		std::copy(samples, samples + frameCount, mono.begin());
	} else {
		auto invChannels = 1.0f / static_cast<float>(channelCount);
		for (int i = 0; i < frameCount; i++) {
			float sum = 0.0f;
			for (int c = 0; c < channelCount; c++) {
				sum += samples[i * channelCount + c]; // NOLINT
			}
			mono[i] = sum * invChannels;
		}
	}

	this->spectrum->feedSamples(mono.data(), frameCount);
}

// --- PwAudioSpectrum ---

PwAudioSpectrum::PwAudioSpectrum(QObject* parent): QObject(parent) {
	this->initProcessing();
	QObject::connect(&this->mFrameTimer, &QTimer::timeout, this, &PwAudioSpectrum::onFrameTick);
}

PwAudioSpectrum::~PwAudioSpectrum() {
	delete this->mStream;
	this->mStream = nullptr;
}

PwNodeIface* PwAudioSpectrum::node() const { return this->mNode; }

void PwAudioSpectrum::setNode(PwNodeIface* node) {
	if (node == this->mNode) return;

	if (this->mNode != nullptr) {
		QObject::disconnect(this->mNode, nullptr, this, nullptr);
	}

	if (node != nullptr) {
		QObject::connect(node, &QObject::destroyed, this, &PwAudioSpectrum::onNodeDestroyed);
	}

	this->mNode = node;
	this->mNodeRef.setObject(node != nullptr ? node->node() : nullptr);
	qCDebug(logSpectrum) << "setNode:" << (node != nullptr ? "valid node" : "null");
	this->rebuildStream();
	emit this->nodeChanged();
}

bool PwAudioSpectrum::isEnabled() const { return this->mEnabled; }

void PwAudioSpectrum::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	qCDebug(logSpectrum) << "setEnabled:" << enabled;
	this->rebuildStream();
	emit this->enabledChanged();
}

int PwAudioSpectrum::barCount() const { return this->mBarCount; }

void PwAudioSpectrum::setBarCount(int count) {
	count = std::clamp(count, 1, FFT_SIZE / 2);
	if (count == this->mBarCount) return;
	this->mBarCount = count;
	this->initProcessing();
	emit this->barCountChanged();
}

int PwAudioSpectrum::frameRate() const { return this->mFrameRate; }

void PwAudioSpectrum::setFrameRate(int rate) {
	rate = std::clamp(rate, 1, 240);
	if (rate == this->mFrameRate) return;
	this->mFrameRate = rate;
	if (this->mFrameTimer.isActive()) {
		this->mFrameTimer.setInterval(1000 / this->mFrameRate);
	}
	emit this->frameRateChanged();
}

int PwAudioSpectrum::lowerCutoff() const { return this->mLowerCutoff; }

void PwAudioSpectrum::setLowerCutoff(int freq) {
	freq = std::max(1, freq);
	if (freq == this->mLowerCutoff) return;
	this->mLowerCutoff = freq;
	this->computeBarBins();
	emit this->lowerCutoffChanged();
}

int PwAudioSpectrum::upperCutoff() const { return this->mUpperCutoff; }

void PwAudioSpectrum::setUpperCutoff(int freq) {
	freq = std::max(this->mLowerCutoff + 1, freq);
	if (freq == this->mUpperCutoff) return;
	this->mUpperCutoff = freq;
	this->computeBarBins();
	emit this->upperCutoffChanged();
}

qreal PwAudioSpectrum::noiseReduction() const { return this->mNoiseReduction; }

void PwAudioSpectrum::setNoiseReduction(qreal amount) {
	amount = std::clamp(amount, 0.0, 1.0);
	if (qFuzzyCompare(amount, this->mNoiseReduction)) return;
	this->mNoiseReduction = amount;
	emit this->noiseReductionChanged();
}

bool PwAudioSpectrum::smoothing() const { return this->mSmoothing; }

void PwAudioSpectrum::setSmoothing(bool enabled) {
	if (enabled == this->mSmoothing) return;
	this->mSmoothing = enabled;
	emit this->smoothingChanged();
}

void PwAudioSpectrum::onNodeDestroyed() {
	this->mNode = nullptr;
	this->mNodeRef.setObject(nullptr);
	this->rebuildStream();
	emit this->nodeChanged();
}

void PwAudioSpectrum::rebuildStream() {
	delete this->mStream;
	this->mStream = nullptr;

	auto* node = this->mNodeRef.object();
	qCDebug(logSpectrum) << "rebuildStream: enabled=" << this->mEnabled
	                     << "node=" << (node != nullptr)
	                     << "audioFlag=" << (node != nullptr ? node->type.testFlags(PwNodeType::Audio) : false);
	if (!this->mEnabled || node == nullptr || !node->type.testFlags(PwNodeType::Audio)) {
		this->mFrameTimer.stop();
		if (!this->mValues.isEmpty()) {
			this->mValues = QList<float>(this->mBarCount, 0.0f);
			emit this->valuesChanged();
		}
		if (!this->mIdle) {
			this->mIdle = true;
			emit this->idleChanged();
		}
		return;
	}

	qCDebug(logSpectrum) << "rebuildStream: creating stream for node id=" << node->id;
	this->mStream = new PwSpectrumStream(this, node);
	if (!this->mStream->start()) {
		delete this->mStream;
		this->mStream = nullptr;
		return;
	}

	// Reset processing state
	this->mRingPos = 0;
	this->mRingFull = false;
	this->mIdleFrames = 0;
	this->mPeakAvg = 0.0f;
	this->mSensitivity = 0.01f;
	std::fill(this->mPrevBars.begin(), this->mPrevBars.end(), 0.0f);

	this->mFrameTimer.setInterval(1000 / this->mFrameRate);
	this->mFrameTimer.start();
	qCDebug(logSpectrum) << "rebuildStream: stream started, timer at" << (1000 / this->mFrameRate) << "ms";
}

void PwAudioSpectrum::initProcessing() {
	this->mRingBuffer.resize(FFT_SIZE, 0.0f);
	this->mRingPos = 0;
	this->mRingFull = false;
	this->mFftBuf.resize(FFT_SIZE);

	// Pre-compute Hann window
	this->mWindow.resize(FFT_SIZE);
	for (int i = 0; i < FFT_SIZE; i++) {
		this->mWindow[i] = 0.5f
		    * (1.0f
		       - std::cos(
		           2.0f * std::numbers::pi_v<float> * static_cast<float>(i)
		           / static_cast<float>(FFT_SIZE - 1)
		       ));
	}

	this->mPrevBars.assign(this->mBarCount, 0.0f);
	this->mValues = QList<float>(this->mBarCount, 0.0f);
	this->computeBarBins();
}

void PwAudioSpectrum::computeBarBins() {
	this->mBarBinLow.resize(this->mBarCount);
	this->mBarBinHigh.resize(this->mBarCount);

	auto fLow = static_cast<float>(this->mLowerCutoff);
	auto fHigh =
	    static_cast<float>(std::min(this->mUpperCutoff, this->mSampleRate / 2));
	auto ratio = fHigh / fLow;
	auto fftBins = FFT_SIZE / 2;

	for (int i = 0; i < this->mBarCount; i++) {
		auto barFreqLow = fLow
		    * std::pow(ratio, static_cast<float>(i) / static_cast<float>(this->mBarCount));
		auto barFreqHigh = fLow
		    * std::pow(
		        ratio,
		        static_cast<float>(i + 1) / static_cast<float>(this->mBarCount)
		    );

		auto binLow = static_cast<int>(
		    std::ceil(barFreqLow * static_cast<float>(FFT_SIZE) / static_cast<float>(this->mSampleRate))
		);
		auto binHigh = static_cast<int>(
		    std::floor(barFreqHigh * static_cast<float>(FFT_SIZE) / static_cast<float>(this->mSampleRate))
		);

		binLow = std::clamp(binLow, 1, fftBins);
		binHigh = std::clamp(binHigh, binLow, fftBins);

		this->mBarBinLow[i] = binLow;
		this->mBarBinHigh[i] = binHigh;
	}
}

void PwAudioSpectrum::feedSamples(const float* monoSamples, int count) {
	bool wasFull = this->mRingFull;
	for (int i = 0; i < count; i++) {
		this->mRingBuffer[this->mRingPos] = monoSamples[i]; // NOLINT
		this->mRingPos = (this->mRingPos + 1) % FFT_SIZE;
		if (this->mRingPos == 0) this->mRingFull = true;
	}
	if (!wasFull && this->mRingFull) {
		qCDebug(logSpectrum) << "Ring buffer full, FFT processing will begin";
	}
}

void PwAudioSpectrum::onFrameTick() { this->processFrame(); }

void PwAudioSpectrum::processFrame() {
	if (!this->mRingFull) return;

	// 1. Copy ring buffer to FFT buffer with Hann window applied
	for (int i = 0; i < FFT_SIZE; i++) {
		int idx = (this->mRingPos + i) % FFT_SIZE;
		this->mFftBuf[i] = {this->mRingBuffer[idx] * this->mWindow[i], 0.0f};
	}

	// 2. Compute FFT
	fft(this->mFftBuf.data(), FFT_SIZE);

	// 3. Map FFT bins to bars using logarithmic frequency distribution
	//    For each bar, take the peak magnitude across its frequency range.
	std::vector<float> bars(this->mBarCount, 0.0f);
	for (int i = 0; i < this->mBarCount; i++) {
		float maxMag = 0.0f;
		for (int bin = this->mBarBinLow[i]; bin <= this->mBarBinHigh[i]; bin++) {
			float mag = std::abs(this->mFftBuf[bin]);
			maxMag = std::max(maxMag, mag);
		}
		bars[i] = maxMag;
	}

	// 4. Frequency weighting: perceptual boost for lower frequencies.
	//    Low-frequency bars cover fewer FFT bins and need compensation.
	for (int i = 0; i < this->mBarCount; i++) {
		float weight = 1.0f
		    + 0.5f * static_cast<float>(this->mBarCount - i)
		          / static_cast<float>(this->mBarCount);
		bars[i] *= weight;
	}

	// 5. Noise gate: subtract a fixed threshold from raw magnitudes before
	//    auto-sensitivity can amplify noise. The threshold scales with FFT size
	//    so it stays proportional to the magnitude range.
	auto nrFactor = static_cast<float>(this->mNoiseReduction);
	float noiseGate = nrFactor * static_cast<float>(FFT_SIZE) * 0.00005f;
	for (auto& bar: bars) {
		bar = std::max(0.0f, bar - noiseGate);
	}

	// 6. Auto-sensitivity: dynamically adjust gain so the output fills the 0-1 range
	//    regardless of input volume. When signal drops below a minimum threshold,
	//    sensitivity decays to prevent amplifying noise.
	float currentPeak = *std::max_element(bars.begin(), bars.end());
	this->mPeakAvg = this->mPeakAvg * 0.95f + currentPeak * 0.05f;

	constexpr float MIN_SIGNAL = 0.5f;
	if (this->mPeakAvg > MIN_SIGNAL) {
		float target = 0.8f / this->mPeakAvg;
		this->mSensitivity = this->mSensitivity * 0.9f + target * 0.1f;
		this->mSensitivity = std::clamp(this->mSensitivity, 0.001f, 20.0f);
	} else {
		this->mSensitivity *= 0.95f;
		this->mSensitivity = std::max(this->mSensitivity, 0.001f);
	}

	for (auto& bar: bars) {
		bar *= this->mSensitivity;
	}

	// 7. Monstercat-style spatial smoothing: each bar is influenced by its neighbors
	//    to create a smoother, less jittery spectrum.
	if (this->mSmoothing) {
		constexpr float SMOOTH_FACTOR = 0.64f;
		for (int i = 1; i < this->mBarCount; i++) {
			bars[i] = std::max(bars[i], bars[i - 1] * SMOOTH_FACTOR);
		}
		for (int i = this->mBarCount - 2; i >= 0; i--) {
			bars[i] = std::max(bars[i], bars[i + 1] * SMOOTH_FACTOR);
		}
	}

	// 8. Temporal smoothing: bars rise and fall smoothly for fluid animation.
	//    Rise is fast but not instant; fall is slower. Both rates are frame-rate independent.
	float fall = std::pow(0.05f, 1.0f / (0.8f * static_cast<float>(this->mFrameRate)));
	float rise = std::pow(0.1f, 1.0f / (0.25f * static_cast<float>(this->mFrameRate)));
	for (int i = 0; i < this->mBarCount; i++) {
		if (bars[i] >= this->mPrevBars[i]) {
			this->mPrevBars[i] = this->mPrevBars[i] * rise + bars[i] * (1.0f - rise);
		} else {
			this->mPrevBars[i] *= fall;
		}
		bars[i] = this->mPrevBars[i];
	}

	// 9. Clamp to 0-1
	for (auto& bar: bars) {
		bar = std::clamp(bar, 0.0f, 1.0f);
	}

	// 10. Idle detection: if all bars are near zero for several consecutive frames,
	//     stop emitting updates to save GPU rendering.
	bool allZero = std::all_of(bars.begin(), bars.end(), [](float v) { return v < 0.01f; });
	if (allZero) {
		this->mIdleFrames++;
		if (this->mIdleFrames >= IDLE_THRESHOLD) {
			if (!this->mIdle) {
				this->mIdle = true;
				this->mValues = QList<float>(this->mBarCount, 0.0f);
				emit this->valuesChanged();
				emit this->idleChanged();
			}
			return;
		}
	} else {
		this->mIdleFrames = 0;
		if (this->mIdle) {
			this->mIdle = false;
			emit this->idleChanged();
		}
	}

	// 11. Emit updated values to QML
	QList<float> newValues(this->mBarCount);
	for (int i = 0; i < this->mBarCount; i++) {
		newValues[i] = bars[i];
	}

	if (newValues != this->mValues) {
		this->mValues = std::move(newValues);
		emit this->valuesChanged();
	}
}

} // namespace qs::service::pipewire

#pragma GCC diagnostic pop
