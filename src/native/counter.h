#pragma once
#include <string_view>
// Ingress-stage display suppression, before native roster rendering/queueing.
bool BeginsRoundLoading(std::string_view xml);
bool IsRoundInitialization(std::string_view xml);
// Supported client's already-restored public wall geometry, not tile identities.
// The byte view starts at the native table state (object + virtual-base offset).
int RemainingFromNativeWall(std::string_view state);
// Counts public draw events only. Never inspects tile identities or hidden hands.
class WallCounter {
public:
    void set_type(unsigned type) { sanma_ = (type & 0x10) != 0; }
    void event(std::string_view xml);
    void restore(int remaining);
    void clear() { remaining_ = -1; active_ = false; }
    int remaining() const { return active_ ? remaining_ : -1; }
private:
    bool sanma_ = false, active_ = false;
    int remaining_ = -1;
};
