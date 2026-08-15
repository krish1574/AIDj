#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "AudioTypes.h"
#include "IDecoder.h"
#include "analysis/AnalysisTypes.h"

namespace aidj {

/**
 * Runs the full analysis chain over one track.
 *
 * Streams the file through every stage in a single decode pass. Decoding
 * dominates the cost, so analysing a 50 minute mix twice to compute two
 * different features would double the expensive part for nothing.
 *
 * Audio is mixed to mono at the analysis rate before anything else. Stereo
 * adds nothing to tempo, key or loudness estimation and doubles the work; the
 * only stage that would want stereo is vocal detection, which uses the
 * mid/side relationship - noted as a limitation rather than silently faked.
 */
class TrackAnalyzer {
 public:
  struct Progress {
    double decodedMs = 0.0;
    double totalMs = 0.0;
  };

  /**
   * Reports progress and allows cancellation. Returning false aborts the
   * analysis - the background worker uses this to stop when the user leaves,
   * the battery drops or the device gets hot, without waiting for a 50 minute
   * file to finish.
   */
  using ProgressCallback = std::function<bool(const Progress&)>;

  explicit TrackAnalyzer(AnalysisFormat format = {});

  /**
   * @return the analysis, or nullopt-equivalent (empty beat grid and zero
   *         duration) when the file could not be decoded or was cancelled.
   */
  TrackAnalysisResult analyse(IDecoder& decoder, const ProgressCallback& onProgress);

  /** True when the last analyse() call stopped because the callback said so. */
  bool wasCancelled() const { return cancelled_; }

 private:
  AnalysisFormat format_;
  bool cancelled_ = false;
};

}  // namespace aidj
