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

#ifndef PACONTEXT_H
#define PACONTEXT_H

#include "node_api.h"
#include <memory>
#include <mutex>
#include <string>

struct PaStreamParameters;

namespace streampunk {

class AudioOptions;
class Chunk;
class Chunks;

class PaContext {
public:
  PaContext(napi_env env, napi_value inOptions, napi_value outOptions);
  ~PaContext() {}

  enum class eStopFlag : uint8_t { WAIT = 0, ABORT = 1 };

  bool hasInput() { return mInOptions ? true : false; }
  bool hasOutput() { return mOutOptions ? true : false; }

  void start(napi_env env);
  void stop(eStopFlag flag);

  std::shared_ptr<Chunk> pullInChunk(uint32_t numBytes, bool &finished);
  void pushOutChunk(std::shared_ptr<Chunk> chunk);

  void checkStatus(uint32_t statusFlags);
  bool getErrStr(std::string& errStr, bool isInput);

  void quit();

  bool readPaBuffer(const void *srcBuf, uint32_t frameCount, double inTimestamp);
  bool fillPaBuffer(void *dstBuf, uint32_t frameCount);

  double getCurTime() const;
  double getInLatency() const { return mInLatency; }

private:
  std::shared_ptr<AudioOptions> mInOptions;
  std::shared_ptr<AudioOptions> mOutOptions;
  std::shared_ptr<Chunks> mInChunks;
  std::shared_ptr<Chunks> mOutChunks;
  void *mStream;
  double mInLatency;
  std::string mErrStr;
  std::mutex m;

  uint32_t fillBuffer(uint8_t *buf, uint32_t numBytes,
                      double &timeStamp,
                      std::shared_ptr<Chunks> chunks,
                      bool &finished, bool isInput);

  void setParams(napi_env env, bool isInput, 
                 std::shared_ptr<AudioOptions> options, 
                 PaStreamParameters &params, double &sampleRate);
};

} // namespace streampunk

#endif
