/******************************************************************************
    AOOpenAL.cpp: description
    Copyright (C) 2012 Wang Bin <wbsecg1@gmail.com>
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/


#include "QtAV/AOOpenAL.h"
#include "private/AudioOutput_p.h"
#include <QtCore/QVector>
#include <QtCore/QElapsedTimer>
#include <string>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

namespace QtAV {

// TODO: planar
static ALenum audioFormatToAL(const AudioFormat& fmt)
{
    ALenum format = 0;
    if (fmt.sampleFormat() == AudioFormat::SampleFormat_Unsigned8) {
        if (fmt.channels() == 1)
                format = AL_FORMAT_MONO8;
        else if (fmt.channels() == 2)
            format = AL_FORMAT_STEREO8;
        else if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
            if (fmt.channels() == 4)
                format = alGetEnumValue("AL_FORMAT_QUAD8");
            else if (fmt.channels() == 6)
                format = alGetEnumValue("AL_FORMAT_51CHN8");
            else if (fmt.channels() == 7)
                format = alGetEnumValue("AL_FORMAT_71CHN8");
            else if (fmt.channels() == 8)
                format = alGetEnumValue("AL_FORMAT_81CHN8");
        }
    } else if (fmt.sampleFormat() == AudioFormat::SampleFormat_Signed16) {
        if (fmt.channels() == 1)
            format = AL_FORMAT_MONO16;
        else if (fmt.channels() == 2)
            format = AL_FORMAT_STEREO16;
        else if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
            if (fmt.channels() == 4)
                format = alGetEnumValue("AL_FORMAT_QUAD16");
            else if (fmt.channels() == 6)
                format = alGetEnumValue("AL_FORMAT_51CHN16");
            else if (fmt.channels() == 7)
                format = alGetEnumValue("AL_FORMAT_61CHN16");
            else if (fmt.channels() == 8)
                format = alGetEnumValue("AL_FORMAT_71CHN16");
        }
    } else if (fmt.sampleFormat() == AudioFormat::SampleFormat_Float) {
        if (alIsExtensionPresent("AL_EXT_float32")) {
            if (fmt.channels() == 1)
                format = alGetEnumValue("AL_FORMAT_MONO_FLOAT32");
            else if (fmt.channels() == 2)
                format = alGetEnumValue("AL_FORMAT_STEREO_FLOAT32");
            else if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
                if (fmt.channels() == 4)
                    format = alGetEnumValue("AL_FORMAT_QUAD32");
                else if (fmt.channels() == 6)
                    format = alGetEnumValue("AL_FORMAT_51CHN32");
                else if (fmt.channels() == 7)
                    format = alGetEnumValue("AL_FORMAT_61CHN32");
                else if (fmt.channels() == 8)
                    format = alGetEnumValue("AL_FORMAT_71CHN32");
            }
        }
    } else if (fmt.sampleFormat() == AudioFormat::SampleFormat_Double) {
        if (alIsExtensionPresent("AL_EXT_double")) {
            if (fmt.channels() == 1)
                format = alGetEnumValue("AL_FORMAT_MONO_DOUBLE_EXT");
            else if (fmt.channels() == 2)
                format = alGetEnumValue("AL_FORMAT_STEREO_DOUBLE_EXT");
        }
    }
    if (format == 0) {
        qWarning("AOOpenAL Error: No OpenAL format available for audio data format %s."
                 , qPrintable(fmt.sampleFormatName())
                 , qPrintable(fmt.channelLayoutName()));
    }
    return format;
}

#define ALERROR_RETURN_F(alf) { \
    alf; \
    ALenum err = alGetError(); \
    if (err != AL_NO_ERROR) { \
        qWarning("AOOpenAL Error (%d/%d) %s : %s", err, AL_NO_ERROR, #alf, alGetString(err)); \
        return false; \
    }}

const int kBufferSize = 4096*2;
const int kBufferCount = 3;

class  AOOpenALPrivate : public AudioOutputPrivate
{
public:
    AOOpenALPrivate()
        : AudioOutputPrivate()
        , format(AL_FORMAT_STEREO16)
        , source(0)
        , device(0)
        , state(0)
        , last_duration(0)
    {
    }
    ~AOOpenALPrivate() {
    }

    ALenum format;
    ALuint buffer[kBufferCount];
    ALuint source;
    ALCdevice *device;
    ALCcontext *context;
    ALint state;
    QElapsedTimer time;
    qint64 last_duration;
    QMutex mutex;
    QWaitCondition cond;
};

AOOpenAL::AOOpenAL()
    :AudioOutput(*new AOOpenALPrivate())
{
}

AOOpenAL::~AOOpenAL()
{
    close();
}

bool AOOpenAL::open()
{
    DPTR_D(AOOpenAL);
    d.available = false; // TODO: d.reset()
    QVector<std::string> _devices;
    const char *p = NULL;
    if (alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT"))
        p = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    else if (alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT"))
        p = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
    while (p && *p) {
        _devices.push_back(p);
        p += _devices.last().size() + 1;
    }
    qDebug("%d OpenAL devices available: %d", _devices.size());
    for (int i = 0; i < _devices.size(); i++) {
        qDebug("device %d: %s", i, _devices[i].c_str());
    }
    const ALCchar *default_device = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
    qDebug("AOOpenAL Opening default device: %s", default_device);
    if ((d.device = alcOpenDevice(NULL)) == NULL) { //or NULL to open default
        qWarning("AOOpenAL Failed to open sound device: %s", alcGetString(0, alcGetError(0)));
        return false;
    }
    qDebug("AOOpenAL creating context...");
    d.context = alcCreateContext(d.device, NULL);
    alcMakeContextCurrent(d.context);
    //alcProcessContext(d.context); //??
    ALCenum err = alcGetError(d.device);
    if (err != ALC_NO_ERROR) {
        qWarning("AOOpenAL Error: %s", alcGetString(d.device, err));
        return false;
    }
    //init params. move to another func?
    d.format = audioFormatToAL(audioFormat());
    alGenBuffers(kBufferCount, d.buffer);
    err = alGetError();
    if (err != AL_NO_ERROR) {
        alcMakeContextCurrent(NULL);
        alcDestroyContext(d.context);
        alcCloseDevice(d.device);
        return false;
    }
    alGenSources(1, &d.source);
    err = alGetError();
    if (err != AL_NO_ERROR) {
        alDeleteBuffers(kBufferCount, d.buffer);
        alcMakeContextCurrent(NULL);
        alcDestroyContext(d.context);
        alcCloseDevice(d.device);
        return false;
    }

    alSourcei(d.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(d.source, AL_ROLLOFF_FACTOR, 0);
    alSource3f(d.source, AL_POSITION, 0.0, 0.0, 0.0);
    alSource3f(d.source, AL_VELOCITY, 0.0, 0.0, 0.0);
    alListener3f(AL_POSITION, 0.0, 0.0, 0.0);
    qDebug("AOOpenAL open ok...");
    d.state = 0;
    d.available = true;
    return true;
}

bool AOOpenAL::close()
{
    if (!isAvailable())
        return true;
    DPTR_D(AOOpenAL);
    d.state = 0;
    d.available = false;
    do {
        alGetSourcei(d.source, AL_SOURCE_STATE, &d.state);
    } while (alGetError() == AL_NO_ERROR && d.state == AL_PLAYING);
    alDeleteSources(1, &d.source);
    alDeleteBuffers(kBufferCount, d.buffer);
    alcMakeContextCurrent(NULL);
    if (d.context) {
        alcDestroyContext(d.context);
        ALCenum err = alcGetError(d.device);
        if (err != ALC_NO_ERROR) { //ALC_INVALID_CONTEXT
            qWarning("AOOpenAL Failed to destroy context: %s", alcGetString(d.device, err));
            return false;
        }
    }
    if (d.device) {
        alcCloseDevice(d.device);
        ALCenum err = alcGetError(d.device);
        if (err != ALC_NO_ERROR) { //ALC_INVALID_DEVICE
            qWarning("AOOpenAL Failed to close device: %s", alcGetString(d.device, err));
            return false;
        }
    }
    return true;
}

QString AOOpenAL::name() const
{
    const ALCchar *name = alcGetString(d_func().device, ALC_DEVICE_SPECIFIER);
    return name;
}

// http://kcat.strangesoft.net/openal-tutorial.html
bool AOOpenAL::write()
{
    DPTR_D(AOOpenAL);
    if (d.data.isEmpty())
        return false;
    QMutexLocker lock(&d.mutex);
    Q_UNUSED(lock);
    if (d.state == 0) {
        //// Initial buffering
        alSourcef(d.source, AL_GAIN, d.vol);
        const char* b = d.data.constData();
        int remain = d.data.size();
        d.time.start();
        for (int i = 0; i < kBufferCount; ++i) {
            ALERROR_RETURN_F(alBufferData(d.buffer[i], d.format, d.data, d.data.size(), audioFormat().sampleRate()));
            alSourceQueueBuffers(d.source, 1, &d.buffer[i]);
            continue;
            if (remain <= 0)
                break;
            // TODO: buffer the whole data and kBufferCount times with the same data?
            alBufferData(d.buffer[i], d.format, b, qMin(remain, kBufferSize), audioFormat().sampleRate());
            alSourceQueueBuffers(d.source, 1, &d.buffer[i]);
            b += kBufferSize;
            remain -= kBufferSize;
        }
        //alSourceQueueBuffers(d.source, 3, d.buffer);
        alGetSourcei(d.source, AL_SOURCE_STATE, &d.state);
        ALERROR_RETURN_F(alSourcePlay(d.source));
        d.last_duration = audioFormat().durationForBytes(d.data.size());
        return true;
    }
    qint64 dt = d.last_duration - d.time.elapsed();
    d.last_duration = audioFormat().durationForBytes(d.data.size());
    if (dt > 8LL)
        d.cond.wait(&d.mutex, (ulong)(dt-2LL));

    ALint processed = 0;
    alGetSourcei(d.source, AL_BUFFERS_PROCESSED, &processed);
    if (processed <= 0) {
        alGetSourcei(d.source, AL_SOURCE_STATE, &d.state);
        if (d.state != AL_PLAYING) {
            ALERROR_RETURN_F(alSourcePlay(d.source));
        }
        //return false;
    }
    const char* b = d.data.constData();
    int remain = d.data.size();
    d.time.restart();
    while (processed--) {
        if (remain <= 0)
            break;
        ALuint buf;
        //unqueues a set of buffers attached to a source
        alSourceUnqueueBuffers(d.source, 1, &buf);
        ALERROR_RETURN_F(alBufferData(buf, d.format, b, qMin(remain, kBufferSize), audioFormat().sampleRate()));
        alSourceQueueBuffers(d.source, 1, &buf);
        b += kBufferSize;
        remain -= kBufferSize;

//        qDebug("remain: %d", remain);
        ALenum err = alGetError();
        if (err != AL_NO_ERROR) { //return ?
            qWarning("AOOpenAL Error: %s ---remain=%d", alGetString(err), remain);
            return false;
        }
    }
    alGetSourcei(d.source, AL_SOURCE_STATE, &d.state);
    if (d.state != AL_PLAYING) {
        qDebug("AOOpenAL: !AL_PLAYING alSourcePlay");
        ALERROR_RETURN_F(alSourcePlay(d.source));
    }
    return true;
}

} //namespace QtAV
