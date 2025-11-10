/* Copyright 2019 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "PaContext.h"
#include "Params.h"
#include "Chunks.h"
#include <portaudio.h>
#include <thread>

namespace streampunk {

int PaCallback(const void *input, void *output, unsigned long frameCount, 
               const PaStreamCallbackTimeInfo *timeInfo, 
               PaStreamCallbackFlags statusFlags, void *userData) {
  PaContext *paContext = (PaContext *)userData;
  double inTimestamp = timeInfo->inputBufferAdcTime > 0.0 ?
    timeInfo->inputBufferAdcTime :
    paContext->getCurTime() - paContext->getInLatency(); // approximation for timestamp of first sample
  double outTimestamp = timeInfo->outputBufferDacTime;
  paContext->checkStatus(statusFlags);
  // printf("PaCallback output %p, frameCount %d\n", output, frameCount);
  int inRetCode = paContext->hasInput() && paContext->readPaBuffer(input, frameCount, inTimestamp) ? paContinue : paComplete;
  int outRetCode = paContext->hasOutput() && paContext->fillPaBuffer(output, frameCount) ? paContinue : paComplete;
  return ((inRetCode == paComplete) && (outRetCode == paComplete)) ? paComplete : paContinue;
}

PaContext::PaContext(napi_env env, napi_value inOptions, napi_value outOptions)
  : mInOptions(checkOptions(env, inOptions) ? std::make_shared<AudioOptions>(env, inOptions) : std::shared_ptr<AudioOptions>()), 
    mOutOptions(checkOptions(env, outOptions) ? std::make_shared<AudioOptions>(env, outOptions) : std::shared_ptr<AudioOptions>()),
    mInChunks(new Chunks(mInOptions ? mInOptions->maxQueue() : 0)),
    mOutChunks(new Chunks(mOutOptions ? mOutOptions->maxQueue() : 0)),
    mStream(nullptr) {

  PaError errCode = Pa_Initialize();
  if (errCode != paNoError) {
    std::string err = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  if (!mInOptions && !mOutOptions) {
    napi_throw_error(env, nullptr, "Input and/or Output options must be specified");
    return;
  }

  if (mInOptions && mOutOptions &&
      (mInOptions->sampleRate() != mOutOptions->sampleRate())) {
    napi_throw_error(env, nullptr, "Input and Output sample rates must match");
    return;
  }    

  printf("%s\n", Pa_GetVersionText());
  if (mInOptions)
    printf("Input %s\n", mInOptions->toString().c_str());
  if (mOutOptions)
    printf("Output %s\n", mOutOptions->toString().c_str());

  double sampleRate;
  PaStreamParameters inParams;
  memset(&inParams, 0, sizeof(PaStreamParameters));
  if (mInOptions)
    setParams(env, /*isInput*/true, mInOptions, inParams, sampleRate);

  PaStreamParameters outParams;
  memset(&outParams, 0, sizeof(PaStreamParameters));
  if (mOutOptions)
    setParams(env, /*isInput*/false, mOutOptions, outParams, sampleRate);

  uint32_t framesPerBuffer = paFramesPerBufferUnspecified;
  #ifdef __arm__
  framesPerBuffer = 256;
  #endif
  uint32_t inFramesPerBuffer = mInOptions ? mInOptions->framesPerBuffer() : 0;
  uint32_t outFramesPerBuffer = mOutOptions ? mOutOptions->framesPerBuffer() : 0;
  if (!((0 == inFramesPerBuffer) && (0 == outFramesPerBuffer)))
    framesPerBuffer = std::max<uint32_t>(inFramesPerBuffer, outFramesPerBuffer);

  errCode = Pa_IsFormatSupported(mInOptions ? &inParams : NULL, mOutOptions ? &outParams : NULL, sampleRate);
  if (errCode != paFormatIsSupported) {
    std::string err = std::string("Format not supported: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  errCode = Pa_OpenStream(&mStream,
                          mInOptions ? &inParams : NULL,
                          mOutOptions ? &outParams : NULL,
                          sampleRate, framesPerBuffer,
                          paNoFlag, PaCallback, this);
  if (errCode != paNoError) {
    std::string err = std::string("Could not open stream: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  const PaStreamInfo *streamInfo = Pa_GetStreamInfo(mStream);
  mInLatency = streamInfo->inputLatency;
}

void PaContext::start(napi_env env) {
  PaError errCode = Pa_StartStream(mStream);
  if (errCode != paNoError) {
    std::string err = std::string("Could not start stream: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }
}

void PaContext::stop(eStopFlag flag) {
  if (eStopFlag::ABORT == flag)
    Pa_AbortStream(mStream);
  else
    Pa_StopStream(mStream);
  Pa_CloseStream(mStream);
  Pa_Terminate();
}

std::shared_ptr<Chunk> PaContext::pullInChunk(uint32_t numBytes, bool &finished) {
  std::shared_ptr<Memory> result = Memory::makeNew(numBytes);
  finished = false;
  double timeStamp = 0.0;
  uint32_t bytesRead = fillBuffer(result->buf(), numBytes, timeStamp, mInChunks, finished, /*isInput*/true);
  if (bytesRead != numBytes) {
    if (0 == bytesRead)
      result = std::shared_ptr<Memory>();
    else {
      std::shared_ptr<Memory> trimResult = Memory::makeNew(bytesRead);
      memcpy(trimResult->buf(), result->buf(), bytesRead);
      result = trimResult;
    }
  }

  return std::make_shared<Chunk>(result, timeStamp);
}

void PaContext::pushOutChunk(std::shared_ptr<Chunk> chunk) {
  mOutChunks->push(chunk);
}

void PaContext::checkStatus(uint32_t statusFlags) {
  if (statusFlags) {
    std::string err = std::string("portAudio status - ");
    if (statusFlags & paInputUnderflow)
      err += "input underflow ";
    if (statusFlags & paInputOverflow)
      err += "input overflow ";
    if (statusFlags & paOutputUnderflow)
      err += "output underflow ";
    if (statusFlags & paOutputOverflow)
      err += "output overflow ";
    if (statusFlags & paPrimingOutput)
      err += "priming output ";

    std::lock_guard<std::mutex> lk(m);
    mErrStr = err;
  }
}

bool PaContext::getErrStr(std::string& errStr, bool isInput) {
  std::lock_guard<std::mutex> lk(m);
  std::shared_ptr<AudioOptions> options = isInput ? mInOptions : mOutOptions;
  if (options->closeOnError()) // propagate the error back to the stream handler
    errStr = mErrStr;
  else if (mErrStr.length())
    printf("AudioIO: %s\n", mErrStr.c_str());
  mErrStr.clear();
  return !errStr.empty();
}

void PaContext::quit() {
  if (mInOptions)
    mInChunks->quit();
  if (mOutOptions) {
    mOutChunks->quit();
    mOutChunks->waitDone();
  }
  // wait for next PaCallback to run
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool PaContext::readPaBuffer(const void *srcBuf, uint32_t frameCount, double inTimestamp) {
  uint32_t bytesAvailable = frameCount * mInOptions->channelCount() * mInOptions->sampleBits() / 8;
  std::shared_ptr<Memory> chunk = Memory::makeNew(bytesAvailable);
  memcpy(chunk->buf(), srcBuf, bytesAvailable);
  mInChunks->push(std::make_shared<Chunk>(chunk, inTimestamp));
  return true;
}

bool PaContext::fillPaBuffer(void *dstBuf, uint32_t frameCount) {
  uint32_t bytesRemaining = frameCount * mOutOptions->channelCount() * mOutOptions->sampleBits() / 8;
  bool finished = false;
  double timeStamp = 0.0;
  fillBuffer((uint8_t *)dstBuf, bytesRemaining, timeStamp, mOutChunks, finished, /*isInput*/false);
  return !finished;
}

double PaContext::getCurTime() const  { 
  return Pa_GetStreamTime(mStream);
}

// private
uint32_t PaContext::fillBuffer(uint8_t *buf, uint32_t numBytes, double &timeStamp,
                               std::shared_ptr<Chunks> chunks,
                               bool &finished, bool isInput) {
  uint32_t bufOff = 0;
  timeStamp = 0.0;
  while (numBytes) {
    if (!chunks->curBuf() || (chunks->curBuf() && (chunks->curBytes() == chunks->curOffset()))) {
      chunks->waitNext();
      if (!chunks->curBuf()) {
        printf("Finishing %s - %d bytes not available to fill the last buffer\n", isInput ? "input" : "output", numBytes);
        memset(buf + bufOff, 0, numBytes);
        finished = true;
        break;
      }
    }
    if ((0 == bufOff) && isInput) {
      // offset the chunk timestamp by the chunk offset
      double timeOffset = (double)chunks->curOffset() / mInOptions->channelCount() / (mInOptions->sampleBits() / 8) / mInOptions->sampleRate();
      timeStamp = chunks->curTs() + timeOffset;
    }

    uint32_t curBytes = std::min<uint32_t>(numBytes, chunks->curBytes() - chunks->curOffset());
    void *srcBuf = chunks->curBuf() + chunks->curOffset();
    memcpy(buf + bufOff, srcBuf, curBytes);

    bufOff += curBytes;
    chunks->incOffset(curBytes);
    numBytes -= curBytes;
  }

  return bufOff;
}

void PaContext::setParams(napi_env env, bool isInput, 
                          std::shared_ptr<AudioOptions> options, 
                          PaStreamParameters &params, double &sampleRate) {
  int32_t deviceID = (int32_t)options->deviceID();
  if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
    params.device = (PaDeviceIndex)deviceID;
  else
    params.device = isInput ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
  if (params.device == paNoDevice) {
    napi_throw_error(env, nullptr, "No default device");
    return;
  }  

  printf("%s device name is %s\n", isInput?"Input":"Output", Pa_GetDeviceInfo(params.device)->name);

  params.channelCount = options->channelCount();
  int maxChannels = isInput ? Pa_GetDeviceInfo(params.device)->maxInputChannels : Pa_GetDeviceInfo(params.device)->maxOutputChannels;
  if (params.channelCount > maxChannels) {
    napi_throw_error(env, nullptr, "Channel count exceeds maximum number of channels for device");
    return;
  }

  uint32_t sampleFormat = options->sampleFormat();
  switch(sampleFormat) {
  case 1: params.sampleFormat = paFloat32; break;
  case 8: params.sampleFormat = paInt8; break;
  case 16: params.sampleFormat = paInt16; break;
  case 24: params.sampleFormat = paInt24; break;
  case 32: params.sampleFormat = paInt32; break;
  default: {
      napi_throw_error(env, nullptr, "Invalid sampleFormat");
      return;
    }
  }

  params.suggestedLatency = isInput ? Pa_GetDeviceInfo(params.device)->defaultLowInputLatency : 
                                      Pa_GetDeviceInfo(params.device)->defaultLowOutputLatency;
  params.hostApiSpecificStreamInfo = NULL;

  sampleRate = (double)options->sampleRate();

  #ifdef __arm__
  params.suggestedLatency = isInput ? Pa_GetDeviceInfo(params.device)->defaultHighInputLatency : 
                                      Pa_GetDeviceInfo(params.device)->defaultHighOutputLatency;
  #endif
}

} // namespace streampunk