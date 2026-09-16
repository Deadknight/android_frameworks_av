// ARDA_C2_GPU_I420_V2_0
#pragma once

#include <cstdint>
#include <memory>

class C2ConstGraphicBlock;

namespace android {

class ArdaGpuI420 {
public:
    ArdaGpuI420();
    ~ArdaGpuI420();

    bool requested() const;
    uint8_t* tryConvert(
            const C2ConstGraphicBlock& block,
            uint32_t width,
            uint32_t height);

    void markSubmitted(const void* base);
    void releaseIfFreed(const void* freed);
    bool slotHeld() const;
    const void* slotBase() const;
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace android
