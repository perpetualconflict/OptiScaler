#pragma once

#include "ffx12/FfxRr12Provider.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RayReconstruction
{
enum class NormalSpace : uint8_t
{
    Unspecified,
    World,
    View,
};

enum class MatrixConversion : uint8_t
{
    Unspecified,
    DirectCopy,
    Transpose,
};

enum class DepthConvention : uint8_t
{
    Unspecified,
    Hardware,
    LinearSigned,
};

enum class MotionVectorDirection : uint8_t
{
    Unspecified,
    PreviousMinusCurrent,
    CurrentMinusPrevious,
};

enum class SignalAdapter : uint8_t
{
    Disabled,
    CompositeFloorSplit,
    CompositeAlbedoSplit,
    DirectResources,
};

struct Profile
{
    std::string id;
    std::vector<std::string> executables;
    bool enabled = false;
    bool validated = false;
    NormalSpace normalSpace = NormalSpace::Unspecified;
    MatrixConversion matrixConversion = MatrixConversion::Unspecified;
    DepthConvention depthConvention = DepthConvention::Unspecified;
    MotionVectorDirection motionVectorDirection = MotionVectorDirection::Unspecified;
    SignalAdapter signalAdapter = SignalAdapter::Disabled;
    FfxRr12::SignalMask signals = 0;
    FfxRr12::SignalMask checkerboardSignals = 0;
    float linearDepthMin = 0.0f;
    float linearDepthMax = 0.0f;
    std::string notes;

    bool IsDispatchable(std::string& reason) const;
};

class ProfileDatabase
{
  public:
    bool Load(const std::filesystem::path& path);
    const Profile* FindForExecutable(std::string executable) const;

    const std::vector<Profile>& Profiles() const { return _profiles; }
    const std::string& LastError() const { return _lastError; }

  private:
    std::vector<Profile> _profiles;
    std::string _lastError;
};
} // namespace RayReconstruction
