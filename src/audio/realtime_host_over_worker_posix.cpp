#include "audio/realtime_promotion.h"

namespace realtime {

// a plugin host refused real time asked again in a worker's place (realtime_promotion.h)
Outcome promoteHostOverWorker(int hostProcessId, long hostThreadId, int workerProcessId, long workerThreadId,
                              const PromotionSteps& hostSteps, const PromotionSteps& workerSteps,
                              const std::function<bool(long workerThreadId)>& dropWorker, Outcome& workerOutcome) {
    Outcome host = promoteThread(hostProcessId, hostThreadId, hostSteps);
    if (host.realtime || !dropWorker(workerThreadId)) return host;
    workerOutcome = Outcome();
    workerOutcome.whyNot = "its place went to a plugin host's audio thread (rtkit allows only so many real-time "
                           "threads per user)";
    host = promoteThread(hostProcessId, hostThreadId, hostSteps);
    if (!host.realtime) workerOutcome = promoteThread(workerProcessId, workerThreadId, workerSteps);
    return host;
}

}  // namespace realtime
